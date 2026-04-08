# App Classes

DisplayXR supports four ways for an application to integrate with the runtime, differing in who owns the window and rendering targets.

## The Four Classes

| Class | Suffix | Description | Compositor path |
|-------|--------|-------------|----------------|
| **Handle** | `_handle` | App provides its own window handle via `XR_EXT_*_window_binding` | Native compositor directly in-process |
| **Texture** | `_texture` | App provides textures, runtime composites into its own window | Native compositor directly in-process |
| **Hosted** | `_hosted` | Runtime creates window and rendering targets (standard OpenXR/WebXR) | Native compositor directly in-process |
| **IPC/Service** | _(internal)_ | Out-of-process via client compositor → IPC → server multi-compositor. Used internally by the shell and WebXR — apps don't need to target this directly. | Client compositor → IPC → multi-compositor → native compositor in server |

## Which Class Should I Use?

- **Building a native app with your own window?** Use **Handle**. You create and manage the window, pass the handle (HWND, NSView) to the runtime via `XR_EXT_win32_window_binding` or `XR_EXT_cocoa_window_binding`. Most control, best for apps that need to own their window lifecycle.

- **Building an app that renders to an offscreen texture?** Use **Texture**. You provide textures to the runtime, which composites them into its own window. Good for apps that want to render 3D content as part of a larger 2D UI.

- **Building a standard OpenXR app?** Use **Hosted**. The runtime creates everything — window, swapchains, rendering targets. This is the standard OpenXR path and the simplest integration. Also the path for WebXR content.

- **Multi-app / shell / WebXR?** The **IPC/Service** path is used internally by the [DisplayXR Shell](https://github.com/DisplayXR/displayxr-shell-releases) and WebXR browsers. Apps don't need to target IPC directly — the shell launches standard handle apps and manages multi-app compositing transparently.

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
- [XR_EXT_win32_window_binding](../specs/XR_EXT_win32_window_binding.md) — Win32 window binding spec
- [XR_EXT_cocoa_window_binding](../specs/XR_EXT_cocoa_window_binding.md) — macOS window binding spec
- [Spatial OS](../roadmap/spatial-os.md) — multi-compositor architecture for the IPC path
