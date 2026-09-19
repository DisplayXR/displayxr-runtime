# XR_DXR_wayland_surface_binding

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_wayland_surface_binding` |
| Spec Version | 1 |
| Type Values | `XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR` (1004999250) |
| Author | The DisplayXR Project |
| Platform | Desktop Linux (Wayland). X11 has its own sibling: [`XR_DXR_xlib_window_binding`](XR_DXR_xlib_window_binding.md). |

---

## 1. Overview

`XR_DXR_wayland_surface_binding` lets an OpenXR application provide its own Wayland surface — a `wl_display*` + `wl_surface*` pair — to the runtime at session creation on desktop Linux. When present, the runtime builds its `VkSurfaceKHR` from that pair with `VK_KHR_wayland_surface` and presents into the application's surface instead of creating a window of its own.

It is the Wayland sibling of [`XR_DXR_xlib_window_binding`](XR_DXR_xlib_window_binding.md) (X11), [`XR_DXR_win32_window_binding`](XR_DXR_win32_window_binding.md) (HWND) and [`XR_DXR_cocoa_window_binding`](XR_DXR_cocoa_window_binding.md) (NSView). The motivation — window focus, the two-pose problem, interlacing-phase alignment — is identical and documented in depth in the win32 spec (§2); it is not repeated here.

The division of labour is narrower than on X11. The runtime consumes the pair and nothing else: **the application owns the whole Wayland client stack** — registry, xdg-shell role, configure acknowledgements, the event loop, input. There is no `wl_display_*` call anywhere in `src/`. Once the session exists, Mesa's WSI owns `wl_surface.attach` / `damage` / `commit` on that surface and the application must not touch them.

**Current support is fullscreen-on-the-panel only.** Everything else weaves at the wrong scale — see §5.

## 2. API Reference

### 2.1 Extension name and constants

```c
#define XR_DXR_wayland_surface_binding 1
#define XR_DXR_wayland_surface_binding_SPEC_VERSION 1
#define XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME "XR_DXR_wayland_surface_binding"

#define XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999250)
```

The header is gated on `#if defined(__linux__) && !defined(__ANDROID__)`. It forward-declares `struct wl_display;` / `struct wl_surface;` so it stays self-contained — include `<wayland-client.h>` **before** it to get the real types.

### 2.2 XrWaylandSurfaceBindingCreateInfoDXR

```c
typedef struct XrWaylandSurfaceBindingCreateInfoDXR {
    XrStructureType             type;       // XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS    next;
    struct wl_display*          wlDisplay;  // from wl_display_connect()
    struct wl_surface*          wlSurface;  // app-owned, must already have an xdg role
    XrBool32                    transparentBackgroundEnabled;
} XrWaylandSurfaceBindingCreateInfoDXR;
```

Chained onto `XrSessionCreateInfo::next`, ahead of `XrGraphicsBindingVulkanKHR`.

**Valid usage:**

- `wlDisplay` must be a live client connection (`wl_display_connect`).
- `wlSurface` must be a surface created on that connection which **already has an xdg-shell role and has acknowledged its first `xdg_surface.configure`** (§4).
- Both must outlive the session: the `VkSurfaceKHR` borrows the connection for its lifetime. Destroy the surface / disconnect the display only **after** `xrDestroySession` and after the Vulkan instance is gone.
- If the structure is chained but either field is `NULL`, `xrCreateSession` fails with `XR_ERROR_VALIDATION_FAILURE` naming the extension and the offending field (`oxr_session.c`, the `XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR` branch). It used to be dropped silently and fall through to the hosted path, where the runtime then tried to create an XCB window and failed with an error that never mentioned Wayland. Omitting the structure entirely is still a hosted session.
- If an `XrXlibWindowBindingCreateInfoDXR` is (implausibly) chained as well, the Wayland binding wins — it is parsed second and overwrites `window_handle` (`oxr_session.c:4353-4355`).
- `transparentBackgroundEnabled` is honoured (`oxr_session.c:4860-4866` sets `xsi.transparent_background_enabled`). Transparency is native on Wayland — a surface composites its premultiplied alpha over whatever is behind it, with none of the X11 ARGB-visual dance.

## 3. Runtime Behavior

- **Surface creation is synchronous, inside `xrCreateSession`.** `comp_vk_native_target.cpp:1797-1836` resolves `vkCreateWaylandSurfaceKHR` and calls it with the pair, then checks `vkGetPhysicalDeviceSurfaceSupportKHR` for the compositor's queue family. A missing PFN fails the session with `vkCreateWaylandSurfaceKHR not available — VK_KHR_wayland_surface must be enabled`.
- **Branch selection.** `comp_vk_native_compositor.c:8269-8285` takes the Wayland branch, copies the handle, sets `c->use_wayland = true`, clears `c->xcb_window`, sets `c->owns_window = false`, and logs `Using app-provided Wayland surface (XR_DXR_wayland_surface_binding)` at INFO. The target is then created with `target_is_wayland = true` (`:7936-7944`).
- **Swapchain extent = the PANEL, not the surface.** On Wayland `VkSurfaceCapabilitiesKHR::currentExtent` is `UINT32_MAX` (the compositor asks the client to choose), and the Linux extent fix-up block (`comp_vk_native_compositor.c:8469-8497`) only consults `c->xcb_window` / `c->xcb_handle.connection`, both NULL here. `c->settings.preferred` therefore keeps the panel pixel dimensions seeded from display info, and the WSI swapchain is created at panel size.
- **No resize follow.** The per-frame Linux resize poll (`comp_vk_native_compositor.c:2008-2030`) is likewise XCB-only, so an `xdg_toplevel.configure` that changes the surface size is never observed by the runtime.
- **The runtime never pumps the Wayland queue.** There is no `wl_display_dispatch` / `wl_display_flush` / `wl_display_roundtrip` call in `src/`. Everything the compositor sends — including `xdg_wm_base.ping` — is the application's to service.
- **Window position / weave scope.** Wayland never tells a client where its surface is, so the interlacing-phase anchor comes from a compositor-published D-Bus geometry service (`comp_vk_native_wl_geom.c`, created at `comp_vk_native_compositor.c:8282`). Without the `window-geometry@displayxr.org` GNOME Shell extension the runtime logs `wl_geom: geometry service org.displayxr.WindowGeometry not answering …` and weaves **display-scoped** — correct for a fullscreen surface anchored at the panel's top-left, wrong anywhere else. Contract: [`docs/specs/runtime/wayland-window-geometry.md`](../runtime/wayland-window-geometry.md), boundary rule ADR-033. Note that the display processor receives no window handle at all on this path (`vk_make_dp_vk: passing X11 window XID 0x0 to the weaver`).

## 4. Application Responsibilities

These are requirements, not advice — violating the first two is a protocol error that kills the connection.

1. **Give the surface a role and ack its first configure BEFORE `xrCreateSession`.** The full sequence is: `wl_display_connect` → registry (bind `wl_compositor`, `xdg_wm_base`, `wl_seat`, every `wl_output`) → roundtrip → `wl_compositor_create_surface` → `xdg_wm_base_get_xdg_surface` → `xdg_surface_get_toplevel` → `set_title` / `set_app_id` → `set_fullscreen` → `wl_surface_commit` → roundtrip until `xdg_surface.configure` arrives → `xdg_surface_ack_configure`. Only then create the session. The runtime creates the `VkSurfaceKHR` synchronously and the WSI attaches a buffer at the first present; attaching to a role-less or unconfigured surface is fatal.
2. **Never attach a buffer, commit, or install a `wl_surface_frame` callback on that surface once the session exists.** Mesa's WSI owns `attach` / `damage` / `commit`. The single `wl_surface_commit` in step 1 — before the session — is the only one the application may issue.
3. **Own the event loop, non-blocking, every frame.** Drain the socket with `wl_display_prepare_read` + `poll(timeout 0)` + `wl_display_read_events` + `wl_display_dispatch_pending`, then `wl_display_flush`. Never call blocking `wl_display_dispatch` in the render loop.
4. **Answer `xdg_wm_base.ping` with `xdg_wm_base_pong`.** A client that does not is killed as unresponsive.
5. **Handle `xdg_toplevel.close`** by requesting session exit (`xrRequestExitSession`).
6. **Go fullscreen on the panel.** `xdg_toplevel_set_fullscreen(<wl_output>)` is the INV-1.3 substitute — a Wayland client cannot position itself. Match the output by comparing `wl_output.geometry(x,y)` and the current `wl_output.mode(width,height)` against the panel rect the runtime reports (`XrDisplayDesktopPositionDXR` `left/top` + `XrDisplayInfoDXR` `displayPixelWidth/Height`); pass `NULL` and log when nothing matches.
7. **Run the desktop at 100% scale.** `wl_output` geometry is in *logical* coordinates while the runtime's panel rect is in device pixels, so at any other scale the output match cannot succeed and the fullscreen surface is not panel-sized. Compare the configure size to the panel size and warn once when they differ — the weave cannot be 1:1 in that session.
8. **Keep the connection alive past teardown.** Destroy the toplevel / xdg_surface / wl_surface and `wl_display_disconnect` only after `xrDestroySession`, `vkDestroyDevice` and `vkDestroyInstance`.

## 5. Limitations (current revision)

| Limitation | Why | Consequence |
|---|---|---|
| **Fullscreen-on-panel only** | WSI swapchain is created at panel dims because `currentExtent == UINT32_MAX` and no Wayland branch exists in the extent fix-up (`comp_vk_native_compositor.c:8469-8497`) | A surface that is not exactly panel-sized is resampled by the compositor; the lenticular phase and scale are wrong |
| **No resize follow** | Per-frame resize poll is XCB-only (`:2008-2030`) | Resizing the surface mid-session does not re-create the swapchain |
| **Position only via the geometry service** | Wayland does not expose surface position | Without the GNOME Shell extension the weave is display-scoped (fine fullscreen, wrong windowed) — `docs/specs/runtime/wayland-window-geometry.md` |
| **Non-unit desktop scale is refused for windowed geometry** | Logical vs device pixels | #817: the runtime refuses Wayland window geometry at non-unit scale rather than weaving at a known-wrong phase |
| **Texture class** | No shared-texture field in this revision | No Linux `_texture` producer exists yet (#696 is the class taxonomy reference) |
| **Service / IPC mode** | Binding is consumed by the in-process `comp_vk_native` compositor only | Under `XRT_FORCE_MODE=ipc` the binding is not consumed (same status as the xlib sibling) |

## 6. Reference Implementation

- Extension header: `src/external/openxr_includes/openxr/XR_DXR_wayland_surface_binding.h`
- oxr consumption: `src/xrt/state_trackers/oxr/oxr_session.c:4350-4364` (handle packing), `:4860-4866` (transparency opt-in)
- Compositor: `src/xrt/compositor/vk_native/comp_vk_native_compositor.c:8269-8285` (branch), `:7932-7945` (target create)
- Target / surface: `src/xrt/compositor/vk_native/comp_vk_native_target.cpp:1797-1836`
- Window geometry provider: `src/xrt/compositor/vk_native/comp_vk_native_wl_geom.c`
- **Test app:** `test_apps/cube_handle_vk_linux` — one binary, both backends, `--backend=x11|wayland|auto` (or `DXR_WINDOW_BACKEND`). The Wayland client is the shared helper `test_apps/common/dxr_linux_window.{h,cpp}`, which implements every requirement in §4 and is the thing to copy. `test_apps/cube_zones_vk_linux` uses the same helper. Build with `scripts/build_linux.sh --apps`; the xdg-shell glue is generated by `wayland-scanner` from the XML vendored at `test_apps/common/wayland-protocols/`.

## 7. Revision History

| Version | Changes |
|---------|---------|
| 1 | Initial version — `wlDisplay` + `wlSurface` + `transparentBackgroundEnabled`. Type value renumbered to 1004999250 from an initial 1004999210 that collided with `XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR`. |
