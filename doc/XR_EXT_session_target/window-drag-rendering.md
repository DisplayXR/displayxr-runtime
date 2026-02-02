# Rendering During Window Drag: WM_PAINT Trick vs. Render Thread

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

## Solution: The WM_PAINT Trick

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

## References

- Test app implementation: `test_apps/sr_cube_openxr_ext/` (D3D11 with WM_PAINT handling)
- Extension spec: `doc/XR_EXT_session_target/XR_EXT_session_target.md` (Section 4.3)
- D3D11 renderer: `src/xrt/compositor/d3d11/comp_d3d11_renderer.cpp`
