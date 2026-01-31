# SR SDK Bug Report: D3D11 Crash in Multi-Threaded Applications

## Summary

Applications that pump Win32 messages on the main thread and call `weave()` on a dedicated render thread crash during mouse interaction. The most reliable trigger is mouse button release (`WM_LBUTTONUP`).

The root cause is that `CreateDX11Weaver` receives the application's `ID3D11DeviceContext` (the immediate context) and installs `weaverWndProc` via `SetWindowLongPtr` — creating a situation where the D3D11 immediate context can be touched from two threads simultaneously without any thread-safety protection. The D3D11 immediate context is **not thread-safe by default**.

**Affected APIs:** D3D11 (confirmed). Likely also D3D12 and Vulkan variants if they share a similar pattern.

## Symptoms

- Application crashes during mouse-up while rendering is active
- No shutdown or destruction involved — the weaver is alive and in use
- Crash does NOT occur in single-threaded applications where the message pump and `weave()` run on the same thread
- Crash appears immediately when rendering is moved to a separate thread

## Architecture That Triggers It

Many applications use a dedicated render thread to keep animation smooth during window drag/resize (where `DefWindowProc` enters a modal loop that blocks the message pump thread):

```
Main thread:     GetMessage loop → DispatchMessage → WndProcDispatcher → weaverWndProc
                   └→ CallWindowProc → app WndProc → DefWindowProc → DXGI housekeeping (touches D3D11 context)

Render thread:   app D3D11 rendering (clear, draw, set viewport, map/unmap) → weave() (D3D11 draw calls)
```

In a single-threaded app these are serialized and never overlap. With a render thread, they execute simultaneously.

## Why Mouse-Up Specifically

`WM_LBUTTONUP` handling typically calls `ReleaseCapture()`, which synchronously sends `WM_CAPTURECHANGED` back to the window. This means `weaverWndProc` is called **twice** in rapid succession on the main thread, increasing the collision window with concurrent D3D11 calls on the render thread. However, the crash can occur on any message that causes DXGI to touch the D3D11 device context while the render thread is active.

## Root Cause

The D3D11 immediate device context (`ID3D11DeviceContext`) is shared between two threads without thread-safety:

1. **Render thread:** The application makes 20+ D3D11 immediate context calls per frame — `ClearRenderTargetView`, `RSSetViewports`, `DrawIndexed`, `Map`/`Unmap`, etc. — followed by `weave()`, which issues its own D3D11 draw calls through the same context.

2. **Main thread:** `weaverWndProc` processes window messages and chains to the application's WndProc via `CallWindowProc`. When `DefWindowProc` processes certain messages, DXGI performs internal housekeeping that touches the D3D11 immediate context (e.g., swap chain operations, fullscreen transitions, window position changes).

The D3D11 immediate context is **not thread-safe by default** (per Microsoft documentation). Concurrent access from two threads is undefined behavior and causes crashes.

The SR SDK creates this unsafe pattern by:
1. Accepting the application's `ID3D11DeviceContext` in `CreateDX11Weaver`
2. Installing `weaverWndProc` on the application's window (which runs on the main/message thread)
3. Exposing `weave()` for the application to call (typically from a render thread)
4. Providing **no synchronization** between these two code paths
5. **Not enabling `ID3D11Multithread::SetMultithreadProtected(TRUE)`** on the context

Note: the SR SDK's own member variables accessed by `weaverWndProc` (`m_windowData`) vs. `weave()` (`SRMonitorRectangle`, etc.) appear to be disjoint. The race is not in the SDK's C++ members — it is in the shared D3D11 immediate context that both code paths use.

## Recommended Fix

The simplest and most robust fix is for the SR SDK to enable D3D11 multithread protection when it receives the device context:

```cpp
// In CreateDX11Weaver, after receiving the ID3D11DeviceContext:
ID3D11Multithread *mt = nullptr;
if (SUCCEEDED(d3d11Context->QueryInterface(__uuidof(ID3D11Multithread),
                                           reinterpret_cast<void **>(&mt))) &&
    mt != nullptr) {
    mt->SetMultithreadProtected(TRUE);
    mt->Release();
}
```

`SetMultithreadProtected(TRUE)` tells the D3D11 runtime to internally serialize **all** immediate context calls via a critical section. This covers every D3D11 call on both threads — rendering, weaving, DXGI housekeeping, swap chain operations — without requiring the application to add explicit synchronization.

### Performance Note

`SetMultithreadProtected(TRUE)` adds a critical section enter/leave around every D3D11 context call. For single-threaded applications this overhead is negligible (uncontended critical sections are ~20ns on modern hardware). For multi-threaded applications it is *necessary* for correctness. The SDK could conditionally enable it only when it detects that `weaverWndProc` and `weave()` are called from different threads.

### Alternative: Internal Synchronization

If enabling global multithread protection is undesirable, the SDK could add internal locking to serialize `weaverWndProc` with `weave()`:

- Use a `std::recursive_mutex` (recursive because `WM_LBUTTONUP` → `ReleaseCapture()` → `WM_CAPTURECHANGED` is reentrant)
- `weave()` takes the lock for the duration of its D3D11 work
- `weaverWndProc` takes the lock before calling `CallWindowProc` (the point where control passes to the app's WndProc and eventually `DefWindowProc`/DXGI)
- The lock must NOT be held across the entire `weaverWndProc` if the app might call back into weaver APIs, to avoid deadlock

However, this only protects the `weaverWndProc` ↔ `weave()` overlap. It does **not** protect the render thread's non-weave D3D11 calls (clear, draw, etc.) from racing with the main thread's DXGI housekeeping. `SetMultithreadProtected(TRUE)` is the more complete fix.

## Reproduction

1. Create a Win32 + D3D11 application with:
   - Main thread: `GetMessage` / `DispatchMessage` loop
   - Render thread: D3D11 rendering + `weave()` at frame rate
2. Create the weaver with `CreateDX11Weaver`, passing the window handle and immediate context
3. Click and drag inside the window, then release the mouse button
4. Observe crash on mouse-up

The crash is highly reproducible with continuous mouse interaction during rendering. It does NOT reproduce in single-threaded applications.

## Related Issue

This is separate from the `WndProcDispatcher` use-after-free during weaver destruction (documented in `sr-sdk-race-condition-bug.md`). That bug involves the instance pointer becoming dangling after `weaver->destroy()`. This bug involves concurrent access to a live weaver instance.
