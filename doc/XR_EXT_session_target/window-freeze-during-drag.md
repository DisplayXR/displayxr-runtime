# Window Freeze During Drag (sr_cube_openxr, Non-Extension Path)

**Status:** Resolved (dedicated window thread)
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

### Attempt 7: Diagnostic Logging of ALL Messages During Drag (commit cbda410)

**Hypothesis:** We need to determine exactly which messages reach our `wnd_proc` during drag, and confirm the WndProc subclass chain.

**Change:** Added logging before the switch statement that logs every message received during `in_size_move`, plus logging the current WndProc address vs our `wnd_proc` address in WM_ENTERSIZEMOVE to confirm subclassing.

**Result: BREAKTHROUGH** — proved the earlier diagnosis was wrong. Logs from build `cbda410` show:

1. **WndProc IS subclassed**: `Current WndProc=00000000FFFF095D, our wnd_proc=00007FFE75471740 (subclassed=YES)`
2. **Both WM_PAINT (0x000F) AND WM_TIMER (0x0113) DO reach our wnd_proc during drag**
3. **The repaint callback IS being invoked** — ~150+ times over a 5.5-second drag
4. **No "Mutex locked" messages** — the mutex lock succeeds every time

Messages decoded during drag:
- `0x0216` = WM_MOVING, `0x0046` = WM_WINDOWPOSCHANGING, `0x0024` = WM_GETMINMAXINFO
- `0x0083` = WM_NCCALCSIZE, `0x0085` = WM_NCPAINT
- `0x0047` = WM_WINDOWPOSCHANGED, `0x0003` = WM_MOVE, `0x0005` = WM_SIZE
- `0x000F` = **WM_PAINT**, `0x0113` = **WM_TIMER**

**The earlier conclusion that "WM_PAINT/WM_TIMER never reach us" was wrong.** The previous builds lacked diagnostic logging; the "Executing repaint" log was at `U_LOG_I` (INFO) which was filtered out by log level. The repaint callback has been running all along since Attempt 6, but its output is not producing visible results.

**New diagnosis:** The problem is NOT message delivery — it's that `d3d11_compositor_repaint()` runs but doesn't produce visible output. Either `comp_d3d11_target_acquire` fails silently, the weave doesn't draw to the backbuffer, or `Present` doesn't flip.

### Attempt 8: WARN-Level Repaint Callback Diagnostics (pending)

**Hypothesis:** The repaint callback runs but one of its internal steps fails silently (acquire, weave, or present).

**Change:** Added `U_LOG_W` logging after each step in `d3d11_compositor_repaint()`:
- After `comp_d3d11_target_resize` (if triggered)
- After `comp_d3d11_target_acquire` (log failure with return code)
- Before `leiasr_d3d11_weave` (log dimensions)
- Before `comp_d3d11_target_present` (log target_index)

**Result:** Pending test.

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

## 8. How SRHydra Solves This (Reference Implementation)

SRHydra is another OpenXR runtime that creates its own window and integrates with the SR SDK. It does **not** suffer from the drag freeze problem. Analysis of its architecture reveals a fundamentally different approach.

### Architecture: Dedicated Window Thread + Message Deferral

SRHydra uses a **dedicated window thread** that owns the Win32 window and message pump, while the **main/render thread** runs the OpenXR render loop independently.

```
┌─────────────────────────┐     ┌──────────────────────────┐
│     Window Thread        │     │     Main/Render Thread    │
│                          │     │                           │
│  CreateWindowEx()        │     │  while (running) {        │
│  while (running) {       │     │    compositor->Update()   │
│    PeekMessageA(...)     │     │      → process deferred   │
│    if (msg) {            │     │        messages from queue │
│      TranslateMessage()  │     │    xrWaitFrame()          │
│      DispatchMessage()   │     │    xrBeginFrame()         │
│    }                     │     │    render(...)            │
│  }                       │     │    xrEndFrame()           │
│                          │     │  }                        │
│  ┌─ WndProc ──────────┐ │     │                           │
│  │ WM_SIZE, WM_MOVE...│─┼──→ thread-safe queue ──→ deferred processing
│  │ → defer to queue    │ │     │                           │
│  └─────────────────────┘ │     │                           │
└─────────────────────────┘     └──────────────────────────┘
```

### Key Design Points

1. **Non-blocking message pump**: The window thread uses `PeekMessageA` (non-blocking), not `GetMessage` (blocking). When `DefWindowProc` enters a modal drag loop, it blocks only the window thread — the render thread continues unaffected.

2. **Message deferral**: Size-sensitive messages (like `WM_SIZE`) are captured in the WndProc and queued to a thread-safe structure. The render thread processes these deferred messages in its `Update()` call, allowing it to react to window changes without being blocked.

3. **Independent render loop**: The main thread's render loop (`xrWaitFrame` → `xrBeginFrame` → render → `xrEndFrame`) runs continuously and independently of the window thread. It never calls `DispatchMessage` and is never blocked by a modal loop.

4. **No WM_PAINT/WM_TIMER tricks**: Because rendering happens on a separate thread, there is no need to render from inside WM_PAINT or use timer callbacks. The render thread simply keeps rendering.

5. **D3D11 immediate context (no deferred context)**: SRHydra does NOT use a D3D11 deferred context. It uses the immediate context from the render thread, with fence-based synchronization where needed.

### Why It Works During Drag

When the user drags the SRHydra window:
1. `DefWindowProc` enters a modal loop on the **window thread**
2. The window thread is blocked — but this only affects message processing
3. The **render thread** keeps running its `xrWaitFrame`/`BeginFrame`/`EndFrame` loop
4. The 3D animation continues with fresh frames
5. SR SDK phase snapping events (`WM_WINDOWPOSCHANGING`) are handled on the window thread, which can still dispatch messages within the modal loop

### Why This Differs From Our Approach

We eliminated our separate window thread (Attempt 1) because cross-thread WndProc subclassing broke SR SDK phase snapping. The SR SDK calls `SetWindowLongPtr` from the app thread, but the window was on a different thread.

SRHydra avoids this problem because:
- Its window thread is a well-defined, long-lived thread
- The SR SDK subclassing likely happens from the window thread itself (or the SR SDK handles cross-thread subclassing correctly in SRHydra's initialization order)
- The render thread never touches the WndProc chain

### Applicability to Our Case

Adopting the SRHydra approach would require:

1. **Re-introduce a dedicated window thread** — but this time, ensure the SR SDK subclasses the WndProc **from the window thread** (or after the window is fully created and visible on that thread).

2. **Add a message deferral queue** — thread-safe queue for `WM_SIZE`, `WM_MOVE`, and other size-sensitive messages that the render thread needs to act on.

3. **Enable D3D11 multithread protection** — since two threads would access D3D11 resources (render thread for drawing, window thread for SR SDK weave during phase snap). Use `ID3D10Multithread::SetMultithreadProtected(TRUE)`.

4. **Ensure SR SDK phase snapping works cross-thread** — this was the original reason for eliminating the window thread. Would need to verify that creating the SR weaver on the render thread while the window lives on a different thread doesn't break `SetWindowLongPtr` subclassing.

This is a significant architectural change but is the proven approach used by a shipping OpenXR runtime with SR SDK integration.

---

## 9. Resolution: Dedicated Window Thread

### Root Cause (Proven)

DWM stalls the DXGI presentation pipeline when the window-owning thread is blocked in a modal loop. `Present(S_OK)` succeeds but DWM refuses to flip buffers. No single-threaded approach (WM_TIMER, WM_PAINT, `Present(0)`) can fix this. The window's owning thread must NOT be the thread calling `Present`.

### Solution Implemented

Moved the window to a **dedicated thread** that owns the HWND and message pump. The compositor/app thread continues rendering independently — `Present` is called from a non-blocked thread, so DWM flips buffers normally.

```
Window Thread                      Compositor Thread (= App Thread)
─────────────                      ────────────────────────────────
CreateWindowEx(HWND)               ID3D11Device + IDXGISwapChain
GetMessage / DispatchMessage       while (running) {
                                     read window dims (atomic)
┌─ WndProc ─────────────────┐       xrWaitFrame / BeginFrame
│ WM_WINDOWPOSCHANGING       │       render stereo
│   → phase snap (SR SDK)    │       xrEndFrame → weave → Present
│ WM_SIZE → update atomics  ─┼──→  }
│ WM_ENTERSIZEMOVE           │     (never blocked by modal loop)
│   → modal loop (BLOCKS     │
│     only THIS thread)      │
└────────────────────────────┘
```

### Why Single-Threaded Approaches Failed

All single-threaded approaches (Attempts 1–8) failed for the same fundamental reason: even though the repaint callback ran successfully during drag (Attempt 7 proved WM_TIMER fires and the callback executes), **DWM will not flip DXGI buffers when the window-owning thread is in a modal state**. `Present` returns `S_OK` but produces no visible output.

### Changes Made

1. **`comp_d3d11_window.cpp`** — Rewrote to use a dedicated window thread:
   - Window created via `CreateThread` → `window_thread_func`
   - Thread runs its own `GetMessage` loop
   - State communicated via `volatile LONG` + `InterlockedExchange`
   - `pump_messages` and `set_repaint_callback` are now no-ops
   - Removed WM_TIMER repaint mechanism entirely

2. **`comp_d3d11_compositor.cpp`** — Simplified:
   - Removed `d3d11_compositor_repaint` function entirely
   - Removed repaint callback registration/clearing
   - Added `ID3D10Multithread::SetMultithreadProtected(TRUE)` for cross-thread safety

### Thread Safety

| Resource | Accessed By | Protection |
|----------|-------------|------------|
| HWND | Both threads | Created on window thread, read-only after creation |
| Window dimensions | Window thread writes, compositor reads | `volatile LONG` + `InterlockedExchange` |
| `should_exit`, `in_size_move` | Window thread writes, compositor reads | `volatile LONG` + `InterlockedExchange` |
| D3D11 device/context | Compositor thread primarily | `ID3D10Multithread::SetMultithreadProtected(TRUE)` |
| SR weaver | Compositor thread (weave) | Not accessed from window thread |
| SR WndProc data | Window thread only | No protection needed |

### SR SDK Threading (Confirmed Safe)

- `weaverWndProc` handles `WM_WINDOWPOSCHANGING` (SnapToPhase), `WM_ENTERSIZEMOVE`, `WM_EXITSIZEMOVE` — **no D3D11 ops** in WndProc
- `SetWindowLongPtr` (called during `CreateDX11Weaver` on compositor thread) works cross-thread — Windows processes it via `SendMessage` to the window thread
- Phase snapping runs on the window thread where messages are dispatched — correct behavior

---

## 10. Files Reference

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | Dedicated window thread, WndProc, atomic state |
| `src/xrt/compositor/d3d11/comp_d3d11_window.h` | Window API (create, destroy, get_dimensions, etc.) |
| `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp` | D3D11 compositor, multithread protection, `begin_frame` resize |
| `test_apps/sr_cube_openxr_ext/main.cpp` | Working reference: app-owned window with WM_PAINT rendering |
| `test_apps/sr_cube_openxr/main.cpp` | Non-extension app (message pump, control window) |
| `src/xrt/drivers/leiasr/leiasr_d3d11.cpp` | SR SDK D3D11 weaver (subclasses WndProc for phase snapping) |
