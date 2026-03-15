# Rendering During Window Drag: WM_PAINT Trick vs. Render Thread

## Two Scenarios: App-Owned vs. Runtime-Owned Window

This document covers **window drag handling** for the D3D11 native compositor. There are two distinct scenarios depending on who owns the window:

| Scenario | Window Owner | Extension | Example App | Section |
|----------|-------------|-----------|-------------|---------|
| **App-owned window** | Application creates HWND, passes to runtime | `XR_EXT_win32_window_binding` | `cube_ext_d3d11` | [WM_PAINT Trick](#solution-the-wm_paint-trick) |
| **Runtime-owned window** | Monado creates HWND on dedicated thread | None (standard OpenXR) | `cube_d3d11`, Blender | [Cross-Thread Sync](#monados-dedicated-window-thread-cross-thread-paint-synchronization) |

**Key difference:**
- **App-owned**: App controls the message pump. Rendering and window messages are on the **same thread**. The app can render inside `WM_PAINT` directly.
- **Runtime-owned**: Monado's window thread is **separate** from the compositor thread. Cross-thread synchronization is required to keep the window position stable during render.

---

## The Problem

When a user drags or resizes a Win32 window, Windows enters a **modal message loop** inside `DefWindowProc`. The application's normal frame loop stops executing entirely — Windows does not return control until the user releases the mouse button.

For a 3D light field display this is unacceptable: no frames are rendered, eye tracking stops updating, and the 3D effect breaks.

---

## Background: How Win32 Message Pumping Works

A Win32 application processes **messages** — small events that Windows puts into a queue. The application runs a loop that pulls messages out and handles them:

```
Normal game loop:
┌─────────────────────────────────────────┐
│  while (running) {                      │
│      PeekMessage(...)  ← pull messages  │
│      handle input                       │
│      xrWaitFrame()                      │
│      xrBeginFrame()                     │
│      render scene                       │
│      xrEndFrame()                       │
│  }                                      │
└─────────────────────────────────────────┘
```

This is "pumping messages" — the application is the pump, pulling messages from the queue and processing them.

**During a window drag**, `DefWindowProc(WM_NCLBUTTONDOWN)` enters its own internal loop that the application cannot see or control:

```
Windows' internal drag loop:
┌─────────────────────────────────────────┐
│  while (mouse_button_held) {            │
│      GetMessage(...)                    │
│      move the window                    │
│      dispatch WM_PAINT if invalidated   │
│      dispatch WM_SIZE if resizing       │
│  }                                      │
│  // Application code resumes only HERE  │
└─────────────────────────────────────────┘
```

The application's game loop is **blocked** at `DefWindowProc()`, waiting for it to return. Calls to `xrWaitFrame` / `xrEndFrame` never happen.

---

## App-Owned Window: The WM_PAINT Trick

> **Applies to:** Apps using `XR_EXT_win32_window_binding` that create their own HWND and pass it to the runtime (e.g., `cube_ext_d3d11`).

Even though Windows hijacked the message loop, it still **dispatches `WM_PAINT` messages** to the application's `WndProc`. The trick exploits this to sneak render frames into the modal loop:

1. When dragging starts (`WM_ENTERSIZEMOVE`), call `InvalidateRect()` — this tells Windows "my window needs repainting."
2. Windows' internal loop sees the invalidation and sends `WM_PAINT`.
3. In the `WM_PAINT` handler, **run one full OpenXR frame** (wait, begin, render, end).
4. **Do not call `BeginPaint`/`EndPaint`** — this means the window stays "invalid."
5. Windows sees it is still invalid and sends another `WM_PAINT`.
6. The cycle repeats — continuous rendering inside the modal loop.

```
Windows' drag loop (with the trick):
┌────────────────────────────────────────────────┐
│  while (mouse_button_held) {                   │
│      move the window                           │
│      window is invalid? → dispatch WM_PAINT    │
│         └→ App: render one OpenXR frame        │
│            └→ don't call EndPaint              │
│               └→ window stays invalid          │
│                  └→ Windows sends WM_PAINT     │
│                     again next iteration ←─────┘
└────────────────────────────────────────────────┘
```

The application tricks Windows into calling the render function repeatedly by never acknowledging that painting is "done."

### Implementation

```cpp
static bool g_isMoving = false;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ENTERSIZEMOVE:
        g_isMoving = true;
        InvalidateRect(hWnd, nullptr, false);  // Force first WM_PAINT
        return 0;

    case WM_EXITSIZEMOVE:
        g_isMoving = false;
        return 0;

    case WM_PAINT:
        if (g_isMoving) {
            run_one_openxr_frame();  // Full xrWaitFrame → xrEndFrame cycle
            // No BeginPaint/EndPaint — window stays invalid
            // Windows keeps sending WM_PAINT — continuous rendering
            return 0;
        }
        break;  // Fall through to DefWindowProc when not dragging
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
```

---

## Why Not Use a Separate Render Thread?

The seemingly obvious alternative: put rendering on a second thread so it keeps running while the main thread's message pump is blocked inside the modal drag loop.

**This does not work because of D3D11's threading model.**

The D3D11 **immediate device context** — the object used to issue all GPU commands — is not thread-safe. During a drag, both threads touch it concurrently:

```
Thread A (main / message pump):         Thread B (render):
┌──────────────────────────┐           ┌──────────────────────────┐
│ DefWindowProc()          │           │ xrWaitFrame()            │
│   ↓                      │           │ xrBeginFrame()           │
│ Windows internally       │           │ render scene             │
│ touches DXGI / D3D11     │           │ leiasr_weave() ← uses   │
│ (swapchain resize,       │           │   D3D11 device context   │
│  DWM recomposition)      │           │ xrEndFrame()             │
│          ↑               │           │          ↑               │
│          └──────── SAME D3D11 CONTEXT ─────────┘               │
│                                                                 │
│                     CRASH / CORRUPTION                          │
└─────────────────────────────────────────────────────────────────┘
```

**Thread B** calls `leiasr_weave()` which issues D3D11 draw calls through the immediate context. **Thread A** is inside `DefWindowProc`, which internally triggers DXGI operations (swapchain management, DWM recomposition) that also use the same D3D11 context.

The worst case: when the user **releases the mouse button**, Windows calls `ReleaseCapture()` which sends synchronous messages that re-enter the D3D11 context from thread A while thread B is mid-render.

### What about `ID3D11Multithread::SetMultithreadProtected(TRUE)`?

D3D11's built-in thread-safety mutex does not help here. Even if D3D11 calls are serialized, the **SR SDK weaver's internal state** has no synchronization of its own. The weaver maintains render state (shader bindings, render targets, intermediate buffers) that would be corrupted by concurrent access from two threads.

---

## Summary

| Approach | How it works | Result |
|----------|-------------|--------|
| **Separate render thread** | Render on thread B, message pump on thread A | D3D11 context accessed from both threads simultaneously — crashes and corruption |
| **WM_PAINT trick** | Sneak frames into Windows' own modal loop via perpetual invalidation | Single thread, no data races, correct by design |

The WM_PAINT trick is the correct approach: one thread, one D3D11 context, no races. It is a well-known Win32 pattern applied to keep OpenXR frames flowing during window drag and resize operations.

---

## Runtime-Owned Window: Cross-Thread Paint Synchronization

> **Applies to:** Apps that do NOT use `XR_EXT_win32_window_binding`. Monado creates and owns the window (e.g., `cube_d3d11`, Blender).

When apps use Monado without `XR_EXT_win32_window_binding` (no app-provided HWND), Monado creates its own window on a **dedicated thread**. This separates the window thread from the compositor thread, which introduces a different challenge.

### The Architecture

```
Window Thread                          Compositor Thread
────────────────                       ──────────────────
HWND owner                             D3D11 rendering
GetMessage loop                        xrWaitFrame/xrEndFrame
WM_PAINT handler                       SR weaver + Present(1)
```

The window thread runs its own message loop, so the compositor thread is **not blocked** during drag — it continues calling `Present(1)`. However, this creates a **3D phase sync problem**.

### The Phase Sync Problem

The SR SDK weaver reads the window position **at weave time** via `GetClientRect()` + `ClientToScreen()`. During drag, the window position changes rapidly. If the compositor renders at position P₁ but DWM composites the frame when the window has moved to P₂, the interlacing pattern is wrong — the 3D effect breaks.

```
Timeline during drag (async rendering — BROKEN 3D):
───────────────────────────────────────────────────
Window:     P₁ ──────→ P₂ ──────→ P₃ ──────→ P₄
Weaver:     weave(P₁)      weave(P₂)      weave(P₃)
DWM shows:        ↓              ↓              ↓
              frame@P₁       frame@P₂       frame@P₃
              shown@P₂       shown@P₃       shown@P₄
                 ↑              ↑              ↑
              WRONG 3D       WRONG 3D       WRONG 3D
```

### The Solution: WM_PAINT-Synchronized Rendering

We replicate the WM_PAINT trick's key property — **the window position is stable during render** — using cross-thread event synchronization:

```
Window Thread (modal drag loop)          Compositor Thread
────────────────────────────────         ──────────────────
WM_WINDOWPOSCHANGING → phase snap
WM_PAINT fires:
  SetEvent(paint_requested) ──────────→ xrWaitFrame unblocks
  WaitForSingleObject(paint_done)        render → weave → Present(1)
    (modal loop PAUSED — no             SetEvent(paint_done) ──────→
     position changes!)
  ←────────────────────────────────────
  InvalidateRect (request next paint)
  return 0
WM_WINDOWPOSCHANGING → phase snap
WM_PAINT fires again...
```

The key insight: **WM_PAINT blocks the modal drag loop**. While `WaitForSingleObject(paint_done)` waits, no `WM_WINDOWPOSCHANGING` or `WM_MOVE` messages are processed. The window position is frozen until the compositor signals completion.

### Implementation

**Window thread (comp_d3d11_window.cpp):**

```cpp
// Events for cross-thread synchronization
HANDLE paint_requested_event;  // Auto-reset
HANDLE paint_done_event;       // Auto-reset

case WM_ENTERSIZEMOVE:
    InterlockedExchange(&w->in_size_move, TRUE);
    InvalidateRect(hWnd, NULL, FALSE);  // Kick off first WM_PAINT
    return 0;

case WM_EXITSIZEMOVE:
    InterlockedExchange(&w->in_size_move, FALSE);
    SetEvent(w->paint_requested_event);  // Unblock compositor if waiting
    return 0;

case WM_PAINT:
    if (InterlockedCompareExchange(&w->in_size_move, 0, 0)) {
        // During drag: trigger compositor render, wait for completion.
        // The modal loop is paused while we wait, so the window position
        // is stable between weave() and Present().
        SetEvent(w->paint_requested_event);
        WaitForSingleObject(w->paint_done_event, 100);
        InvalidateRect(hWnd, NULL, FALSE);  // Request next WM_PAINT
        return 0;
    }
    ValidateRect(hWnd, NULL);
    break;
```

**Compositor thread (comp_d3d11_compositor.cpp):**

```cpp
// In wait_frame, before rendering:
if (c->owns_window && comp_d3d11_window_is_in_size_move(c->own_window)) {
    comp_d3d11_window_wait_for_paint(c->own_window);
}

// In layer_commit, after Present(1):
if (c->owns_window && c->own_window != nullptr) {
    comp_d3d11_window_signal_paint_done(c->own_window);
}
```

### Result

| Aspect | Before (async) | After (WM_PAINT sync) |
|--------|---------------|----------------------|
| During drag | Compositor renders freely, position races | Compositor waits for WM_PAINT |
| Weave position | May differ from DWM composition position | Guaranteed to match |
| 3D effect | Broken during drag | Correct during drag |
| Frame rate | 60Hz | ~60Hz (paced by WM_PAINT + vsync) |

### Known Limitation: Minor Resize Jitter

The cross-thread architecture introduces a small visual artifact: **resize jitter** during drag. DWM may briefly stretch the previous frame to fit the new window bounds during the cross-thread round-trip. This is inherent to the architecture — the WM_PAINT trick on a same-thread app (like cube_ext_d3d11) does not have this artifact because render is inline with the modal loop.

Attempts to reduce jitter with `DwmFlush()`, `ValidateRect` cycling, and longer timeouts were tested but made the 3D phase sync worse. The current implementation prioritizes correct 3D over perfectly smooth resize visuals.

---

## Summary: When to Use Each Technique

| Scenario | Architecture | Technique |
|----------|-------------|-----------|
| App owns window (XR_EXT_win32_window_binding) | Single thread | WM_PAINT trick — render inline |
| Monado owns window (no extension) | Cross-thread | WM_PAINT sync via events |

Both achieve the same goal: **stable window position during weave + Present**. The cross-thread version uses synchronization primitives to achieve what the single-thread version gets for free.

---

## References

- Test app implementation: `test_apps/cube_ext_d3d11_win/` (D3D11 with WM_PAINT handling)
- Monado window management: `src/xrt/compositor/d3d11/comp_d3d11_window.cpp`
- Monado compositor integration: `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp`
- Extension spec: `docs/specs/XR_EXT_win32_window_binding.md` (Section 4.3)
- D3D11 renderer: `src/xrt/compositor/d3d11/comp_d3d11_renderer.cpp`
