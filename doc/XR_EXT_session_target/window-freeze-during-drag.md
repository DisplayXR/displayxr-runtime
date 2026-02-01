# Window Freeze During Drag (sr_cube_openxr, Non-Extension Path)

**Status:** Unresolved
**Affected path:** `sr_cube_openxr` (Monado-owned window, no `XR_EXT_session_target`)
**Working path:** `sr_cube_openxr_ext` (app-owned window, uses `XR_EXT_session_target`)
**Date:** 2026-02-01

---

## 1. Problem Description

When running `sr_cube_openxr` in windowed mode (F11 to exit fullscreen), dragging the window causes the 3D animation to **completely freeze** for the duration of the drag. The scene only resumes rendering when the user releases the mouse button. Phase snapping (lenticular alignment) also does not work during the drag.

In contrast, `sr_cube_openxr_ext` handles drag/resize smoothly: the animation continues, the SR weaver re-interlaces each frame, and phase snapping keeps the lenticular alignment correct as the window moves.

---

## 2. Why sr_cube_openxr_ext Works

In the extension path, the **application owns the window** and its WndProc. The app directly renders an OpenXR frame from inside `WM_PAINT`:

```cpp
// test_apps/sr_cube_openxr_ext/main.cpp:52-82
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ENTERSIZEMOVE:
        g_inSizeMove = true;
        InvalidateRect(hwnd, nullptr, FALSE);  // Kick off first WM_PAINT
        return 0;

    case WM_EXITSIZEMOVE:
        g_inSizeMove = false;
        return 0;

    case WM_PAINT:
        if (g_inSizeMove && g_renderState != nullptr) {
            RenderOneFrame(*g_renderState);     // Full xrWaitFrame/BeginFrame/EndFrame
            InvalidateRect(hwnd, nullptr, FALSE); // Self-invalidate → continuous loop
            return 0;                             // No BeginPaint/EndPaint → stays dirty
        }
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
```

Key design points:
- **No `BeginPaint`/`EndPaint`** — the window stays invalidated, so Windows keeps sending `WM_PAINT` inside the modal loop
- **`RenderOneFrame()` runs a complete OpenXR frame** — `xrWaitFrame`, `xrBeginFrame`, render eyes, `xrEndFrame`
- **Self-`InvalidateRect`** after rendering keeps the loop going
- The SR SDK subclasses this WndProc (for phase snapping), but **forwards `WM_PAINT`** to the app's handler via `CallWindowProc`

---

## 3. Why sr_cube_openxr Freezes

In the non-extension path, **Monado creates its own window** (`comp_d3d11_window.cpp`) and the app's message loop processes messages for both the app's control window and the Monado window.

### The Modal Loop Problem

When a user drags a Win32 window, `DefWindowProc` enters a **modal message loop** inside `DispatchMessage` that blocks the calling thread until the drag ends. The app's normal render loop (`PeekMessage` → `BeginFrame` → render → `EndFrame`) cannot run.

The only way to render during this modal loop is to handle messages that Windows dispatches inside it (`WM_PAINT`, `WM_TIMER`, etc.).

### The WndProc Subclass Problem

The SR SDK automatically subclasses the window's WndProc when the weaver is created (see Section 5.7 of `XR_EXT_session_target.md`). The subclass chain becomes:

```
Message → SR SDK WndProc (outermost) → Monado wnd_proc (original)
```

Our attempts to use `WM_PAINT` for repaint during drag **failed because `WM_PAINT` never reaches our `wnd_proc`**. The logs prove this conclusively:

```
[23:48:06.802] WM_ENTERSIZEMOVE — entering modal drag/resize loop
                (4.37 seconds — ZERO WM_PAINT messages)
[23:48:11.173] WM_EXITSIZEMOVE — left modal drag/resize loop
```

`WM_ENTERSIZEMOVE` and `WM_EXITSIZEMOVE` reach our handler (the SR SDK forwards them via `CallWindowProc`), but `WM_PAINT` does not.

### Possible Explanations

1. **SR SDK consumes WM_PAINT**: The SR SDK's subclassed WndProc may call `BeginPaint`/`EndPaint` internally, which validates the dirty region and prevents the message from propagating to our handler.

2. **DXGI swapchain WndProc subclass**: When `IDXGIFactory::CreateSwapChain` binds a swapchain to a window, DXGI may install its own WndProc subclass that handles `WM_PAINT` (and `WM_SIZE`) for internal backbuffer management.

3. **Multiple subclass layers**: Both the SR SDK and DXGI may subclass the window, creating a chain where `WM_PAINT` is consumed before reaching our handler:
   ```
   Message → DXGI WndProc → SR SDK WndProc → Monado wnd_proc
   ```

### Why This Doesn't Affect sr_cube_openxr_ext

In the extension path, the app's WndProc is registered **before** the SR SDK and DXGI subclass it. Even though the message goes through the SR SDK and DXGI first, the `WM_PAINT` handler in the app's WndProc is called via `CallWindowProc`. The app's handler intentionally does NOT call `BeginPaint`/`EndPaint` (it returns 0 directly), so the region stays dirty and the loop continues.

The difference may be that `WM_PAINT` IS forwarded by `CallWindowProc` in both cases, but in the Monado window case, something between the SR SDK and our handler validates the region (e.g., the SR SDK calls `BeginPaint`/`EndPaint` for its own internal rendering, then calls `CallWindowProc`, but by then the dirty region is empty so our handler's `w->in_size_move` check passes but Windows doesn't generate another `WM_PAINT` because the region is already clean). Alternatively, a DXGI subclass may be the one consuming it.

---

## 4. Attempted Fixes

### Attempt 1: Eliminate Separate Window Thread (commit a518f0c)

**Hypothesis:** The original Monado window was on a separate thread, causing cross-thread WndProc subclassing to fail.

**Change:** Rewrote `comp_d3d11_window.cpp` to create the window synchronously on the app thread, eliminating `os_thread_helper` and the window thread entirely. Added `WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE`/`WM_PAINT` handling and a repaint callback.

**Result:** Thread elimination worked correctly. Window now created on app thread. But WM_PAINT never fired during drag.

### Attempt 2: Full Resize Suppression During Drag (commit a94dba0)

**Hypothesis:** The constant per-pixel renderer resize (819x512 → 818x511 → 820x512 every frame) during drag caused stutter.

**Change:** Guarded the entire resize block in `begin_frame` with `!in_size_move`.

**Result:** REGRESSION — animation stopped entirely during drag. Suppressing swapchain resize broke the render pipeline during the modal loop.

### Attempt 3: Partial Resize Suppression (commit ff157d0)

**Hypothesis:** Only the renderer (stereo texture) resize is expensive; the swapchain target resize should continue.

**Change:** Only suppress renderer resize during `in_size_move`; keep swapchain target resize active.

**Result:** Fixed the regression from Attempt 2, but animation still froze during drag (WM_PAINT was not firing).

### Attempt 4: Swapchain Resize in Repaint Callback (commit 852585f)

**Hypothesis:** The repaint callback needs to resize the swapchain target since `begin_frame` doesn't run during the modal loop.

**Change:** Added `GetClientRect` → `comp_d3d11_target_resize` to `d3d11_compositor_repaint`. Moved `InvalidateRect` from WM_PAINT to WM_SIZE.

**Result:** Moving `InvalidateRect` to `WM_SIZE` broke move drags (WM_SIZE never fires during title-bar move, only during border resize). Animation still frozen.

### Attempt 5: Restore InvalidateRect in WM_ENTERSIZEMOVE + WM_PAINT Self-Invalidation (commit 487962e)

**Hypothesis:** InvalidateRect needs to be in WM_ENTERSIZEMOVE (for initial trigger) and WM_PAINT (for continuous loop).

**Change:** Added `InvalidateRect` to `WM_ENTERSIZEMOVE` and self-`InvalidateRect` in `WM_PAINT` after the repaint callback. Added diagnostic `U_LOG_W` logging.

**Result:** Logs proved `WM_PAINT` never reaches our handler (4+ seconds of drag with zero WM_PAINT messages). Confirmed the issue is in the WndProc subclass chain, not our InvalidateRect logic.

### Attempt 6: WM_TIMER Instead of WM_PAINT (commit abf860a)

**Hypothesis:** Since `WM_PAINT` is consumed by the SR SDK/DXGI subclass chain, use `WM_TIMER` which can't be intercepted by unknown timer IDs.

**Change:** Replaced WM_PAINT repaint loop with `SetTimer`/`KillTimer`:
```cpp
case WM_ENTERSIZEMOVE:
    SetTimer(hWnd, REPAINT_TIMER_ID, USER_TIMER_MINIMUM, NULL);
    return 0;
case WM_EXITSIZEMOVE:
    KillTimer(hWnd, REPAINT_TIMER_ID);
    return 0;
case WM_TIMER:
    if (wParam == REPAINT_TIMER_ID && w->repaint_callback != NULL)
        w->repaint_callback(w->repaint_userdata);
    return 0;
```

**Analysis:** Both Claude and Gemini analysis confirmed WM_TIMER is reliable during DefWindowProc modal loops and can't be consumed by WndProc subclasses. `USER_TIMER_MINIMUM` (~10-15ms) fires fast enough for VSync-paced rendering.

**Result:** FAILED. Tested in build containing commit `1ad830cd2` (which includes `abf860a`). Animation still freezes during drag. No "WM_TIMER fired" or "repaint timer" messages appear in logs. This means WM_TIMER is also NOT reaching our `wnd_proc` during the modal loop — the SR SDK or DXGI WndProc subclass chain consumes or blocks ALL messages except WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE.

### Attempt 7: Diagnostic Logging of ALL Messages During Drag (pending)

**Hypothesis:** We need to determine exactly which messages reach our `wnd_proc` during drag, and confirm the WndProc subclass chain.

**Change:** Added logging before the switch statement that logs every message received during `in_size_move`, plus logging the current WndProc address vs our `wnd_proc` address in WM_ENTERSIZEMOVE to confirm subclassing.

**Result:** Pending test. This will reveal whether ANY messages reach us during drag, or only WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE (which the SR SDK explicitly forwards).

---

## 5. Repaint Callback Architecture

When WM_TIMER fires during drag, the `d3d11_compositor_repaint` callback executes:

```
WM_TIMER
  → d3d11_compositor_repaint()
    → std::try_to_lock (mutex)           // Non-blocking, should always succeed during modal loop
    → GetClientRect → comp_d3d11_target_resize  // Resize swapchain if window changed
    → comp_d3d11_target_acquire           // Acquire backbuffer
    → leiasr_d3d11_set_input_texture      // Set stereo texture (from last xrEndFrame)
    → leiasr_d3d11_weave()                // Re-interlace with fresh eye tracking
    → comp_d3d11_target_present(1)        // Present with VSync
```

Important limitations:
- **Stale scene**: The stereo texture is from the last `xrEndFrame` call (before drag started). Eye tracking is fresh, but the 3D scene itself doesn't update.
- **No renderer resize**: The stereo render texture is not resized during drag to avoid expensive reallocation every pixel. DXGI stretches if needed.

---

## 6. Fundamental Architecture Difference

The root cause is an architectural asymmetry between the two paths:

| Aspect | sr_cube_openxr_ext (works) | sr_cube_openxr (broken) |
|--------|----------------------------|-------------------------|
| Window owner | App | Monado runtime |
| WndProc owner | App | Monado runtime |
| WM_PAINT render | Full OpenXR frame (`RenderOneFrame`) | Repaint callback (re-weave only) |
| WM_PAINT delivery | Reaches app's handler | **Never reaches Monado's handler** |
| Message pump | App controls it directly | App's PeekMessage dispatches to Monado window |

In `sr_cube_openxr_ext`, the app runs a **complete OpenXR frame** from inside WM_PAINT. The entire `xrWaitFrame` → `xrBeginFrame` → render → `xrEndFrame` pipeline runs, producing a fresh scene.

In `sr_cube_openxr`, the repaint callback can only **re-weave the last rendered frame** because it operates below the OpenXR layer. It cannot call `xrBeginFrame`/`xrEndFrame` — those are called by the app, which is blocked in the modal loop.

---

## 7. Diagnostic Evidence

### Log Analysis (SRMonado_2026-01-31_23-47-59.log)

Three drag episodes, all showing the same pattern:

```
[23:48:06.802] WM_ENTERSIZEMOVE — entering modal drag/resize loop
                <--- 4.37 seconds of silence --->
[23:48:11.173] WM_EXITSIZEMOVE — left modal drag/resize loop

[23:48:13.628] WM_ENTERSIZEMOVE — entering modal drag/resize loop
                <--- 1.30 seconds of silence --->
[23:48:14.925] WM_EXITSIZEMOVE — left modal drag/resize loop

[23:48:17.656] WM_ENTERSIZEMOVE — entering modal drag/resize loop
                <--- 1.40 seconds of silence --->
[23:48:19.056] WM_EXITSIZEMOVE — left modal drag/resize loop
```

Zero WM_PAINT log messages. Zero repaint callback log messages. The window content is completely frozen during each drag.

Normal rendering resumes immediately after WM_EXITSIZEMOVE (Kooima FOV calculations and weaver commits appear in the log within milliseconds of the drag ending).

---

## 8. Possible Next Steps

### A. Confirm WM_TIMER Fires (Priority: High)

The WM_TIMER approach (Attempt 6) has not been tested with the latest build. Test and check logs for:
- `WM_ENTERSIZEMOVE — starting repaint timer`
- `D3D11 repaint: Executing repaint` (from the callback)
- `WM_EXITSIZEMOVE — killed repaint timer`

### B. Debug WndProc Subclass Chain (Priority: Medium)

Add logging **before** the `GetPropW` / switch statement in `wnd_proc` to trace ALL messages received during drag:
```cpp
if (message == WM_PAINT || message == WM_TIMER)
    U_LOG_W("wnd_proc received message %u (WM_PAINT=%u WM_TIMER=%u)", message, WM_PAINT, WM_TIMER);
```
This would confirm whether the messages reach our wnd_proc at all.

### C. Investigate DXGI/SR SDK Subclass Order (Priority: Medium)

Use `GetWindowLongPtr(hWnd, GWLP_WNDPROC)` at various points to trace the subclass chain:
- After `CreateWindowExW` (should be our `wnd_proc`)
- After `CreateDXGISwapChain` (may be DXGI's handler)
- After `leiasr_d3d11_create` (should be SR SDK's handler)

This reveals the full subclass chain and helps identify who consumes `WM_PAINT`.

### D. App-Side WM_PAINT Approach (Priority: Low)

Modify `sr_cube_openxr/main.cpp` to subclass the Monado window from the app side and handle WM_PAINT directly (like `sr_cube_openxr_ext` does). This would bypass the runtime-side WndProc subclass issue entirely.

### E. Accept Limitation and Document (Priority: Fallback)

If the modal-loop rendering cannot be made to work for the Monado-owned window, document the limitation: apps that need smooth drag/resize behavior should use `XR_EXT_session_target` to own their window. This is already the recommended path for desktop 3D display applications.

---

## 9. Files Reference

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | Monado window creation, WndProc, WM_TIMER repaint |
| `src/xrt/compositor/d3d11/comp_d3d11_window.h` | Window API (create, destroy, pump_messages, set_repaint_callback) |
| `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp` | `d3d11_compositor_repaint` callback, `begin_frame` resize logic |
| `test_apps/sr_cube_openxr_ext/main.cpp` | Working reference: app-owned window with WM_PAINT rendering |
| `test_apps/sr_cube_openxr/main.cpp` | Non-extension app (message pump, control window) |
| `src/xrt/drivers/leiasr/leiasr_d3d11.cpp` | SR SDK D3D11 weaver (subclasses WndProc for phase snapping) |
