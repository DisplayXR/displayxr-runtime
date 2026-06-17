# App Classes

DisplayXR supports four ways for an application to integrate with the runtime, differing in who owns the window and rendering targets.

## The Four Classes

| Class | Suffix | Description | Compositor path |
|-------|--------|-------------|----------------|
| **Handle** | `_handle` | App provides its own window handle via `XR_EXT_*_window_binding` | Native compositor directly in-process |
| **Texture** | `_texture` | App provides a shared texture **and its own window handle**; runtime weaves the 3D **canvas sub-rect** into the shared texture. App declares the sub-rect (`xrSetSharedTextureOutputRectEXT`) and may fill the surrounding 2D area (`xrSetSharedTextureSurround2DEXT`) | Native compositor directly in-process |
| **Hosted** | `_hosted` | Runtime creates window and rendering targets (standard OpenXR/WebXR) | Native compositor directly in-process |
| **IPC/Service** | _(internal)_ | Out-of-process via client compositor → IPC → server multi-compositor. Used internally by the shell and WebXR — apps don't need to target this directly. | Client compositor → IPC → multi-compositor → native compositor in server |

> **Class is about *who presents the surface*, not about mixing 2D and 3D.**
> The class only decides who owns the window/swapchain (runtime = Handle,
> app/engine/browser = Texture, runtime-everything = Hosted). **To put 3D inside
> a 2D UI, you don't pick a class — you submit layers + a mask** (see
> [Mixing 2D and 3D](#mixing-2d-and-3d-in-one-app) below). A **Handle** app
> submitting a 3D projection layer + a 2D layer + a 3D-ness mask is the
> recommended way to build a local 2D/3D app.

## Mixing 2D and 3D in one app

A 3D viewport embedded in a flat UI (toolbars, panels, chrome) is built by
**submitting layers**, independent of app class — `XR_EXT_local_3d_zone`:

- **3D layer** — your normal projection layer (the weaved stereo content).
- **2D layer** — `XrCompositionLayerLocal2DEXT`, a post-weave screen-space 2D
  layer submitted through the normal `xrEndFrame` layer list (toolbars/UI).
- **3D-ness mask** `M ∈ [0,1]` — a *separate scalar channel* (not the 2D
  layer's alpha) selecting where the display is 3D (`M=1` → keep the weave) vs
  2D (`M=0`). Authoring tiers: whole-window / rect-list / freeform. The same
  mask also drives the hardware switchable-lens cells on displays that support
  per-region 2D/3D ([`XR_EXT_display_zones`](../specs/extensions/XR_EXT_display_zones.md)).

The runtime weaves the full bounding rect (phase is tied to absolute screen
position) and then alpha-composites the 2D layer per the mask — so disconnected
/ non-rectangular 3D regions just work. This is the in-process native-compositor
path; **a `_handle` app + 3D layer + 2D layer + mask is the first-choice way to
build a local 2D/3D app**. Design: [unified 2D/3D compositing](../roadmap/unified-2d-3d-compositing.md);
API: [`XR_EXT_local_3d_zone.h`](../../src/external/openxr_includes/openxr/XR_EXT_local_3d_zone.h).

For **HUD-style 2D over the whole weave** (status bars, overlays in window-pixel
space rather than a masked region), submit `XrCompositionLayerWindowSpaceEXT`
layers — see `windowspace_handle_*` test apps.

The older **Texture-class "2D surround"** (`xrSetSharedTextureSurround2DEXT`)
predates this and is now a special case of the same idea; prefer the Local2D
layer + mask for new apps. Texture class remains the right choice when the
app/engine **must own the swapchain** (present-ownership) — see below.

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

- **Need the app, engine, or browser to own the swapchain?** Use **Texture** — for present-ownership: offscreen rendering, frame capture/streaming, or an engine/browser that must hold its own surface. You create and own the window (and pass its handle) and provide a shared texture; the runtime composites into it and uses your HWND for display-processor position tracking.
  - To embed 3D in a 2D UI, **don't reach for Texture's `xrSetSharedTextureSurround2DEXT` first** — prefer the Local2D layer + mask ([Mixing 2D and 3D](#mixing-2d-and-3d-in-one-app)), which works for Handle apps too. `texture + mask` is also supported; the legacy surround side-channel remains for present-owning apps that haven't migrated.

  Full contract: [`XR_EXT_win32_window_binding`](../specs/extensions/XR_EXT_win32_window_binding.md) §3.5 (output rect), §3.6/§3.7 (legacy 2D surround).

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
| `windowspace_handle_d3d11_win` | Handle (+ window-space 2D layers) | D3D11 | Windows |

The `windowspace_handle_*` apps (D3D11/D3D12/GL/VK on Windows) exercise the
mixed-2D/3D path — `XrCompositionLayerWindowSpaceEXT` layers over the weave —
rather than a different app class.

## Further Reading

- [XR_EXT_local_3d_zone](../../src/external/openxr_includes/openxr/XR_EXT_local_3d_zone.h) + [unified 2D/3D compositing](../roadmap/unified-2d-3d-compositing.md) — the Local2D layer + 3D-ness mask (recommended way to mix 2D and 3D)
- [XR_EXT_display_zones](../specs/extensions/XR_EXT_display_zones.md) — hardware per-region 2D/3D (switchable-lens cells) driven by the same mask
- [In-Process vs Service](../architecture/in-process-vs-service.md) — detailed comparison of the two compositor deployment modes
- [XR_EXT_win32_window_binding](../specs/extensions/XR_EXT_win32_window_binding.md) — Win32 window binding spec
- [XR_EXT_cocoa_window_binding](../specs/extensions/XR_EXT_cocoa_window_binding.md) — macOS window binding spec
- [Service-Mode Multi-Compositor](../architecture/multi-compositor.md) — multi-compositor architecture for the IPC path
