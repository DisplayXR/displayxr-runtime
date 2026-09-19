# XR_DXR_wayland_surface_binding

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_wayland_surface_binding` |
| Spec Version | 2 |
| Type Values | `XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR` (1004999250), `XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR` (1004999251) |
| Author | The DisplayXR Project |
| Platform | Desktop Linux (Wayland). X11 has its own sibling: [`XR_DXR_xlib_window_binding`](XR_DXR_xlib_window_binding.md). |

---

## 1. Overview

`XR_DXR_wayland_surface_binding` lets an OpenXR application provide its own Wayland surface — a `wl_display*` + `wl_surface*` pair — to the runtime at session creation on desktop Linux. When present, the runtime builds its `VkSurfaceKHR` from that pair with `VK_KHR_wayland_surface` and presents into the application's surface instead of creating a window of its own.

It is the Wayland sibling of [`XR_DXR_xlib_window_binding`](XR_DXR_xlib_window_binding.md) (X11), [`XR_DXR_win32_window_binding`](XR_DXR_win32_window_binding.md) (HWND) and [`XR_DXR_cocoa_window_binding`](XR_DXR_cocoa_window_binding.md) (NSView). The motivation — window focus, the two-pose problem, interlacing-phase alignment — is identical and documented in depth in the win32 spec (§2); it is not repeated here.

The division of labour is narrower than on X11. The runtime consumes the pair and nothing else: **the application owns the whole Wayland client stack** — registry, xdg-shell role, configure acknowledgements, the event loop, input. There is no `wl_display_*` call anywhere in `src/`. Once the session exists, Mesa's WSI owns `wl_surface.attach` / `damage` / `commit` on that surface and the application must not touch them.

**Windowed surfaces are supported from spec version 2** — but only because the application tells the runtime how big its surface is. A `wl_surface` has no intrinsic size: the WSI reports `currentExtent == {UINT32_MAX, UINT32_MAX}` and the buffer the runtime attaches is what *defines* the surface, so a runtime that guesses does not mis-size the window, it **resizes** it. Nobody else can supply the number either — the compositor-side geometry service cannot bootstrap it, because Mutter lists a window only once it has a mapped buffer and the first buffer comes from the very swapchain whose size is in question. The application always knows: it received and acked the `xdg_toplevel.configure`.

So on Wayland the two halves of "where is this window" come from opposite directions: **size from the app** ([`XrWaylandSurfaceGeometryDXR`](#22-xrwaylandsurfacegeometrydxr-spec-v2) / [`xrSetWaylandSurfaceGeometryDXR`](#23-xrsetwaylandsurfacegeometrydxr-spec-v2)), **position from the compositor** ([the geometry service](../runtime/wayland-window-geometry.md)). Both report *geometry*; the weaver still owns all phase, so [ADR-033](../../adr/ADR-033-placement-reports-geometry-weaver-owns-phase.md) is unchanged. The shape is lifted from the Android sibling [`xrSetAndroidWindowGeometryDXR`](XR_DXR_android_surface_binding.md) (ADR-036 D6), which exists for the same reason: a window fact the runtime cannot observe from outside the app's own toolkit.

## 2. API Reference

### 2.1 Extension name and constants

```c
#define XR_DXR_wayland_surface_binding 1
#define XR_DXR_wayland_surface_binding_SPEC_VERSION 2
#define XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME "XR_DXR_wayland_surface_binding"

#define XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999250)
#define XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR            ((XrStructureType)1004999251)
```

The header is gated on `#if defined(__linux__) && !defined(__ANDROID__)`. It forward-declares `struct wl_display;` / `struct wl_surface;` so it stays self-contained — include `<wayland-client.h>` **before** it to get the real types.

### 2.1b XrWaylandSurfaceBindingCreateInfoDXR

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
- `transparentBackgroundEnabled` is honoured (`oxr_session.c`, the `XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR` transparency branch, sets `xsi.transparent_background_enabled`). Transparency is native on Wayland — a surface composites its premultiplied alpha over whatever is behind it, with none of the X11 ARGB-visual dance.
- The structure is **unchanged** in spec v2. The geometry fields went into a separate structure rather than onto the end of this one, because growing a published structure changes its size and breaks every application compiled against v1.

### 2.2 XrWaylandSurfaceGeometryDXR (spec v2)

```c
typedef struct XrWaylandSurfaceGeometryDXR {
    XrStructureType             type;   // XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR
    const void* XR_MAY_ALIAS    next;
    uint32_t                    width;             // buffer pixels, 0 = unknown
    uint32_t                    height;            // buffer pixels, 0 = unknown
    uint32_t                    refreshMilliHertz; // wl_output.mode, 0 = unknown
} XrWaylandSurfaceGeometryDXR;
```

Chained onto `XrSessionCreateInfo::next` **alongside** `XrWaylandSurfaceBindingCreateInfoDXR`, not instead of it.

**Valid usage:**

- Optional. Omitting the structure, or leaving a field 0, keeps the pre-v2 behaviour for that field: a panel-sized swapchain, and a 60 Hz assumption. A v1 application is therefore unaffected.
- `width` / `height` are the size of the **buffer** the runtime should attach, not the logical size from `xdg_toplevel.configure`. They are equal at desktop scale 1.0 and only there.
  - A **fullscreen** toplevel must report the `wl_output.mode` size of the output it is fullscreen on. The compositor maps that buffer onto the whole output, so buffer pixels and panel pixels line up 1:1 — e.g. a toplevel configured at 1728×1080 on a 166.67% desktop declares **2880×1800**. Reporting the logical size instead hands the weaver a 1728×1080 image for the compositor to upscale, and that resample destroys the interlace.
  - A **windowed** toplevel has no such probe. Report the configure size, and accept that the weave is not 1:1 unless the desktop is at 100% (§5).
- `refreshMilliHertz` is `wl_output.mode`'s refresh, in the same milli-hertz unit the protocol uses (e.g. `59997` for 59.997 Hz). The runtime cannot query it on Wayland — the RandR path that serves the X11 sibling needs an XCB connection — so without it the runtime keeps a hardcoded 60 Hz and hands the display processor a frame period that can be 2–4× too long on a high-refresh panel.
- `xrCreateSession` fails with `XR_ERROR_VALIDATION_FAILURE` only for the binding structure's own field checks; a zeroed geometry structure is legal and simply declares nothing.

### 2.3 xrSetWaylandSurfaceGeometryDXR (spec v2)

```c
XrResult xrSetWaylandSurfaceGeometryDXR(XrSession session,
                                        uint32_t  width,
                                        uint32_t  height,
                                        uint32_t  refreshMilliHertz);
```

Republishes the geometry mid-session. Scalar parameters rather than a structure pointer, because there is no chain to extend and nothing optional to express beyond the 0 sentinel on `refreshMilliHertz`.

**Valid usage:**

- Call it for every `xdg_toplevel.configure` whose size differs from the one in force, **after** acking it — never before.
- `width` and `height` must be non-zero; `XR_ERROR_VALIDATION_FAILURE` otherwise. `refreshMilliHertz` may be 0, which leaves the session's current value alone.
- The session must have been created with a chained `XrWaylandSurfaceBindingCreateInfoDXR`; `XR_ERROR_VALIDATION_FAILURE` otherwise.
- Returns `XR_ERROR_FUNCTION_UNSUPPORTED` from `xrGetInstanceProcAddr` when the extension was not enabled at `xrCreateInstance`.
- The runtime de-duplicates, so calling it once per frame with an unchanged size is a cheap no-op and logs nothing.
- **Position is not a parameter.** A Wayland client is never told where its surface is; the runtime gets that from the compositor geometry service.
- Thread-safe against the render thread: the runtime takes the compositor lock, so an application may call it from whichever thread sees its configures.
- **Resolve it through `xrGetInstanceProcAddr`.** A spec-v1 runtime returns `XR_ERROR_FUNCTION_UNSUPPORTED`, which is a degraded session and not a broken one: the size declared at create still applies, only later resizes stop being followed. `test_apps/common/dxr_linux_window.cpp` (`DxrLinuxWindow::attach_session`) logs that once and carries on.

## 3. Runtime Behavior

- **Surface creation is synchronous, inside `xrCreateSession`.** `comp_vk_native_target.cpp:1797-1836` resolves `vkCreateWaylandSurfaceKHR` and calls it with the pair, then checks `vkGetPhysicalDeviceSurfaceSupportKHR` for the compositor's queue family. A missing PFN fails the session with `vkCreateWaylandSurfaceKHR not available — VK_KHR_wayland_surface must be enabled`.
- **Branch selection.** `comp_vk_native_compositor.c:8269-8285` takes the Wayland branch, copies the handle, sets `c->use_wayland = true`, clears `c->xcb_window`, sets `c->owns_window = false`, and logs `Using app-provided Wayland surface (XR_DXR_wayland_surface_binding)` at INFO. The target is then created with `target_is_wayland = true` (`:7936-7944`).
- **Swapchain extent = the DECLARED size, falling back to the panel.** On Wayland `VkSurfaceCapabilitiesKHR::currentExtent` is `UINT32_MAX` (the compositor asks the client to choose), so the Linux extent fix-up block in `comp_vk_native_compositor.c` — which for X11 queries the window — takes a Wayland arm that uses `XrWaylandSurfaceGeometryDXR`'s `width`/`height` instead. When nothing was declared, `c->settings.preferred` keeps the panel pixel dimensions seeded from display info and the swapchain is created at panel size, which (since the buffer defines the surface) forces the window to the panel. The compositor logs which of the two happened at `xrCreateSession`.
- **Resize follow.** The per-frame Linux resize poll in `vk_compositor_begin_frame` takes the same Wayland arm: it reads the latest value published by `xrSetWaylandSurfaceGeometryDXR` under the compositor mutex and, when it differs from the live swapchain, calls the same `vk_output_follow_window_locked` path the XCB leg uses — swapchain recreate plus `vkDeviceWaitIdle`. Nothing is polled; the app pushes.
- **Refresh rate.** `refreshMilliHertz` replaces the hardcoded 60 Hz default. The X11 leg reads the current mode over RandR, which needs an XCB connection the Wayland path does not have; before spec v2 the Wayland session silently kept 60 and did not even log it.
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
| **Size must be declared, or the window is forced to the panel** | `currentExtent == UINT32_MAX`, and on Wayland the buffer *defines* the surface | An app that chains no `XrWaylandSurfaceGeometryDXR` gets a panel-sized swapchain — which does not mis-size its window, it resizes it. This is the v1 behaviour, kept as the fallback |
| **Position only via the geometry service** | Wayland does not expose surface position | Without the `window-geometry@displayxr.org` GNOME Shell extension the weave is display-scoped: fine for a fullscreen surface at the panel origin, wrong for a window anywhere else — `docs/specs/runtime/wayland-window-geometry.md`. Note that extension only becomes active at the user's next login |
| **Non-unit desktop scale: fullscreen survives, windowed does not** | `xdg_toplevel.configure` is logical; only the buffer is in device pixels | A fullscreen surface can still be 1:1 by declaring the matched `wl_output.mode` size (§2.2), which is how it worked before v2 as well. A *windowed* surface has no way to recover its device-pixel size, so #817 has the runtime refuse Wayland window geometry at non-unit scale rather than weave at a known-wrong phase |
| **Texture class** | No shared-texture field in this revision | No Linux `_texture` producer exists yet (#696 is the class taxonomy reference) |
| **Service / IPC mode** | Binding is consumed by the in-process `comp_vk_native` compositor only | Under `XRT_FORCE_MODE=ipc` the binding is not consumed (same status as the xlib sibling) |

## 6. Reference Implementation

- Extension header: `src/external/openxr_includes/openxr/XR_DXR_wayland_surface_binding.h`
- oxr consumption: `src/xrt/state_trackers/oxr/oxr_session.c` (handle packing, geometry parse, transparency opt-in — grep `XR_TYPE_WAYLAND_SURFACE`)
- oxr entry point: `src/xrt/state_trackers/oxr/oxr_api_session.c` (`oxr_xrSetWaylandSurfaceGeometryDXR`), registered in `oxr_api_negotiate.c` via `ENTRY_IF_EXT`, declared in `oxr_api_funcs.h`
- Compositor: `src/xrt/compositor/vk_native/comp_vk_native_compositor.c` — the `use_wayland` branch in create, the Wayland arm of the Linux extent fix-up, the Wayland arm of the `begin_frame` resize follow, and `comp_vk_native_compositor_set_wayland_surface_geometry`. Carrier struct: `comp_vk_native_window_xcb.h`, `struct comp_vk_native_wayland_handle`
- Target / surface: `src/xrt/compositor/vk_native/comp_vk_native_target.cpp:1797-1836`
- Window geometry provider: `src/xrt/compositor/vk_native/comp_vk_native_wl_geom.c`
- **Test app:** `test_apps/cube_handle_vk_linux` — one binary, both backends, `--backend=x11|wayland|auto` (or `DXR_WINDOW_BACKEND`). The Wayland client is the shared helper `test_apps/common/dxr_linux_window.{h,cpp}`, which implements every requirement in §4 and is the thing to copy. `test_apps/cube_zones_vk_linux` uses the same helper. Build with `scripts/build_linux.sh --apps`; the xdg-shell glue is generated by `wayland-scanner` from the XML vendored at `test_apps/common/wayland-protocols/`.

## 7. Revision History

| Version | Changes |
|---------|---------|
| 1 | Initial version — `wlDisplay` + `wlSurface` + `transparentBackgroundEnabled`. Type value renumbered to 1004999250 from an initial 1004999210 that collided with `XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR`. |
| 2 | Added `XrWaylandSurfaceGeometryDXR` (1004999251) and `xrSetWaylandSurfaceGeometryDXR`. A `wl_surface` has no intrinsic size and the buffer the runtime attaches defines it, so the app must declare the size — this is what makes windowed Wayland work, and it also carries the `wl_output` refresh the runtime cannot query without an XCB connection. `XrWaylandSurfaceBindingCreateInfoDXR` is byte-for-byte unchanged, so v1 applications keep working (and keep the panel-sized fallback). |
