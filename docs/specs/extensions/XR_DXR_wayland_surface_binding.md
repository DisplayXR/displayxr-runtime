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
  - A **windowed** toplevel has no such probe. Report the configure size multiplied by the output's real scale if you know it (`wp_fractional_scale_v1`, or `wl_output.mode ÷ zxdg_output_v1.logical_size` — never the integer `wl_output.scale`); report the configure size otherwise, and accept that the weave then is not 1:1. The runtime measures the difference and degrades the session to flat 2D rather than weaving into the resample (§5, #1595).
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
- **Window position / weave scope.** Wayland never tells a client where its surface is, so the interlacing-phase anchor comes from a compositor-published D-Bus geometry service (`comp_vk_native_wl_geom.c`, created at `comp_vk_native_compositor.c:8282`). Without the `window-geometry@displayxr.org` GNOME Shell extension the runtime logs `wl_geom: geometry service org.displayxr.WindowGeometry not answering …` and weaves **display-scoped** — correct for a fullscreen surface anchored at the panel's top-left, wrong anywhere else. Contract: [`docs/specs/runtime/wayland-window-geometry.md`](../runtime/wayland-window-geometry.md), boundary rule ADR-033. Note that the display processor receives no window handle at all on this path (`vk_make_dp_vk: passing X11 window XID 0x0 to the weaver`). That is the SR SDK's supported **windowless** construction, not a degraded one — `srCreateWeaverVulkan` with `window = 0` weaves, and takes its position from `srWeaverSetPresentOrigin` instead (see [§5](#5-limitations-current-revision)).
- **The present origin IS fed on this path, in device pixels** (#1596). Earlier revisions of this spec said a windowless weave was display-scoped until that landed; it has. Wayland reports geometry in *logical* coordinates and everything the weaver consumes is *device* pixels, so the runtime converts at its Wayland boundary: `comp_vk_native_wl_geom` takes the geometry service's `monitor` rect and Mutter's **fractional** scale (never the integer `wl_output.scale`, which is 2 on a 1.6667 output) and returns the window rect in device pixels with its origin relative to its own monitor — a mixed-scale layout has no single global device grid, but a displacement inside one output is exact, and that displacement is precisely what the phase needs. `get_window_metrics` then rebuilds an absolute rect as *runtime-resolved panel origin + monitor-relative device offset* (ADR-033), so the subtraction that produces the present origin is exact by construction. Two refusals guard it: the payload must carry a `monitor` object, and the window's monitor must be the 3D panel (otherwise one `wl_geom: this surface is on a … px output, but the 3D panel is …` WARN, and display-scoped metrics). The conversion is isolated as platform-free arithmetic in `u_wayland_geom.h` and host-tested (`tests/tests_aux_wayland_geom.cpp`, both of the measured box's scales); its correctness *on a panel* is a separate question, and no phase or scale claim here has been validated on hardware.

## 4. Application Responsibilities

These are requirements, not advice — violating the first two is a protocol error that kills the connection.

1. **Give the surface a role and ack its first configure BEFORE `xrCreateSession`.** The full sequence is: `wl_display_connect` → registry (bind `wl_compositor`, `xdg_wm_base`, `wl_seat`, every `wl_output`) → roundtrip → `wl_compositor_create_surface` → `xdg_wm_base_get_xdg_surface` → `xdg_surface_get_toplevel` → `set_title` / `set_app_id` → `set_fullscreen` → `wl_surface_commit` → roundtrip until `xdg_surface.configure` arrives → `xdg_surface_ack_configure`. Only then create the session. The runtime creates the `VkSurfaceKHR` synchronously and the WSI attaches a buffer at the first present; attaching to a role-less or unconfigured surface is fatal.
2. **Never attach a buffer, commit, or install a `wl_surface_frame` callback on that surface once the session exists.** Mesa's WSI owns `attach` / `damage` / `commit`. The single `wl_surface_commit` in step 1 — before the session — is the only one the application may issue.
3. **Own the event loop, non-blocking, every frame.** Drain the socket with `wl_display_prepare_read` + `poll(timeout 0)` + `wl_display_read_events` + `wl_display_dispatch_pending`, then `wl_display_flush`. Never call blocking `wl_display_dispatch` in the render loop.
4. **Answer `xdg_wm_base.ping` with `xdg_wm_base_pong`.** A client that does not is killed as unresponsive.
5. **Handle `xdg_toplevel.close`** by requesting session exit (`xrRequestExitSession`).
6. **Go fullscreen on the panel — and match the output in DEVICE pixels.** `xdg_toplevel_set_fullscreen(<wl_output>)` is the INV-1.3 substitute; a Wayland client cannot position itself. The match is where this goes wrong, because `wl_output.geometry(x,y)` is **logical** while the panel rect the runtime reports (`XrDisplayDesktopPositionDXR` `left/top` + `XrDisplayInfoDXR` `displayPixelWidth/Height`) is **device pixels**. Comparing them directly is a logical-vs-device comparison that simply cannot succeed on a scaled desktop: on 2026-09-20 no `wl_output` matched the panel rect `3840x2160+3456+0`, `set_fullscreen(NULL)` let the compositor choose, and it chose the laptop. The recipe:
   - Bind `zxdg_output_manager_v1` and take each output's **logical** rect from `zxdg_output_v1.logical_position` / `logical_size`. (`xdg-output-unstable-v1` is not in `wayland-protocols`' stable set; vendor the XML — the reference client keeps it at `test_apps/common/wayland-protocols/xdg-output-unstable-v1.xml`.)
   - Derive that output's scale as `wl_output.mode(width) ÷ logical_size(width)`. **Never `wl_output.scale`** — it is an integer by protocol and reports `2` for a 1.6667 output, which is a 20 % error that looks entirely plausible. (`wp_fractional_scale_v1`, where the compositor offers it, is the other honest source.)
   - **Match SIZE first, and let it decide.** An output's `wl_output.mode` is device pixels by protocol and `displayPixelWidth/Height` is device pixels by contract, so those two compare with no conversion at all — the reliable half of the match. When exactly one output matches by size, **take it** even if the converted origin disagrees, and warn; a lone size match is far likelier to be the panel than the origin conversion is to be exactly right on a mixed-scale layout.
   - Use the **converted device origin** (`logical_position × scale`) as the tie-break: when several outputs share the panel's mode size, the origin is what picks between them.
   - Pass `NULL` and log when nothing matches. The runtime will separately notice that the surface is not on the panel and refuse to weave into it (§5, #1595), but a clear app-side log is what makes that diagnosable.
7. **Declare a buffer that is 1:1 with what the compositor will paint — a fractionally-scaled desktop is workable, a resample is not.** 100 % scale is no longer the requirement it was: with the device-pixel match in §4.6 and the runtime's logical→device conversion (#1596), a fullscreen surface on a fractionally-scaled output is a supported configuration. What is required is that the buffer you attach equal the region the compositor paints it into — for a fullscreen toplevel, the matched output's `wl_output.mode` size, **not** the logical `xdg_toplevel.configure` size (§2.2). Declaring the logical size on a 1.6667 desktop hands the weaver a 1728×1080 image to be upscaled by 5/3, and a resampled ~1-pixel-period interlace is a uniform double image with no vantage point where it resolves. Compare your buffer size against the panel size and warn once when they differ. The runtime **enforces** this rather than trusting it: it degrades the session to flat 2D rather than weaving into a resample (§5, #1595), because correct content in the wrong dimensionality is strictly better than a double image, and it recovers the instant the session becomes 1:1.
8. **Keep the connection alive past teardown.** Destroy the toplevel / xdg_surface / wl_surface and `wl_display_disconnect` only after `xrDestroySession`, `vkDestroyDevice` and `vkDestroyInstance`.

## 5. Limitations (current revision)

| Limitation | Why | Consequence |
|---|---|---|
| **Size must be declared, or the window is forced to the panel** | `currentExtent == UINT32_MAX`, and on Wayland the buffer *defines* the surface | An app that chains no `XrWaylandSurfaceGeometryDXR` gets a panel-sized swapchain — which does not mis-size its window, it resizes it. This is the v1 behaviour, kept as the fallback |
| **Leia SR weaves *windowless* here — phase comes from the present origin, not a window handle** | There is no window handle on this path, and none is needed: the plug-in constructs the weaver with `srCreateWeaverVulkan(..., window = 0)`, which the v2 C API documents as `0 -> nullptr -> windowless weave`. Position is then supplied through `srWeaverSetPresentOrigin` rather than read off a window. Measured 2026-09-20 on a DS1 in a GNOME 50 Wayland session: `Vulkan weaver created (window=0x0 = windowless/display-scoped)`, display processor `backend: weaver`, the weave path entered, and **zero** occurrences of `weaving is disabled` or `Window handle is invalid`. **The opposite null is a different path:** in `WeaverBaseImpl::canWeaveInternal` a weaver *constructed* without a window sets a `constructedWithoutWindow` flag and always weaves, while a weaver constructed *with* a handle that later becomes null never weaves — same null, opposite outcome, decided by whether it was null at construction. So construct windowless; never null out a handle you passed | Native-Wayland apps weave, **and the phase is now fed**: [#1596](https://github.com/DisplayXR/displayxr-runtime/issues/1596) has landed, so `srWeaverSetPresentOrigin` receives the window's device-pixel origin on this path (§3) whenever the geometry service resolves it and the surface is on the 3D panel. It falls back to display-scoped — origin (0,0) — when either is not true, and the weave is suppressed entirely when the session cannot be 1:1 (next-but-one row). The origin's *correctness on a panel* is unvalidated |
| **Position only via the geometry service** | Wayland does not expose surface position | Without the `window-geometry@displayxr.org` GNOME Shell extension the weave is display-scoped: fine for a fullscreen surface at the panel origin, wrong for a window anywhere else — `docs/specs/runtime/wayland-window-geometry.md`. Note that extension only becomes active at the user's next login |
| **Non-unit desktop scale: the geometry converts; the buffer still has to be 1:1** | `xdg_toplevel.configure` is logical, only the buffer is device pixels — two different problems that #1557 conflated under one refusal | The *units* problem is solved: the runtime converts the compositor-published geometry to device pixels at its Wayland boundary ([#1596](https://github.com/DisplayXR/displayxr-runtime/issues/1596)), so a windowed surface on a fractionally-scaled monitor now gets a real phase anchor instead of none. The *resample* problem is not, and cannot be solved by the runtime: if the buffer is not the window's device extent the compositor scales it on the way to the panel. The runtime measures that and **degrades to flat 2D** rather than weaving into it ([#1595](https://github.com/DisplayXR/displayxr-runtime/issues/1595)) — one `NOT_1TO1:` WARN naming both extents, reversible via `NOT_1TO1 cleared:`. The app's job is §4.7: declare a buffer equal to what the compositor will paint |
| **A surface that is not on the 3D panel** | `set_fullscreen(NULL)`, or an output match made in the wrong coordinate space, lands the surface on another monitor | Measured 2026-09-20: no `wl_output` matched the panel rect `3840x2160+3456+0` (the comparison was logical-vs-device and could never succeed), the surface fullscreened on the 2880×1800 laptop, and the weave ran anyway. Both halves are now closed — the runtime refuses panel-scoped metrics when the window's monitor is not the panel (§3) and degrades the weave (#1595) — and the app-side fix is the device-pixel output match in §4.6 |
| **Texture class** | No shared-texture field in this revision | No Linux `_texture` producer exists yet (#696 is the class taxonomy reference) |
| **Service / IPC mode** | Binding is consumed by the in-process `comp_vk_native` compositor only | Under `XRT_FORCE_MODE=ipc` the binding is not consumed (same status as the xlib sibling) |

> **Withdrawn 2026-09-20 — the null-window blocker was never real for windowless construction.**
> An earlier revision of this table claimed the Leia SR display processor *could not* weave a Wayland
> surface on LeiaSR 1.37, and that Leia weaving on Linux was therefore X11/XWayland only
> (added in `acb73755a`, [#1586](https://github.com/DisplayXR/displayxr-runtime/issues/1586)).
> **That claim is wrong and is withdrawn.** It was sourced from a `VulkanWeaver::setWindowHandle`
> failure — the *constructed-with-a-handle-that-later-went-null* path, which genuinely never weaves —
> and so never applied to a weaver constructed windowless in the first place; it was either a
> different construction path or an older package. Windowless weaving is supported and is what this
> path uses. The vendor-side ask filed as
> [LeiaSR PR #224, comment 5749033439](https://github.com/LeiaInc/LeiaSR/pull/224#issuecomment-5749033439)
> is resolved/withdrawn with it: no vendor change is required for the Leia DP to weave on native Wayland.

## 6. Reference Implementation

- Extension header: `src/external/openxr_includes/openxr/XR_DXR_wayland_surface_binding.h`
- oxr consumption: `src/xrt/state_trackers/oxr/oxr_session.c` (handle packing, geometry parse, transparency opt-in — grep `XR_TYPE_WAYLAND_SURFACE`)
- oxr entry point: `src/xrt/state_trackers/oxr/oxr_api_session.c` (`oxr_xrSetWaylandSurfaceGeometryDXR`), registered in `oxr_api_negotiate.c` via `ENTRY_IF_EXT`, declared in `oxr_api_funcs.h`
- Compositor: `src/xrt/compositor/vk_native/comp_vk_native_compositor.c` — the `use_wayland` branch in create, the Wayland arm of the Linux extent fix-up, the Wayland arm of the `begin_frame` resize follow, and `comp_vk_native_compositor_set_wayland_surface_geometry`. Carrier struct: `comp_vk_native_window_xcb.h`, `struct comp_vk_native_wayland_handle`
- Target / surface: `src/xrt/compositor/vk_native/comp_vk_native_target.cpp:1797-1836`
- Window geometry provider: `src/xrt/compositor/vk_native/comp_vk_native_wl_geom.c`
- **Test app:** `test_apps/cube_handle_vk_linux` — one binary, both backends, `--backend=x11|wayland|auto` (or `DXR_WINDOW_BACKEND`). The Wayland client is the shared helper `test_apps/common/dxr_linux_window.{h,cpp}`, which implements every requirement in §4 and is the thing to copy. `test_apps/cube_zones_vk_linux` uses the same helper. Build with `scripts/build_linux.sh --apps`; the xdg-shell and `xdg-output-unstable-v1` glue is generated by `wayland-scanner` from the XML vendored at `test_apps/common/wayland-protocols/` (the helper needs xdg-output for the device-pixel output match in §4.6).

## 7. Revision History

| Version | Changes |
|---------|---------|
| 1 | Initial version — `wlDisplay` + `wlSurface` + `transparentBackgroundEnabled`. Type value renumbered to 1004999250 from an initial 1004999210 that collided with `XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR`. |
| 2 | Added `XrWaylandSurfaceGeometryDXR` (1004999251) and `xrSetWaylandSurfaceGeometryDXR`. A `wl_surface` has no intrinsic size and the buffer the runtime attaches defines it, so the app must declare the size — this is what makes windowed Wayland work, and it also carries the `wl_output` refresh the runtime cannot query without an XCB connection. `XrWaylandSurfaceBindingCreateInfoDXR` is byte-for-byte unchanged, so v1 applications keep working (and keep the panel-sized fallback). |
