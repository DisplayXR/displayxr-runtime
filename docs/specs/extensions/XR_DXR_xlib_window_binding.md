# XR_DXR_xlib_window_binding

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_xlib_window_binding` |
| Spec Version | 1 |
| Type Values | `XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR` (1004999200) |
| Author | The DisplayXR Project |
| Platform | Desktop Linux (X11/Xlib). Wayland out of scope (see §6). |

---

## 1. Overview

`XR_DXR_xlib_window_binding` allows an OpenXR application to provide its own X11 window (`Display*` + `Window`) to the runtime when creating a session on desktop Linux. When present, the runtime renders into the application's window instead of creating its own, and the application retains control of input, lifecycle, and the X event loop.

It is the desktop-Linux sibling of [`XR_DXR_win32_window_binding`](XR_DXR_win32_window_binding.md) (HWND) and [`XR_DXR_cocoa_window_binding`](XR_DXR_cocoa_window_binding.md) (NSView). The motivation — the window-focus problem, the two-pose problem, and the phase-alignment problem on lenticular 3D displays — is identical and documented in depth in the win32 spec (§2); it is not repeated here.

This extension is what makes the **handle class** (`_handle` apps) possible on Linux. The **hosted class** needs no extension — the runtime self-creates an XCB window (Linux Phase 1, `comp_vk_native_window_xcb.c`).

## 2. API Reference

### 2.1 Extension name and constants

```c
#define XR_DXR_xlib_window_binding 1
#define XR_DXR_xlib_window_binding_SPEC_VERSION 1
#define XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME "XR_DXR_xlib_window_binding"

#define XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999200)
```

### 2.2 XrXlibWindowBindingCreateInfoDXR

```c
typedef struct XrXlibWindowBindingCreateInfoDXR {
    XrStructureType             type;      // XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS    next;
    Display*                    xDisplay;  // Xlib display connection (XOpenDisplay)
    Window                      window;    // X11 Window (XID) owned by the app
} XrXlibWindowBindingCreateInfoDXR;
```

Chained into `XrSessionCreateInfo::next` at `xrCreateSession`, alongside the graphics binding:

```c
XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
// ... fill instance / physicalDevice / device / queue ...

XrXlibWindowBindingCreateInfoDXR xlibBinding = {XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR};
xlibBinding.next     = &vkBinding;
xlibBinding.xDisplay = dpy;      // the app's Display*
xlibBinding.window   = win;      // the app's mapped window

XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
sessionInfo.next     = &xlibBinding;
sessionInfo.systemId = systemId;
xrCreateSession(instance, &sessionInfo, &session);
```

**Valid usage:**

- `xDisplay` must be a live Xlib `Display*` (from `XOpenDisplay`). It must be an Xlib display — the runtime derives its XCB connection from it via `XGetXCBConnection()` (libX11-xcb); a bare `xcb_connection_t*` is not accepted.
- `window` must be a valid, mapped X11 window created on `xDisplay`.
- Both must outlive the session: the runtime's `VkSurfaceKHR` borrows the display's XCB connection for its lifetime. Destroy the window / close the display only **after** `xrDestroySession`.
- If either field is `NULL`/`0`, the structure is ignored and the session falls back to the hosted path (runtime-created window).

## 3. Runtime Behavior

- **Xlib in, XCB inside.** The runtime converts the app's `Display*` to its underlying `xcb_connection_t*` with `XGetXCBConnection()` and reuses the same XCB present path the hosted class uses (`comp_vk_native_xcb_handle` → `vkCreateXcbSurfaceKHR`, `VK_KHR_xcb_surface`). Because Xlib and XCB share one connection in libX11's hybrid mode, the app keeps full ownership of the X **event queue** — the runtime issues XCB *requests* (geometry queries, surface creation) but never reads events, so the app's `XNextEvent` loop is unaffected.
- **Window geometry.** The runtime seeds the swapchain extent from the window's live geometry (`xcb_get_geometry`) and polls it per-frame to react to resizes (no `ConfigureNotify` subscription — the app owns event selection on its own window).
- **Compositor selection.** The binding targets the in-process native Vulkan compositor (`comp_vk_native`). In service mode (`XRT_FORCE_MODE=ipc`) the in-process compositor is bypassed and the binding is currently not consumed (Linux Phase 2b, hardware-gated).
- **Window metrics / phase.** Window-relative metrics (screen position for interlacing phase, canvas-scoped Kooima) currently take the display-scoped fallback for app-provided windows; the position channel is Phase 3b hardware bring-up work.

## 4. Application Responsibilities

- Create, map, and destroy the window; run the X event pump (e.g. once per frame).
- Handle `WM_DELETE_WINDOW` and request session exit (`xrRequestExitSession`) on close.
- Keep the `Display*` open until after session destruction (§2.2 valid usage).

## 5. Reference Implementation

- Extension header: `src/external/openxr_includes/openxr/XR_DXR_xlib_window_binding.h`
- oxr consumption: `oxr_session.c` (Vulkan branch, packs `Display*`/`Window` into `comp_vk_native_xlib_handle`)
- Compositor: `comp_vk_native_compositor.c` (`XRT_OS_LINUX_DESKTOP` window block), `comp_vk_native_window_xcb.c` (`..._wrap_app_window`, `..._query_geometry`)
- Validation vehicle: `test_apps/cube_handle_vk_linux` (app-owned Xlib window + this binding; built by `scripts/build_linux.sh --apps`)

## 6. Out of Scope / Future

- **Texture class** (shared-texture content handoff, the `sharedTextureHandle` / readback fields of the win32/cocoa siblings) — deliberately absent from spec v1; add as a follow-up revision when a Linux `_texture` producer exists (#696 is the class taxonomy reference).
- **Transparent background** (`transparentBackgroundEnabled` sibling field) — X11 ARGB-visual transparency is not wired in the Linux compositor yet.
- **Window-space composition layers** (`XrCompositionLayerWindowSpaceDXR`) — not declared by this extension yet; the shared type value (1004999002) makes a later `#ifndef`-guarded adoption possible, mirroring the cocoa header.
- **Wayland** — no stable absolute window position (needed for interlacing phase); X11/XCB first by design (`docs/roadmap/linux-support.md` § Decisions).

## 7. Revision History

| Version | Changes |
|---------|---------|
| 1 | Initial version — window binding only (`xDisplay` + `window`); Linux Phase 3 (#660) |
