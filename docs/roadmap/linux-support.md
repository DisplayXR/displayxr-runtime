# Linux Support

Status: **Phase 0 in progress** (headless bring-up). Windows, macOS, and Android
ship today; Linux is the remaining platform.

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
   - `gl` configures on Linux (`XRT_HAVE_OPENGL`) but has no GLX/EGL-on-X11
     window path — GLX was deliberately removed (`CMakeLists.txt:215`).
2. **No self-created window for the `_hosted` class.** Only `comp_window_macos`
   and `comp_window_android` survive — no `comp_window_xcb/wayland/direct`.
3. **No Linux window-binding extension.** Only `XR_EXT_win32_window_binding` and
   `XR_EXT_cocoa_window_binding` exist; the `_handle`/`_texture` classes have no
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

**Delivered here:**
- `scripts/build_linux.sh` — builds the runtime, `displayxr-cli`, and the
  sim_display plug-in; stages the plug-in + a JSON manifest; runs
  `displayxr-cli selftest` (the exact hardware-free gate CI runs) + `info`.
- `CMakeLists.txt` — relaxed `find_package(udev REQUIRED)` → `find_package(udev)`
  so a box without `libudev-dev` configures (the legacy VR prober gated by
  `XRT_HAVE_LIBUDEV` simply turns off; the display processor comes from the
  plug-in loader, not udev).

**Done when:** `./scripts/build_linux.sh` exits green with a passing
`displayxr-cli selftest` on a stock Ubuntu box. `displayxr-cli` links the
no-compositor instance (`target_instance_no_comp`), so Phase 0 needs neither a
native compositor nor the OpenXR loader.

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

- Add `LINUX`/`UNIX` to the `vk_native` CMake gate
  (`compositor/CMakeLists.txt:339`, `vk_native/CMakeLists.txt:8`).
- Add a `VK_KHR_xcb_surface` branch to the `#else` in
  `comp_vk_native_target.cpp:887` (PFN already loaded); wire matching
  resize/swapchain-recreate (`comp_vk_native_compositor.c`).
- Write `comp_vk_native_window_xcb` for the `_hosted` class (runtime
  self-creates the window — no new extension needed yet).
- First runnable target: `cube_hosted_vk_linux`, sim_display DP.

**Done when:** a hosted cube renders into a runtime-created XCB window with
sim_display weaving (e.g. anaglyph/SBS output).

### Phase 2 — Service / IPC path

Mostly already wired (Linux mainloop, unix sockets, SCM_RIGHTS, systemd). The
service compositor reuses Phase 1's window backend, so it **depends on Phase 1**.

- Stand up `displayxr-service` (`./scripts/build_linux.sh --service`) and a
  client with `XRT_FORCE_MODE=ipc`; validate swapchain-image fd-passing over the
  unix socket.
- The Linux service orchestrator is currently no-op stubs
  (`service_orchestrator.c:1634+`) — fine for MVP (the shell is deferred even on
  macOS): just `ipc_server_main` + manual/systemd start, no child auto-spawn.

**Done when:** a handle/hosted app runs out-of-process against
`displayxr-service` on Linux.

### Phase 3 (defer) — `XR_EXT_xlib_window_binding`

Needed only when real `_handle`/`_texture` apps bring their own window. Define
`XrXlibWindowBindingCreateInfoEXT` (`Display*` + `Window`) and the oxr plumbing,
mirroring `XR_EXT_win32_window_binding`. The per-frame window-position contract
is the conceptual hard part on Wayland; X11/XCB supplies it cleanly.

## Decisions

- **XCB first, Wayland later** — window-position queryability (above).
- **sim_display is the bring-up display processor** for all phases — the vendor
  plug-in (Leia SR) has no Linux DP yet, and the plug-in ABI is platform-neutral
  so no ABI work is required.
- **Hybrid mode is out of scope** — `XRT_FEATURE_HYBRID_MODE` is fenced to
  `WIN32 OR APPLE` (`CMakeLists.txt:262`); Linux uses plain in-process or plain
  service mode.
