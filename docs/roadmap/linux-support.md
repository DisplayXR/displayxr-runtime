# Linux Support

Status: **Shipping — code-complete, hardware-validated, and published on every
release.** Linux is a supported platform alongside Windows, macOS and Android.
Phases 0/1a/2a/3a are complete on `main`; hosted + handle
(`XR_DXR_xlib_window_binding`) sessions bring up the native Vulkan/XCB compositor
and render the stereo cube on real Vulkan+X11 hardware (#708 / #706), and the
**Track B real srSDK Vulkan weave is HW-validated on a real panel** (lens
enables). That bring-up pass ran on a 22.04 box, but it long predates the
current packaging (#1656/#1659) and the GNOME 42 entry point (#1677), so it is
**not** evidence for either — see [Supported
releases](#supported-releases). Runtime **v1.28.0** was the first tag
with complete Linux support. **Distribution:** a `displayxr-runtime_<ver>_amd64.deb`
is built on every PR and attached to every `v*` GitHub Release (#781), alongside
the user-level tarball from `scripts/package_linux.sh` (#705/#713); the `.deb` is
built with `libwayland-dev` + `libdbus-1-dev` like every other Linux artifact, so
it carries the Wayland present path and the #817 window-geometry provider (only
`libdbus-1-3` lands in its derived `Depends` — no `wl_*` symbol is referenced, so
`--as-needed` drops `-lwayland-client`).

**Supported releases and what CI actually gates:** see [Supported
releases](#supported-releases) below.

**Still open** — none of these gate an in-process `_handle`/`_hosted` app, which
is the shipping path: Phase 2b service-side render (**#710**, service/IPC mode
only); windowed-3D phase origin (**#729/#730**, twin of Windows #85); Wayland
windowed weaving (**#817**) — the extension + runtime consumer are validated
live on GNOME 50 / Ubuntu 26.04 (2026-09-19), and the logical→device conversion
that actually feeds the phase landed with **#1596** (plus **#1595**, which
degrades a session to flat 2D rather than weaving into a resample), but none of
that is hardware-validated: the weave *phase and scale* still need a 3D panel
in a Wayland session (see [Wayland](#wayland)); X11 is unaffected; the
deployment target (Ubuntu 26.04 + Intel Arc), blocked on hardware; and the Track B
shippable re-pin onto a merged `sr-sdk-v*` tag. Note that vendor-side weave
maturity is tracked separately from runtime readiness — the runtime hands the
display processor a correct atlas on Linux exactly as it does elsewhere.

**Drag phase-lock (#1588)** — with windowed weaving working (#1579/#1585), *dragging*
the window walks the interlace phase across the lens pitch and the 3D stutters. The
runtime half has landed: the Vulkan DP variant carries `snap_window_rect` (the twin of
D3D11 slot 18, appended per ADR-020 — no ABI bump), `XR_DXR_weave` is advertised on
desktop Linux as a **snap-only** extension so `xrWeaveSnapWindowRectDXR` reaches that slot
both in-process and over IPC. None of it changes behaviour until a vendor implements the
slot — an absent slot is identity everywhere. The remaining half is the **plug-in** (the
actual lattice math, LeiaSR#224); the **app-side drag** (an undecorated X11 window owning
its own move, since a client cannot hook Mutter's) is what drives it.

A compositor-side re-quantisation of the fed origin was tried and **removed**: on the DS1
with a real Leia DP it diverged from the true window origin on 12 of 13 moves, by up to
2 px, which is phase error against the lens rather than protection from it. The rule it
established is worth keeping in mind anywhere else this comes up — the runtime feeds the
window's TRUE origin and never quantises it, because only the window owner may move a
window and therefore only the window owner may snap one.
Windows does not have this problem because the plug-in snaps the window during
`WM_MOVING`; X11 gives no such hook, which is why the app has to own the drag.

**Display scaling limits drag snapping, and one scaled display affects all of them.** XWayland
runs the whole X screen at one global scale: the ceiling of the *most-scaled* output
(Mutter's `xwayland-native-scaling`). An X11 window origin can only land on multiples of
that scale. So *any* output above 100% makes odd device pixels unreachable everywhere. That
includes a 166% laptop next to a DS1 at 100%. With the DS1 itself at 200%, the panel's own
size checks all still pass (its X11 rect equals its native mode), which is what hid it. The DS1
measurement: 54% of snapped targets were odd, 0% of landed positions were, and the drag
stuttered. At 100% on every output: ~50% odd reached, 92% of snaps landed exactly, and
correct by eye. **Every output at 100% is the supported configuration today.** Other
configurations are detected (RandR vs DRM on every output) and reported by
`displayxr-cli info` → *X11 coordinate space*. Under a quantum the runtime snaps on the
reachable lattice: coarser drag steps, phase kept. When X11 pixels are not panel pixels
at all, the snap is refused. The app helper checks every landing and says so when the
display drops its snaps. Weaving itself is never stopped for this. Full mechanism, the
solver's blind spot, and the assessment of what a truly scale-proof design would take
(a fixed-origin surface, i.e. the shell's compose model):
[linux-display-scaling.md](../reference/linux-display-scaling.md).

## Supported releases

**Ubuntu 22.04, 24.04 and 26.04 are supported. On 22.04, use an X11 session:
GNOME 42 does not implement the Wayland fractional-scale protocol
(`wp_fractional_scale_v1`), so a native-Wayland window cannot be guaranteed 1:1
with the panel on a scaled desktop and the runtime degrades it to flat 2D
rather than weave into a resample. Packages are built on 22.04 and CI-verified
to install and headlessly self-test on all three releases. Hardware validation
to date is on 24.04 and 26.04 — no panel has been run against the 22.04-built
packages, and the GNOME 42 extension entry point has never run on a GNOME 42
desktop.**

That statement is the one to copy; every other page here should say the same
thing. Two things it deliberately does not claim, because nothing enforces
them: the vendor plug-in's own glibc floor is *measured* at its 22.04 CI
baseline rather than asserted by a check, and the by-hand 22.04 weave and
tracking pass (#1663) is outstanding.

**What CI actually gates, precisely (#714/#722/#1656/#1659).** Earlier revisions
of this page said a tri-LTS matrix "is a required check on the runtime and all 5
demos". Half of that was an overclaim, so here is the real shape:

| | Tri-LTS matrix | Required status check? | What it proves |
|---|---|---|---|
| Runtime (`build-linux.yml`) | `DebInstall (Ubuntu 22.04 \| 24.04 \| 26.04)` | **No** | Installs the `.deb` + tarball in pristine containers of all three and runs `ldd -r` + env-free `displayxr-cli selftest`. Not in the ruleset, but `DebRelease` `needs:` it, so a red leg blocks the release asset. |
| Runtime, required Linux checks | — | **Yes**: `Selftest`, `Service`, `Package` | `Package` builds in the `ubuntu:22.04` container and installs from the tarball; `Newest` (26.04) covers the new toolchain and is *not* required. |
| Demos (all 5) | `Build (Ubuntu 22.04 / 24.04 / 26.04)` | **Yes** | Build-green only — the demos are not run on CI (no display). Four expose the three legs directly; one aggregates them into a single required `Build Linux` check. |

So on the runtime the tri-LTS coverage gates the *release artifact*, not the
merge; on the demos it gates the merge but proves compilation, not execution.
Every claim in the table above is CI evidence: build, package, install, and a
headless self-test. Nothing in it puts pixels on a panel.

## TL;DR

DisplayXR's **non-presentation substrate is already Linux-ready** — it survived
the Monado fork intact and still compiles behind the non-Windows `#ifdef`/CMake
branches. The **entire gap is presentation**: no native compositor can put
pixels on a Linux display, plus there's no Linux build tooling. So the port is
narrow and well-bounded, sequenced as: prove the substrate headless (Phase 0),
then add an in-process Vulkan/XCB compositor (Phase 1), then the service/IPC
path (Phase 2), then a window-binding extension for app-owned windows (Phase 3).
A **second present path for Wayland sessions** landed alongside Phase 3 —
`XR_DXR_wayland_surface_binding` plus the #817 window-geometry provider — and
has its own section: [Wayland](#wayland). Its extension and runtime consumer
are validated live (GNOME 50 / Ubuntu 26.04, 2026-09-19); weave phase is not,
because that needs a 3D panel. The rule that governs that path: **Wayland
reports geometry in logical coordinates, everything the weaver consumes is
device pixels, and the conversion is the runtime's to perform at its Wayland
boundary** (#1596) — and where the buffer cannot reach the glass unresampled,
the session degrades to flat 2D rather than weaving into the resample (#1595).

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
(`build-linux.yml`) builds in the **22.04 container** (the oldest supported
release — #1656; the newest-toolchain coverage it used to give moved to the
`Newest` job's 26.04 container), installs from the tarball, and gates on
`displayxr-cli selftest` resolving everything from the installed XDG paths only.
Remaining staged scope (#705): `.deb` → demo AppImages.

**One artifact for 22.04 / 24.04 / 26.04 (#1656).** A binary's glibc floor is
its build host's, so both release artifacts are built in the **oldest**
supported container. v2.19.1 was built on 24.04 (`GLIBC_2.38`,
`GLIBCXX_3.4.31`) and declared an unversioned `libc6`, so apt installed it on
22.04 and every app failed at `dlopen`. `package_deb_linux.sh` now derives
`Depends` with `dpkg-shlibdeps` (versioned floors), refuses a `DT_NEEDED`
outside its cross-release `STABLE_SONAMES` list or a glibc floor above
`DXR_DEB_MAX_GLIBC`, and the `DebInstall` matrix installs + runs the `.deb` in
pristine 22.04 / 24.04 / 26.04 containers (`ldd -r`, dependency-name existence,
env-free `displayxr-cli selftest`) before `DebRelease` attaches it to a release.
The tarball, which has no dependency metadata, records the floor in a
`GLIBC_FLOOR` file that its `install.sh` enforces. The GNOME Shell extension
used to be the one part that did not reach 22.04 — it is an ES module, which
only GNOME 45+ loads, so on GNOME 42 it installed and never loaded. It now
ships **both** entry-point forms over one shared `lib.js` and the login script
puts the one the running shell can parse in place (#1663); `shell-version` is
`42`–`50` and CI asserts it covers every release the `.deb` installs into.
Loading on GNOME 42 is not hardware-validated yet — no 22.04 desktop box here;
the by-hand pass is written out in the extension's `README.md`. Independently of
whether it loads, GNOME 42 has no `wp_fractional_scale_v1`, so a native-Wayland
window on a scaled 22.04 desktop cannot present a 1:1 buffer and the runtime
degrades it to flat 2D (see *Refuse rather than resample*, #1595). **X11 is the
recommended session on 22.04.**

### Conformance — the Linux CTS arms (#1527)

Until #1527 the platform had **no conformance coverage at all**: `build-linux.yml`
builds and runs the hardware-free `displayxr-cli selftest`, and nothing ran the
Khronos suite. The by-hand NVIDIA validation above is real evidence but not a
repeatable gate.

`cts.yml` now carries a Linux leg — `build-linux-cts` → `run-linux` — running
`vulkan` and `vulkan2` (the whole Linux matrix; the platform is Vulkan-only) on
GitHub-hosted `ubuntu-latest`, against Mesa **lavapipe** under **Xvfb**, with
the `sim-display` plug-in. Hardware-free, at the same pinned CTS
(`openxr-cts-1.1.63.0`) and the same test specs as the Windows arms, so the
result files assemble into one submission package.

Both arms **gate on the hosted lane** (lavapipe under Xvfb): a red Linux arm
fails the lane exactly like a red Windows one, and the whole hosted matrix —
5 Windows + 2 Linux — is now gated.

They earned that the same way every Windows arm did, with a whole-suite zero-red
run rather than with "no known blocker": `vulkan` 40062 assertions / 0 failures
/ 0 errors and `vulkan2` 40046 / 0 / 0, once #1577 (issue #1576) filtered the
`xrGetVulkanDeviceExtensionsKHR` string on Linux and took `vulkan` from 62
errors to none. Linux being **Preview rather than GA** was the other reason for
holding them ungated; it stopped applying once they were green, because an arm
that passes and does not gate just teaches the lane to ignore it. The mechanism
is still one string, `EXPERIMENTAL_LINUX` in `cts.yml`'s `plan` job — now empty;
put an arm's name back to exempt it, and record why. Per-arm evidence:
`docs/roadmap/cts-windows-handoff.md` § *Linux arms*.

What this tier does **not** cover, and what still needs the real-GPU runner
#1527 originally proposed: anything that depends on a real driver stack or a
real panel — NVIDIA/AMD/Intel driver-specific behaviour, the frame-timing test
(quarantined on any software tier, because the budget is unmeetable on a CPU
rasterizer), the direct-scanout path (`DXR_LINUX_DIRECT_SCANOUT=1`), Wayland
(the lane is X11/XCB only), and the installed `.deb` / tarball runtime path
(the lane runs the dev build tree). Hosted first, hardware after.

Running the same script by hand, on a bench box with a real GPU and a real X
session, is the supported path — drop `--software` and the quarantine list goes
with it:

```bash
./scripts/build_linux.sh
./scripts/fetch_build_cts.sh --apt
./scripts/run_cts.sh -g vulkan --scope full --conformance-layer
```

Full detail, including exactly what is and is not ported from the Windows
harness: `docs/roadmap/cts-windows-handoff.md` § *Linux arms*.

## Wayland

The phase plan above is X11/XCB throughout — deliberately, because X11 hands a
client its own absolute window position and Wayland does not (see *Decisions*).
Wayland is now a **second, parallel present path**, not a later phase: an app
in a Wayland session binds its own `wl_display*`/`wl_surface*` and the same
`comp_vk_native` compositor presents through a `VkWaylandSurfaceKHR`. Internal
shorthand for this work is **WS3b** (runtime#757) — a tag that appeared in five
source comments and, until this section, in no document at all.

### What ships

- **Present path** — `XR_DXR_wayland_surface_binding`. `oxr_session.c` packs
  the app's `wl_display*`/`wl_surface*` pair into a
  `comp_vk_native_wayland_handle`, the compositor takes the Wayland arm of its
  Linux window block (`use_wayland`, `owns_window=false`, no XCB window, no
  xdg-shell — the app owns the surface lifecycle), and
  `comp_vk_native_target.cpp` builds the `VkWaylandSurfaceKHR` directly. Built
  when `XRT_HAVE_WAYLAND` (pkg-config `wayland-client` + Vulkan); the CMake
  config summary prints `WAYLAND:`.
- **Window geometry (#817)** — a GNOME Shell publisher
  (`contrib/gnome-shell/window-geometry@displayxr.org`) reports every window's
  global rect on the session bus, and the runtime consumer
  `comp_vk_native_wl_geom` (built when `XRT_HAVE_WAYLAND && XRT_HAVE_DBUS`;
  summary line `DBUS:`) feeds it into the existing
  `get_window_metrics → vk_update_present_origin → DP set_present_origin`
  chain as a third window source next to the two XCB ones. Without it Wayland
  weaves display-scoped.
- **Shipped in the artifacts** — the `.deb` and the tarball are both built with
  `libwayland-dev` + `libdbus-1-dev` (#1565), so the released runtime carries
  both. Only `libdbus-1-3` lands in the `.deb`'s derived `Depends` — no `wl_*`
  symbol is referenced, so `--as-needed` drops `-lwayland-client`.
- **Validated live** — the GNOME Shell extension and the runtime-side consumer
  were exercised end to end on **GNOME 50 / Ubuntu 26.04 on 2026-09-19**
  (publisher owns the bus name, payload parses, consumer resolves the app's own
  window by PID). That is the plumbing, not the picture: see the constraint on
  weave phase below.

### Constraints (all of them current, none of them bugs to file twice)

- **Wayland reports geometry in LOGICAL coordinates; everything the weaver
  consumes is DEVICE pixels, and the conversion is ours to perform.** The
  vendor ships the same guarantee from its side — every geometry its SDK
  reports or accepts is device pixels, unconditionally
  (`docs/reference/xrt_plugin_iface.md`,
  [`XR_DXR_weave`](../specs/extensions/XR_DXR_weave.md)) — so there is exactly
  one place in the stack where the two spaces meet, and it is the runtime's
  Wayland boundary. Since **#1596** that boundary **converts** instead of
  refusing: the geometry payload has carried the window's `monitor` rect *and*
  Mutter's fractional scale since schema v1 (they were simply never read), so
  `comp_vk_native_wl_geom` puts the logical rect through
  `auxiliary/util/u_wayland_geom.h` and hands the compositor device pixels —
  origin relative to the window's own monitor, which
  `get_window_metrics` turns back into an absolute rect by adding the runtime's
  own resolved panel origin (ADR-033). A fractionally-scaled monitor is
  therefore a supported configuration for the phase feed; the earlier #1557
  behaviour (refuse any rect from a non-1.0 monitor) is gone, and with it the
  reason a scaled desktop produced no present origin at all. Note that
  `wl_output.scale` is **not** the conversion factor — it is an integer by
  protocol and advertises 2 for the measured box's 1.6667 output, a 20 % error
  that looks entirely plausible. The two honest sources are
  `wl_output.mode ÷ xdg_output.logical_size`, or a compositor-published
  fractional scale (`wp_fractional_scale_v1`; GNOME's
  `Meta.Display.get_monitor_scale()`, which the geometry extension already
  forwards). What a scaled desktop still costs is not the *units* but the
  *resample* — see the next bullet. **X11 is unchanged** and still refuses the
  phase at a non-panel-native root
  (`vk_x11_present_origin_is_panel_native`): root coordinates there are
  whatever the server's root happens to be, and under XWayland at a non-unit
  scale that is a fiction with no published factor to undo.
- **Refuse rather than resample (#1595).** A resampled weave is not degraded
  3D. A ~1-pixel-period interlace run through a scaling filter is a uniform
  double image across the whole surface with no vantage point where it
  resolves, and nothing the runtime publishes downstream can undo it — flat 2D
  is correct content in the wrong dimensionality, legible and recoverable, so
  degrading is strictly better. `vk_linux_update_surface_not_1to1()` enforces
  that on the Wayland arm. It fires on a **transition** when either (a) the
  window's monitor is not the 3D panel, or (b) the buffer being presented is
  not the window's DEVICE extent, and then does exactly what the Android
  precedent `vk_android_update_container_scaled()` does: one WARN naming both
  extents and which of the two reasons fired,
  `request_display_mode(false)` plus the display processor's
  `on_pause`/`on_resume` pair (the Leia DP only re-asserts the lens from inside
  a weave, #1039), and `vk_compute_effective_layout` collapsing the frame to
  tile 0. Grep a log for `NOT_1TO1:` — and for `NOT_1TO1 cleared:`, because the
  degrade is reversible rather than sticky. The clear edge restores the
  session's **own** last hardware choice, not 3D, and requests made during the
  episode are held until it clears: under the Leia srSDK (LeiaSR #266) the
  degrade's 2D request takes the lens preference from the weaver for good, so
  that restore is the only thing that brings the lens back
  ([lens-preference ownership](../specs/vendor/lens-preference-ownership.md)). **Three states, never two:**
  cannot-be-1:1 degrades, is-1:1 weaves, *don't know* keeps the current state —
  so a session without the `window-geometry@displayxr.org` service has no
  destination extent to compare against and behaves exactly as it did before
  the gate existed. The Wayland present-origin feed is gated on the same flag,
  so a degraded session cannot leave a stale phase latched on the DP.
  Deliberately **Wayland-only**: X11's only available signal is
  `display_desktop_rect_is_panel`, which is false on every dev box by
  construction (sim_display declares a 1920x1080 panel that no real desktop
  rect matches), so gating the weave on it would put every X11 sim session into
  flat 2D.
- **Fullscreen-on-panel is the validated shape.** A Wayland surface has no
  intrinsic size and no XCB geometry to poll, so the app declares its buffer
  size (`XrWaylandSurfaceGeometryDXR` / `xrSetWaylandSurfaceGeometryDXR`, spec
  v2) and the runtime follows resizes from that push rather than from a poll;
  absent a declaration the target is created at the panel dimensions
  (`settings.preferred.width/height`), which on Wayland does not mis-size the
  window but resizes it. A windowed surface now gets a converted, device-pixel
  phase anchor (#1596) rather than none — but it only weaves if its buffer
  equals its device extent, which is the 1:1 gate above, and none of the
  windowed case has been seen on a panel.
- **Position comes from the geometry service, never from the client.** Wayland
  has no `xcb_translate_coordinates`; if the extension is absent, disabled, or
  the session bus is unavailable, the runtime degrades to display-scoped. The
  full ladder is in the spec.
- **The extension takes effect at the next login.** Wayland cannot hot-reload
  GNOME Shell, so installing or updating the publisher requires a log out/in
  before `gnome-extensions enable` has any effect on a running session.
- **The phase feed now EXISTS on Wayland, and is still NOT validated.** Before
  #1596 nothing on this path ever reached the DP's `set_present_origin`: the
  measured session on 2026-09-20 (GNOME 50 / Ubuntu 26.04, a real Leia DP)
  produced **zero** `present origin:` lines in the whole run, because the
  provider refused every rect from the 1.6667-scaled monitor. It is fed now, in
  device pixels. What is *proven* is the conversion itself: it is pure
  arithmetic in `u_wayland_geom.h`, with no Wayland or platform dependency
  precisely so a host test can pin it, and `tests/tests_aux_wayland_geom.cpp`
  does — 22 cases against **both** of the measured box's scales, because an
  integer-only implementation passes every 2.0 case and fails every 1.6667 one.
  The 1:1 gate's *decision* is pinned by the same test. What has NOT been
  re-measured since #1596 is a Wayland run against a real panel — neither that
  `present origin:` now appears, nor the phase and scale it produces. Treat the
  chain as built and unit-tested, not as observed. Everything else here was
  proven on sim_display and on the D-Bus
  wire, and sim_display's anaglyph/SBS output degrades gracefully under
  resampling and a wrong origin, so it cannot establish geometric correctness
  at all — that needs a real 3D panel in a Wayland session.
- **PID matching assumes in-process.** The consumer matches windows owned by
  `getpid()`; service/IPC mode needs the client PID plumbed through (#817
  follow-up).

- **A window that spans the 3D panel and another monitor weaves only on the
  panel (#1654).** The weave is correct only where the lens is, and a spanning
  window is still ONE surface with ONE weave — so the half on an ordinary
  monitor would show the lenticular pattern as a double image. On X11 the
  weaver is window-bound and clips itself; the Wayland weaver is windowless (it
  is handed the surface plus a present origin), so the runtime paints the
  off-panel part itself: `u_wl_offpanel_bands()` cuts the window rect against
  the panel rect — in the same device pixels the present origin is expressed
  in, so the bands and the weave phase cannot disagree — and
  `vk_composite_offpanel_2d()` blits one view's tile from the atlas over each
  band after the weave. Flat 2D there, woven 3D inside the panel, one seam.
  Two cases it deliberately does NOT rescue: a window whose *majority* is on
  the other monitor (its buffer then follows that monitor's scale, so the part
  over the panel is resampled and the whole session degrades to 2D — the
  refuse-rather-than-resample rule, #1595), and a compositor that is not
  publishing geometry at all. Kill switch `DXR_WAYLAND_SPAN_2D=0`.

### Where the detail lives

- `docs/specs/runtime/wayland-window-geometry.md` — the #817 provider: D-Bus
  interface, JSON schema + versioning, degradation ladder, and the packaging
  contract (the publisher is a shared asset; exactly one installed owner via
  the `displayxr-window-geometry-publisher` virtual package).
- `contrib/gnome-shell/window-geometry@displayxr.org/README.md` — install,
  verify with `gdbus call`, and the distributor notes.
- `docs/specs/extensions/XR_DXR_wayland_surface_binding.md` — the extension
  spec, now on `main` and current at **spec version 2** (the version that added
  `XrWaylandSurfaceGeometryDXR` + `xrSetWaylandSurfaceGeometryDXR`, so an app
  declares its own buffer size). Header:
  `src/external/openxr_includes/openxr/XR_DXR_wayland_surface_binding.h`, noted
  in `docs/specs/extensions/index.json`. Its §4.6/§4.7 carry the app-side
  output-matching recipe in device pixels.

## Decisions

- **XCB first, Wayland later** — window-position queryability (above). The
  Wayland answer now exists (#817): the compositor publishes per-window global
  geometry over the session bus (GNOME Shell extension in
  `contrib/gnome-shell/`) and `comp_vk_native_wl_geom` feeds it into the same
  present-origin chain — `docs/specs/runtime/wayland-window-geometry.md`.
- **sim_display is the bring-up display processor** for all phases — the plug-in
  ABI is platform-neutral so no ABI work was required. The vendor plug-in now
  has a **Linux arm** (leia-plugin#82, Track A of leia-plugin#81):
  `DisplayXR-LeiaSR.so`, first shipped with a **stub weaver** (passthrough SBS
  blit) built against runtime v1.28.0, CI-validated on Ubuntu 22.04/24.04/26.04
  (discovery + ABI-green `displayxr-cli selftest`). The **real-weaver track is
  now built and packaged in an `ubuntu:22.04` container too, and installed on
  all three releases** (leia-plugin#260), on the same portable-packaging pattern
  as the runtime. Two limits are worth stating plainly: the vendor's own Linux
  runtime uses 22.04 as its CI build baseline with a **measured** glibc floor of
  2.35 and a gcc 11 SDK build — measured, because no check asserts it, so it can
  drift without failing anything — and no 22.04 box has run the real weaver on a
  panel. The SDK-facing seam is
  fixed by the plug-in repo's `docs/leia-linux-sdk-contract.md` (PROPOSED);
  real weaving lands with Track B when the LeiaSR Linux SDK ships. The stub
  probe declines unless `DXR_LEIA_FORCE_PROBE=1`, so sim_display remains the
  default DP on hardware-less boxes.
- **Hybrid mode is out of scope** — `XRT_FEATURE_HYBRID_MODE` is fenced to
  `WIN32 OR APPLE` (`CMakeLists.txt:262`); Linux uses plain in-process or plain
  service mode.
