# Linux Support

Status: **Preview — code-complete + hardware-validated, not yet GA.** Phases 0/1a/2a/3a
are code-complete on `main`; hosted + handle (`XR_DXR_xlib_window_binding`) sessions
bring up the native Vulkan/XCB compositor and render the stereo cube on real Vulkan+X11
hardware (Ubuntu 22.04, RTX 3080 + Acer SpatialLabs DS1; #708 / #706), and the **Track B
real srSDK Vulkan weave is HW-validated on the DS1** (lens enables). Runtime **v1.28.0**
is the first tag with complete Linux support. Distribution = user-level **tarball** via
`scripts/package_linux.sh` (#705/#713) — no installer asset ships on releases. **CI:** a
tri-LTS matrix (Ubuntu 22.04/24.04/26.04) is a required check on the runtime and all 5
demos (#714/#722); all 5 demos are build-green on real 22.04/24.04/26.04 desktops.
**Still open:** Phase 2b service-side render (**#710**), windowed-3D phase origin
(**#729/#730**, twin of Windows #85), mixed 2D/3D display-zones — software 2D-in-3D
works, hardware per-zone lens switching SDK-blocked (**#778**, Phase 3c below), the
deployment target (Ubuntu 26.04 + Intel Arc, blocked on hardware), and the Track B
shippable re-pin onto a merged `sr-sdk-v*` tag.
Windows, macOS, and Android ship today.

## TL;DR

DisplayXR's **non-presentation substrate is already Linux-ready** — it survived
the Monado fork intact and still compiles behind the non-Windows `#ifdef`/CMake
branches. The **entire gap is presentation**: no native compositor can put
pixels on a Linux display, plus there's no Linux build tooling. So the port is
narrow and well-bounded, sequenced as: prove the substrate headless (Phase 0),
then add an in-process Vulkan/XCB compositor (Phase 1), then the service/IPC
path (Phase 2), then a window-binding extension for app-owned windows (Phase 3).

## What already works on Linux (inherited from Monado, kept compiling)

| Area | Status | Where |
|---|---|---|
| OS detection (`XRT_OS_LINUX`/`XRT_OS_UNIX`, `XRT_HAVE_LINUX`) | ✅ | `include/xrt/xrt_config_os.h:30-34`, `CMakeLists.txt:191-192` |
| `aux/os` — pthread threading, `clock_gettime` time, hidraw | ✅ | `auxiliary/os/` (POSIX paths shared with macOS) |
| IPC transport — AF_UNIX sockets + epoll mainloop | ✅ wired | `ipc/server/ipc_server_mainloop_linux.c`, `ipc/CMakeLists.txt:171-173` |
| IPC fd-passing for swapchain handles (SCM_RIGHTS) | ✅ | `ipc/shared/ipc_message_channel_unix.c:233-255` |
| Shared memory (`shm_open`/`mmap`) | ✅ | `ipc/shared/ipc_shmem.c` (UNIX branch) |
| Handle typedefs (shmem/buffer/sync all = fd) | ✅ defined | `include/xrt/xrt_handles.h` (`XRT_GRAPHICS_*_HANDLE_IS_FD`) |
| Plug-in discovery — JSON manifest, XDG + `/usr/share` roots, `dlopen` | ✅ | `targets/common/target_plugin_loader.c:1299+` (POSIX path, `.so`-agnostic) |
| `XRT_FEATURE_SERVICE` (no OS predicate) + systemd socket activation | ✅ builds | `CMakeLists.txt:256-257`, `targets/service/CMakeLists.txt:69-148` |
| Plug-in / display-processor ABI | ✅ platform-neutral | `include/xrt/xrt_plugin.h`, `xrt_display_processor*.h` |

## The gap (what Linux is missing)

1. **No native presentation compositor.**
   - `vk_native` is explicitly fenced to `WIN32 OR APPLE OR ANDROID`
     (`compositor/CMakeLists.txt:339`, `vk_native/CMakeLists.txt:8`), and its
     surface-create has a literal Linux-fails `#else` returning
     `XRT_ERROR_DEVICE_CREATION_FAILED` (`comp_vk_native_target.cpp:887-891`).
     No `VK_KHR_xcb/xlib/wayland_surface` code exists.
   - `gl` is gated off on Linux entirely (`(WIN32 OR APPLE) AND XRT_HAVE_OPENGL`,
     #709 — it used to configure on bare `XRT_HAVE_OPENGL`): it has no
     GLX/EGL-on-X11 window path — GLX was deliberately removed
     (`CMakeLists.txt:215`) — and nothing on Linux links it. Linux is
     Vulkan-only; the GL *client* bindings in `comp_client` are separate and
     unaffected.
2. **No self-created window for the `_hosted` class.** Only `comp_window_macos`
   and `comp_window_android` survive — no `comp_window_xcb/wayland/direct`.
3. **No Linux window-binding extension.** Only `XR_DXR_win32_window_binding` and
   `XR_DXR_cocoa_window_binding` exist; the `_handle`/`_texture` classes have no
   way to pass a Linux window to the runtime.
4. **No build tooling.** No `build_linux.sh`, no Linux OpenXR-loader
   provisioning, no install rule that drops sim_display manifests into the
   Linux discovery roots. `find_package(udev REQUIRED)` was a hard configure
   block.

### Surviving Monado remnants worth reusing

Dangling but harmless, useful for Phase 1: `vkCreateXcbSurfaceKHR` /
`vkCreateWaylandSurfaceKHR` PFNs are still declared/loaded
(`aux/vk/vk_helpers.h:287,292`, `vk_function_loaders.c:104,109`); CMake already
probes XCB and sets `VK_USE_PLATFORM_XCB_KHR` (`CMakeLists.txt:195,229`); the
unbuilt OpenXR GL-on-Linux *client* bindings (`oxr_session_gfx_gl_xlib.c`,
`oxr_session_gfx_egl.c`, `xrt_gfx_xlib.h`, `xrt_gfx_egl.h`) survive behind
never-set `XRT_HAVE_XLIB`/`XRT_HAVE_OPENGL_GLX` flags.

## Phased plan

### Phase 0 — Configure + headless self-test ✅ (this change)

Highest leverage: prove the whole substrate before touching pixels. Validates
OS detection, `aux/os`, the POSIX plug-in loader, the sim_display display
processor, and that the runtime starts and reports valid display info — all
hardware-free, no GPU/window/loader.

**Delivered & CI-verified green** on `ubuntu-latest` (`displayxr-cli selftest`
passes: instance + system + head device + active plug-in `sim-display` ABI v4 +
display info `0.3440m x 0.1940m, 1920x1080 px`):
- `scripts/build_linux.sh` — builds the runtime, `displayxr-cli`, and the
  sim_display plug-in; stages the plug-in + a JSON manifest; runs
  `displayxr-cli selftest` (the exact hardware-free gate CI runs) + `info`.
- `.github/workflows/build-linux.yml` — non-required dev-loop job
  (`workflow_dispatch` + `linux*` branches); `ubuntu-latest` is the box that
  gets Phase 0 green. Promote to a required PR check once stable.
- `CMakeLists.txt` — (a) relaxed `find_package(udev REQUIRED)` → optional so a
  box without `libudev-dev` configures (`XRT_HAVE_LIBUDEV` just turns off; the DP
  comes from the plug-in loader, not udev); (b) `CMAKE_POSITION_INDEPENDENT_CODE
  ON` on Linux so static libs (incl. the FetchContent'd `displayxr_mcp`) link
  into the runtime `.so`.
- `src/xrt/drivers/CMakeLists.txt` + `sim_display_plugin.c` — gate the desktop-GL
  display processor (source, `aux_ogl` link, plug-in factory) on `XRT_HAVE_OPENGL`
  (no-op on Win/macOS, dropped on a headless Linux box with no GL); route Linux
  through the Android-style self-contained plug-in model (static-link aux, no
  runtime-DLL import — the version script hides all aux symbols but
  `xrtPluginNegotiate`).

All five changes are guarded to be no-ops off Linux; Windows/macOS/Android CI
stayed green on the merge. `displayxr-cli` links the no-compositor instance
(`target_instance_no_comp`), so Phase 0 needs neither a native compositor nor the
OpenXR loader.

**Follow-ups (small, after first green):** add a Linux job to `build-linux.yml`
running this script as a required check; add an install rule that drops the
sim_display `.so` + manifest into the XDG / `/usr/share/displayxr/DisplayProcessors`
roots so discovery works without `XRT_PLUGIN_SEARCH_PATH`.

### Phase 1 — In-process presentation via `vk_native` + XCB

Vulkan is mandatory and cross-platform, so `vk_native` is the cheapest
functional compositor. **XCB before Wayland** — X11 lets a client query its own
absolute screen position, which the display processor needs every frame for
interlacing-phase tracking; Wayland has no clean answer, so it's the harder
second target.

**Phase 1a — compile-green on Linux CI ✅ (done).** All of `comp_vk_native`
(including the new XCB window helper + the XCB surface arm) compiles and links
into `openxr_displayxr.so` on `ubuntu-latest`, headless selftest still green:
- Both `vk_native` CMake gates enabled on `(XRT_HAVE_LINUX AND XRT_HAVE_XCB)`
  (the inner `vk_native/CMakeLists.txt` **and** the outer
  `add_subdirectory(vk_native)` in `compositor/CMakeLists.txt` — both needed).
- `comp_vk_native_target.cpp` — `VK_KHR_xcb_surface` arm replacing the
  unconditional-fail `#else` (uses the already-loaded `vk->vkCreateXcbSurfaceKHR`).
- `comp_vk_native_window_xcb.{c,h}` — new self-created XCB window helper for the
  hosted class (connect/create/map, `WM_DELETE_WINDOW` close, `ConfigureNotify`
  resize, `translate_coordinates` screen position). Exposes connection + window
  via `comp_vk_native_xcb_handle` since `vkCreateXcbSurfaceKHR` needs both.
- `comp_vk_native_compositor.c` — Linux arms for struct fields, hosted
  self-create, seed/resize dims, validity, destroy, target-create gate,
  `get_window_metrics`, HUD, handle-app view scaling.
- `oxr_vulkan.c` — advertises `VK_KHR_surface` + `VK_KHR_xcb_surface` so the
  app's instance can build an XCB surface.
- `build-linux.yml` / `build_linux.sh` — `libxcb*-dev` enables `XRT_HAVE_XCB`.

**Phase 1b — on-screen present (pending Linux hardware).** CI has no display, so
the XCB path compiles but isn't exercised at runtime. The bring-up vehicle —
`test_apps/legacy/cube_hosted_legacy_vk_linux` (hosted legacy Vulkan cube; `main.cpp`
shared verbatim with the macOS peer) — is scaffolded and **compile-validated on
CI** (`build_linux.sh --apps` builds the OpenXR loader + the app). It's hosted, so
the runtime self-creates the XCB window — no window-binding extension needed.
Validate on a real Linux + GPU box:
```
./scripts/build_linux.sh --apps      # build runtime + loader + app
./build/run_cube_hosted_legacy_vk_linux.sh   # needs DISPLAY + a Vulkan GPU
```
Likely follow-ups surfaced only at runtime: confirm the VK-native compositor path
is selected (`OXR_ENABLE_VK_NATIVE_COMPOSITOR`), swapchain format/extent on real
drivers, and resize. A non-legacy (extension) `cube_hosted_vk_linux` follows once
first-light is confirmed. The handle-class app now exists —
`cube_handle_vk_linux` (`XR_DXR_xlib_window_binding`, Phase 3a below).

**Done when:** a hosted cube renders into a runtime-created XCB window with
sim_display weaving.

**Window placement on multi-monitor boxes (#715).** The self-owned XCB window
opens at the 3D panel's desktop position, mirroring the Windows reference
(`comp_d3d11_window.cpp`): the vendor plug-in reports the panel top-left via
`xrt_plugin_display_info` → `xsysc->info.display_screen_left/top`, the
compositor forwards it into `comp_vk_native_window_xcb_create`, and the helper
asks for it three ways (create-time x/y, `WM_NORMAL_HINTS` US/PPosition, and a
post-map `ConfigureRequest` — the same move `xdotool windowmove` sends, since
WMs like Mutter auto-place fresh toplevels otherwise). (0, 0) means primary
monitor — the sim_display convention and the unknown-panel fallback. Manual
override, checked **before** the plug-in value (dev boxes, sim_display):
`DXR_WINDOW_POS=x,y` in top-down desktop pixels, e.g. `DXR_WINDOW_POS=1920,0`.
The same knob and plug-in plumbing apply to the macOS vk_native self-owned
window (`comp_vk_native_window_macos.m`).

### Phase 2 — Service / IPC path

**Phase 2a — build-green on Linux CI ✅ (done).** `displayxr-service` + the
IPC-client runtime compile on `ubuntu-latest` (`./scripts/build_linux.sh
--service`, CI `Service` job in `build-linux.yml`). The substrate needed no new
platform arms — the service CMake gate (`targets/CMakeLists.txt`), the Linux
IPC mainloop (`ipc_server_mainloop_linux.c`), the VK client compositor
(`comp_vk_client.c`, platform-neutral), the IPC-client runtime target
(`XRT_FEATURE_IPC_CLIENT`, auto-ON with service), and the service
system-compositor factory (`target_instance.c`, null compositor by default off
Windows) were all already fence-free on Linux. Delivered on top:

- Extension-list arms (desktop-Linux-guarded, no-op elsewhere):
  `comp_vk_glue.c` instance list gains `VK_KHR_surface` + `VK_KHR_xcb_surface`
  (the `xrGetVulkanInstanceExtensionsKHR` answer — without it an enable1 VK
  app's instance can't build the XCB surface); the FD device-extension arms in
  `comp_vk_glue.c` **and** `oxr_vulkan.c` gain `VK_KHR_swapchain` (they were the
  only arms missing it — the vk_native compositor presents on the app's
  VkDevice); `null_compositor.c` `instance_extensions_common` gains
  `VK_KHR_xcb_surface` (service-side VkInstance, ready for the window arm).
- `build_linux.sh --service` now asserts `displayxr-service` + the IPC-client
  `openxr_displayxr.so` actually linked; headless selftest still runs.

**Correction to the earlier sketch:** the service does *not* reuse the Phase 1
vk_native window backend. Its system compositor is **null + comp_multi + the
DP plug-in weave** (macOS is the exact analog); vk_native is instantiated
in-process only (`oxr_session_gfx_vk_native.c`). The DP-weave path is
platform-neutral and needs no VkSurface.

**Phase 2b — on-screen out-of-process present (pending Linux hardware, with
Phase 1b).** Validate with `displayxr-service` + a client under
`XRT_FORCE_MODE=ipc` (swapchain-image fd-passing over the unix socket). Known
gaps to wire when a display exists, all mirroring the macOS arms:

1. **No `comp_window_xcb` comp_target** — `compositor/main/` only has
   `comp_window_android.c` / `comp_window_macos.m`. A Linux service-owned
   window target should reuse the Phase 1 XCB helper
   (`comp_vk_native_window_xcb.c`).
2. **`null_compositor_init_target_service` has no Linux arm**
   (`null_compositor.c`, WIN32/ANDROID/MACOS only) —
   `create_from_window` stays NULL, so the service can't own a present window.
3. **`ipc_server_handler.c` server-side present/Kooima block is fenced
   `XRT_OS_ANDROID || XRT_OS_MACOS`** (the `comp_multi_private.h` include and
   the ~500-line block at 903-1394, plus sibling fence sites) — extend with
   `XRT_OS_LINUX_DESKTOP` and mirror the APPLE `aux_vk` link in
   `ipc/CMakeLists.txt`.
4. The Linux service orchestrator stubs (`service_orchestrator.c:1634+`) are
   fine for MVP (no child auto-spawn) — note they aren't even compiled on
   Linux; `targets/service/CMakeLists.txt` has no Linux source arm and
   `main.c`'s non-macOS path never calls them.

**Done when:** a handle/hosted app runs out-of-process against
`displayxr-service` on Linux.

### Phase 3 — `XR_DXR_xlib_window_binding`

**Phase 3a — extension + app-window path, build-green on CI ✅ (done, #660).**
`_handle` apps can bring their own X11 window. Delivered:

- **Extension** — `XR_DXR_xlib_window_binding.h` (spec:
  `docs/specs/extensions/XR_DXR_xlib_window_binding.md`).
  `XrXlibWindowBindingCreateInfoDXR { type, next, Display* xDisplay, Window
  window }`, type value 1004999200 (decade 200–209 claimed in the openxr_includes
  README registry). Xlib API in, XCB inside: the runtime converts via
  `XGetXCBConnection()` (libX11-xcb) and reuses the Phase 1
  `comp_vk_native_xcb_handle` + XCB surface arm unchanged. Texture-class
  handoff, transparency, and window-space layers are explicit spec-v1
  non-goals.
- **oxr** — registered in `oxr_extension_support.h` (gated `XRT_OS_LINUX &&
  !XRT_OS_ANDROID` — NOT `XR_USE_PLATFORM_XLIB`, which tracks the removed GLX);
  consumed in `oxr_session.c`'s Vulkan branch, packing the pair into
  `comp_vk_native_xlib_handle` for the compositor's type-erased hwnd param.
- **Compositor** — `comp_vk_native_compositor.c`'s Linux window block gains the
  app-window arm (`owns_window=false`, no self-create); the new
  `comp_vk_native_window_xcb_wrap_app_window()` derives the XCB connection, and
  `..._query_geometry()` seeds + per-frame-polls the swapchain extent for
  app windows (no ConfigureNotify — the app owns event selection).
- **st_oxr Linux CMake arm** — Phase 1 built `comp_vk_native` but never wired
  it into st_oxr on Linux (`XRT_HAVE_VK_NATIVE_COMPOSITOR` was Windows/
  macOS/Android-only), so no Linux app could actually reach the in-process
  compositor. Fixed — this arm is load-bearing for hosted (Phase 1b) too.
- **Validation vehicle** — `test_apps/cube_handle_vk_linux` (app-owned Xlib
  window + the binding; render path shared with the hosted legacy cube), built
  by `build_linux.sh --apps` on CI. Deps: `libx11-dev` (Xlib) on top of the
  existing `libx11-xcb-dev`.

**Phase 3b — on-screen validation (pending Linux+GPU hardware, with Phase
1b).** Run `./build/run_cube_handle_vk_linux.sh` on a real box: confirm the
runtime presents into the app's window, resize tracking via the geometry poll,
and the window-position channel for interlacing phase (window metrics currently
take the display-scoped fallback for app windows). The per-frame
window-position contract is the conceptual hard part on Wayland; X11/XCB
supplies it cleanly.

**Phase 3c — mixed 2D/3D display-zones (ADR-027, #778).** The region paradigm
(a flat 2D region — e.g. an avatar's speech bubble — inside a 3D scene) works on
Linux **through the runtime's software composite**: `vk_composite_local_2d`
(`comp_vk_native_compositor.c`) is platform-agnostic and already flattens
`XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR` layers over the weave on the `vk_native`
path — no platform guard, at parity with Metal/D3D11. The zone extensions
(`XR_DXR_local_3d_zone` / `XR_DXR_display_zones` / `XR_DXR_view_rig`) are
advertised on Linux, so an app just has to submit the Local2D layer. Test vehicle:
`cube_zones_vk_linux` (two 3D cube zones + an always-on amber Local2D strip). The
earlier avatar-on-Linux symptom ("only the projection layer renders, the speech
bubble is missing") was **app-side** — the Linux avatar deferred the Local2D
bubble to Phase 3; it is not a runtime gap.

**Hardware per-zone lens switching is SDK-blocked, not wired.** Real
`zone_grid > 1×1` hardware zones need (a) an XCB client-area screen-anchor helper
in `vk_sync_zone_mask_to_dp` (today `#ifdef XRT_OS_WINDOWS`-only) AND (b) srSDK
Linux per-zone interlacing phase — which srSDK 1.0.0 does not expose (no per-zone
weave, no decoupled phase-origin; the LeiaSR#85 gap). So the Leia Linux DP
intentionally reports no zone caps (`get_local_zone_caps` unwired, `TODO(Track
B)`) and the software composite is the sole 2D-in-3D path on Linux until the SDK
gains a per-zone phase surface.

### Testing on a Linux box (on-screen / Phase 1b–3b)

Needs a Vulkan GPU + an X server (X11 session or XWayland). No Leia hardware —
sim-display weaves anaglyph/SBS to a normal monitor. One command:

```
./scripts/run_linux_demo.sh cube-hosted            # Phase 1b: runtime makes the window
./scripts/run_linux_demo.sh cube-handle            # Phase 3b: app passes its own X11 window
./scripts/run_linux_demo.sh mediaplayer --output=sbs   # a demo (sibling ../displayxr-demo-*)
```

It builds the runtime (`build_linux.sh --apps`), wires `XR_RUNTIME_JSON` +
`XRT_PLUGIN_SEARCH_PATH` at that build, and delegates to the target's own run
script. Service/IPC (Phase 2b): `./scripts/build_linux.sh --service`, start
`displayxr-service`, run a client with `XRT_FORCE_MODE=ipc`.

### Phase 4 — Packaging / installer (#705 — tarball MVP SHIPPED)

The MVP stage landed: `scripts/package_linux.sh` produces
`dist/displayxr-runtime-linux-<arch>-<version>.tar.gz` (runtime `.so` +
`displayxr-cli` + `displayxr-service` + sim-display plug-in + install scripts),
and its `install.sh` (from `scripts/linux/`) does a **user-level, no-root**
install: runtime tree → `$XDG_DATA_HOME/displayxr`, OpenXR `ActiveRuntime` →
`$XDG_CONFIG_HOME/openxr/1/active_runtime.json`, plug-in + manifest →
`$XDG_DATA_HOME/DisplayXR/DisplayProcessors/` (the shared discovery root — a
vendor plug-in installer drops its own `.so` + manifest alongside, lower
probe-order wins), and a systemd `--user` unit for `displayxr-service`
(gracefully skipped without a user bus). `sudo ./install.sh --system` targets
`/usr/local` + `/etc/xdg/openxr/1/` (no unit, v1). CI's `Package` job
(`build-linux.yml`) builds in the **26.04 container**, installs from the
tarball, and gates on `displayxr-cli selftest` resolving everything from the
installed XDG paths only. Remaining staged scope (#705): `.deb` → demo
AppImages.

## Decisions

- **XCB first, Wayland later** — window-position queryability (above).
- **sim_display is the bring-up display processor** for all phases — the plug-in
  ABI is platform-neutral so no ABI work was required. The vendor plug-in now
  has a **Linux arm scaffold** (leia-plugin#82, Track A of leia-plugin#81):
  `DisplayXR-LeiaSR.so` with a **stub weaver** (passthrough SBS blit), built
  against runtime v1.28.0, CI-validated on Ubuntu 22.04/24.04/26.04
  (discovery + ABI-green `displayxr-cli selftest`). The SDK-facing seam is
  fixed by the plug-in repo's `docs/leia-linux-sdk-contract.md` (PROPOSED);
  real weaving lands with Track B when the LeiaSR Linux SDK ships. The stub
  probe declines unless `DXR_LEIA_FORCE_PROBE=1`, so sim_display remains the
  default DP on hardware-less boxes.
- **Hybrid mode is out of scope** — `XRT_FEATURE_HYBRID_MODE` is fenced to
  `WIN32 OR APPLE` (`CMakeLists.txt:262`); Linux uses plain in-process or plain
  service mode.
