# App Classes

DisplayXR supports four ways for an application to integrate with the runtime, differing in who owns the window and rendering targets.

## The Four Classes

| Class | Suffix | Description | Compositor path |
|-------|--------|-------------|----------------|
| **Handle** | `_handle` | App provides its own window handle via `XR_EXT_*_window_binding` | Native compositor directly in-process |
| **Texture** | `_texture` | App provides a shared texture **and its own window handle**; runtime weaves the 3D **canvas sub-rect** into the shared texture. App declares the sub-rect (`xrSetSharedTextureOutputRectEXT`) and may fill the surrounding 2D area (`xrSetSharedTextureSurround2DEXT`) | Native compositor directly in-process |
| **Hosted** | `_hosted` | Runtime creates window and rendering targets (standard OpenXR/WebXR) | Native compositor directly in-process |
| **IPC/Service** | _(internal)_ | Out-of-process via client compositor → IPC → server multi-compositor. Used internally by the shell and WebXR — apps don't need to target this directly. | Client compositor → IPC → multi-compositor → native compositor in server |

## Who passes the window handle?

A frequent point of confusion — the in-process classes differ in **who owns the window**, i.e. what `hwnd`/`NSView` (if any) the app passes to the runtime, and therefore what handle the native compositor hands the display processor for phase/position tracking:

| Class | App passes to runtime | Compositor's window |
|-------|-----------------------|---------------------|
| **Handle** | the app's **real** window handle | uses the app's window |
| **Texture** | the app's **real** window handle (+ a shared texture) | offscreen / shared texture; the HWND is only for DP position tracking |
| **Hosted** | **NULL** | the runtime creates its **own** window at native resolution |

So `NULL` is the **Hosted** path (not Handle) — that's the case that makes the runtime self-create a window. The display processor *always* receives a real handle: the app's for Handle/Texture, the runtime's self-created window for Hosted. Authoritative branch: the window-handling block in `*_compositor_create` (e.g. `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp`).

## Which Class Should I Use?

- **Building a native app with your own window?** Use **Handle**. You create and manage the window, pass the handle (HWND, NSView) to the runtime via `XR_EXT_win32_window_binding` or `XR_EXT_cocoa_window_binding`. Most control, best for apps that need to own their window lifecycle.

- **Building an app that renders to an offscreen texture?** Use **Texture**. You create and own the window (and pass its handle) and provide a shared texture; the runtime composites into it and uses your HWND for display-processor position tracking. Two pieces let the 3D content live inside a larger 2D UI:
  - **3D-zone sub-rect** — by default the whole client area is the 3D canvas; call `xrSetSharedTextureOutputRectEXT(x, y, w, h)` to confine weaved 3D output to a sub-rect (e.g. a viewport surrounded by toolbars). The runtime feeds that rect to the DP as `canvas_offset/size` so interlacing phase and Kooima projection stay correct; you blit the same sub-rect of the shared texture into your window.
  - **2D surround** — pixels *outside* the 3D sub-rect are undefined by default. Register a full-window 2D shared texture via `xrSetSharedTextureSurround2DEXT` (D3D11/keyed-mutex) or `xrSetSharedTextureSurround2DFenceEXT` (D3D12/fence) and the runtime blits its non-canvas pixels into the swapchain each frame — full-resolution UI/chrome around the 3D zone.

  Full contract: [`XR_EXT_win32_window_binding`](../specs/extensions/XR_EXT_win32_window_binding.md) §3.5 (output rect), §3.6/§3.7 (2D surround).

- **Building a standard OpenXR app?** Use **Hosted**. The runtime creates everything — window, swapchains, rendering targets. This is the standard OpenXR path and the simplest integration. Also the path for WebXR content.

- **Multi-app / shell / WebXR?** The **IPC/Service** path is used internally by the [DisplayXR Shell](https://github.com/DisplayXR/displayxr-shell-releases) and WebXR browsers. Apps don't need to target IPC directly — the shell launches standard handle apps and manages multi-app compositing transparently.

- **Building a WebXR app and want DisplayXR awareness?** WebXR pages automatically run as hosted legacy apps via Chrome's built-in WebXR implementation. To access display info, rendering-mode events, eye-tracked poses, window metadata, HUD overlay, and forwarded input, install the WebXR Bridge v2 Chrome extension. The bridge exposes a `session.displayXR` namespace on the standard WebXR session. Start with the [WebXR Bridge developer guide](../../webxr-bridge/DEVELOPER.md) — it covers integration, the `session.displayXR` API, Kooima projection, common pitfalls, and links to a runnable [minimal example](../../webxr-bridge/examples/minimal.html) and the [full reference sample](../../webxr-bridge/sample/sample.js).

## Code Paths

The first three classes all use a native compositor in-process:
```
_handle / _texture / _hosted → compositor/{d3d11,d3d12,metal,gl,vk_native}/
```

The `_ipc` class is fundamentally different — the app links a **client compositor** that serializes calls over IPC to a **server process** running the multi-compositor:
```
_ipc → compositor/client/ → ipc/ → compositor/multi/ → native compositor
```

## Test App Naming Convention

Test apps follow the pattern `cube_{class}_{api}_{platform}`:

| Example | Class | API | Platform |
|---------|-------|-----|----------|
| `cube_handle_metal_macos` | Handle | Metal | macOS |
| `cube_handle_d3d11_win` | Handle | D3D11 | Windows |
| `cube_texture_metal_macos` | Texture | Metal | macOS |
| `cube_texture_d3d11_win` | Texture | D3D11 | Windows |
| `cube_hosted_metal_macos` | Hosted | Metal | macOS |
| `cube_hosted_d3d11_win` | Hosted | D3D11 | Windows |

## Further Reading

- [In-Process vs Service](../architecture/in-process-vs-service.md) — detailed comparison of the two compositor deployment modes
- [XR_EXT_win32_window_binding](../specs/extensions/XR_EXT_win32_window_binding.md) — Win32 window binding spec
- [XR_EXT_cocoa_window_binding](../specs/extensions/XR_EXT_cocoa_window_binding.md) — macOS window binding spec
- [Service-Mode Multi-Compositor](../architecture/multi-compositor.md) — multi-compositor architecture for the IPC path
