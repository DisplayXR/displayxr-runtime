# Linux demo-port playbook

How to bring a `displayxr-demo-*` (or any standalone OpenXR app) to Linux,
correct-by-construction. Companion to the **Demo-repo dev-build rule** in the
root `CLAUDE.md` and the platform port in [`docs/roadmap/linux-support.md`](../roadmap/linux-support.md).

## Two facts that shape every port

1. **Every demo is Vulkan** (`*_vulkan_utils` / `VulkanRenderer` / `tile_renderer`)
   — so **no OpenGL-on-Linux compositor backend is required**; every demo rides
   the Phase 1 `vk_native` + `VK_KHR_xcb_surface` path. No renderer work.
2. **Every demo is a _handle_ app, NOT hosted.** All five create their own
   OS window (`CreateWindowEx` / `NSWindow`) and pass its handle via
   `XR_DXR_win32_window_binding` / `XR_DXR_cocoa_window_binding`. The Linux
   equivalent **landed with runtime Phase 3a** (#660):
   `XR_DXR_xlib_window_binding` (`Display*` + `Window` at xrCreateSession) +
   the compositor's app-provided-window branch — spec at
   `docs/specs/extensions/XR_DXR_xlib_window_binding.md`, working example at
   `test_apps/cube_handle_vk_linux`. So faithful ports are unblocked at the
   API level; on-screen behavior is still pending Phase 3b hardware
   validation. The **hosted-NULL fallback** (pass no window; the runtime
   self-creates one) remains a valid interim "does it render" check.

So a demo port = **build tooling + an app-side Linux windowing arm**. The
windowing arm is the part gated on Phase 3; the build tooling (below) can be
written anytime.

## Two halves — do the first now, the second on hardware

- **Build-green (no hardware, CI-validatable):** add the Linux build script +
  OpenXR-loader provisioning + a CMake Linux arm, and compile on `ubuntu-latest`.
  This is the parallelizable bulk of the work and needs no GPU/display.
- **On-screen (needs the Linux runtime + a GPU + X server):** run the demo
  against the dev runtime and confirm it presents/weaves. Gated on runtime
  **Phase 1b** (on-screen present proven). Don't block build-green work on it.

## Two demo shapes — check this FIRST (it decides most of the work)

The five demos split into two structures, and it changes the port from "one CMake
line" to "author a new entry point." All ports 2026-07 landed build-green; the
split was: **mediaplayer = shared-src; avatar / modelviewer / gaussiansplat /
earthview = per-platform.**

- **Shared-src (mediaplayer only):** one cross-platform `src/` (SDL harness) with
  a thin per-OS window shim. The Linux port really is "pure build tooling" — a
  CMake `else → linux` arm + the loader fallback. This is the *exception*.
- **Per-platform (the other four):** `macos/main.mm` (Cocoa) + `windows/main.cpp`
  (Win32) with the Vulkan+OpenXR bootstrap **inside** each, and **no shared
  cross-platform main**. There is nothing for a CMake arm to point at, so you
  **author a new `linux/<demo>_handle_vk_linux/main.cpp` from scratch**: a compact
  **hosted-NULL** OpenXR+Vulkan harness that drives the demo's shared renderer lib
  (`ModelRenderer` / `gs_renderer` / `TileRenderer`). Scaffold it from the
  runtime's `test_apps/cube_handle_vk_linux` (or `cube_hosted_legacy_vk_linux`),
  strip the cube's own VkRenderer + MoltenVK-portability bits, and wire in the
  demo's renderer (whose `renderEye()` usually takes a swapchain `VkImage`
  directly — no framebuffer machinery). Windowing = hosted-NULL for build-green;
  vendor `XR_DXR_xlib_window_binding.h` and leave a `TODO(Phase 3b)` to switch to
  the real app-owned window once hardware validation happens. **This new main is
  the bulk of the work, not the build tooling.**

## Recipe (mirror the demo's `build_macos.sh`)

Each demo already has `scripts/build_macos.sh` that builds the runtime-agnostic
app + provisions the OpenXR loader + bundles Vulkan. The Linux script is the
same shape with three swaps:

1. **`scripts/build_linux.sh`** (new) — model on `build_macos.sh`:
   - System Vulkan (`libvulkan-dev`) instead of MoltenVK; **no ICD manifest** —
     the native Linux Vulkan loader finds the system driver.
   - **Provision the OpenXR loader pinned to the repo's vendored
     `openxr_includes` rev** (`XR_CURRENT_API_VERSION`) — never hardcode an SDK
     path. Build from source (`KhronosGroup/OpenXR-SDK-Source`,
     `-DBUILD_WITH_SYSTEM_JSONCPP=OFF`) or `FetchContent` it, exactly like the
     runtime's `scripts/build_linux.sh --apps`. Keep the three pins equal:
     CI (`build-linux.yml`) == dev script == header rev.
   - Emit a `run_<demo>_linux.sh` that sets `XR_RUNTIME_JSON` (dev runtime
     manifest), `XRT_PLUGIN_SEARCH_PATH` (sim-display plug-in dir),
     `OXR_ENABLE_VK_NATIVE_COMPOSITOR=1`, and `SIM_DISPLAY_OUTPUT`.

2. **CMake** — the demo's top-level `CMakeLists.txt` usually gates OS with
   `if(APPLE)/elseif(WIN32)`. Add a Linux arm: `find_package(Vulkan REQUIRED)`,
   the OpenXR loader (`find_package(OpenXR CONFIG)` → `find_library` → parent
   build fallback), and drop `OBJCXX` from `LANGUAGES` on Linux.

3. **`.github/workflows/build-linux.yml`** (new, **non-required** at first) —
   `ubuntu-latest`, apt-install the deps below, run `scripts/build_linux.sh`.
   Promote to a required check once it's reliably green.

### apt deps (Debian/Ubuntu)

```
# Base (every demo):
build-essential cmake ninja-build pkg-config \
libvulkan-dev vulkan-validationlayers glslang-tools

# If the OpenXR loader is built from source with GL/EGL headers present
# (SDL-from-source pulls them in) — see the GLX-flip gotcha below:
libxcb-glx0-dev libxxf86vm-dev

# For a _handle demo that opens its OWN window (SDL) + decodes media:
libx11-dev libxcb1-dev libxcb-randr0-dev libx11-xcb-dev \
libwayland-dev libasound2-dev \
libavcodec-dev libavformat-dev libavutil-dev libswscale-dev   # FFmpeg, mediaplayer
```

**The demos are _handle_ apps** (they open their own window — see the two facts
above), so they DO link a window/graphics toolkit (usually SDL) themselves. The
faithful Linux window arm is Phase-3-gated; until then they run hosted-NULL.

## Gotchas already hit (bake into every port)

Confirmed on **demo #1, mediaplayer** (mediaplayer#30, 4 CI iterations):

- **A FetchContent-retrofit demo needs almost nothing.** The mechanical part is a
  CMake `else → linux` arm + the loader `find_package → FetchContent` fallback,
  both already platform-agnostic. The classic runtime-tree gotchas below are often
  **N/A in demo repos** — they use `__linux__` (not `XRT_OS_LINUX`), vendor stb
  unconditionally, and let SDL supply the window handle. Check before "fixing."
- **GLX-flip (new, subtle):** installing GL/EGL headers (needed to build SDL3 from
  source) makes the OpenXR-SDK-Source loader build **detect GLX** → it pulls in
  `<xcb/glx.h>` and gfxwrapper links `Xxf86vm`. Fix: add `libxcb-glx0-dev
  libxxf86vm-dev`. The runtime CI never hits this (it installs no GL headers).
- **`XrCompositionLayerWindowSpaceDXR` lives ONLY in the cocoa/win32 binding
  headers.** A demo that submits a window-space layer needs a Linux arm of its
  `XrCommon.h` that mirrors the wire-shared `#ifndef`-guarded struct. This is an
  **interim** — swap it for the real Linux binding header now that **Phase 3a**
  (`XR_DXR_xlib_window_binding`) has landed (note: spec v1 does NOT declare the
  window-space layer struct, so keep the `#ifndef`-guarded mirror until a later
  spec revision adopts it): vendor the header, add `kWindowBindingExt`
  + the binding branch in `XrSession.cpp`, and extract the SDL window handle in
  `Window.cpp` (`SDL_GetWindowProperties` → X11 display + window props).
- **`_handle` media path can silently compile out:** if the FFmpeg/X11/Wayland/ALSA
  dev pkgs are missing, the decode path is `#if 0`-ed away with no error. The CI job
  must **assert `pkg-config` found them** (fail loudly), not just build.
- **Loader/header pin drift is org-wide:** the loader is pinned `1.1.43` while
  vendored headers are newer (`1.1.51`) in several repos incl. the runtime. Keep
  `1.1.43` for the loader unless you're deliberately bumping all three pins.

Confirmed across the **4-demo batch** (avatar #22, modelviewer #41, gaussiansplat
#61, earthview — all build-green, 1–4 CI iterations each):

- **GCC-vs-Clang flag/header gotchas are the #1 recurring failure.** Third-party
  libs demote warnings under `if(NOT MSVC)` assuming Clang, which breaks GCC:
  - Niantic **SPZ** `splat-types.h` uses `std::sort` without `#include <algorithm>`
    (libc++ hides it on mac); the patch was MSVC-only → hoist the include-injection
    to **all** toolchains. (gaussiansplat)
  - **tinyusdz** `-Wthread-safety-negative` is Clang-only → GCC errors on the
    unknown flag. Gate it to `CMAKE_CXX_COMPILER_ID MATCHES "Clang"` (still catches
    AppleClang) + `-Wno-error` on `GNU`. (modelviewer)
  - General rule: any Clang-only `-W…` flag or transitively-included header will
    surface on GCC. Gate on the compiler ID, not `NOT MSVC`.
- **`displayxr::common` builds on Linux, but its stb impl TUs are Win/APPLE-gated
  — BOTH of them.** A per-platform demo's `linux/` exe must supply whichever it
  uses: `stb_image_impl_linux.cpp` (models/textures) and/or
  `stb_image_write_impl.cpp` (e.g. `TileRenderer::dumpColorTarget` → `stbi_write_png`,
  earthview). One-line TU each (`#define STB_..._IMPLEMENTATION` + the header;
  resolves via common's propagated include dir). tinygltf's `NO_STB_IMAGE` avoids
  double-definition.
- **cesium / vcpkg-source-build demos** (earthview) need more than the base apt
  list: `libcurl4-openssl-dev libssl-dev zlib1g-dev` **plus the ezvcpkg build
  toolchain** `autoconf automake autoconf-archive libtool nasm zip unzip tar`
  (curl/openssl/draco/ktx build from source and hard-fail without them). Reuse the
  mac/win `EZVCPKG_BASEDIR` + `actions/cache` (`/home/runner/.ezvcpkg`).
- **Window-space-layer mirror is often N/A.** A per-platform demo whose Linux leg
  is modeled on the *macOS* harness usually doesn't reference
  `XrCompositionLayerWindowSpaceDXR` (it's a Windows-only HUD in some demos) — and
  demos linking `displayxr::common`'s `xr_window_space_hud.h` get the struct from
  `openxr_includes/` for free. Only add the interim `#ifndef` mirror if the compile
  actually needs it.
- **Merge mechanics:** demo `main` is protected (1 review; CI-green alone doesn't
  satisfy it). Open a PR so the Win/macOS/Android/lint checks run (they only fire
  on `pull_request`, not `linux*` branches), wait for green, then
  `gh pr merge <n> --admin --squash --delete-branch`. Build Linux is non-required
  and does not re-run on `main` (validated on the PR).
- **`.bat` CRLF trap:** `git add -A` can sweep a CRLF→LF renormalization of
  `scripts/*.bat` that `.gitattributes` forces to CRLF — stage intended files
  explicitly, or revert the incidental `.bat` churn before committing.

Runtime-tree apps (e.g. `test_apps/*`) additionally hit — usually N/A for demos:

- **stb / single-header impls:** `displayxr::common` ships some impl TUs
  macOS-gated (e.g. `stb_image_impl_macos.cpp`). If the shared `main.cpp` is
  declarations-only, add a Linux impl TU (`#define STB_IMAGE_IMPLEMENTATION`).
- **`XRT_OS_LINUX` ≠ desktop Linux:** Android also defines it. Runtime code gates on
  `defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)` (the `XRT_OS_LINUX_DESKTOP`
  macro). Demo repos usually use plain `__linux__`, which is fine there.
- **POSIX arms usually already exist:** `/proc/self/exe`, `usleep`, etc. are often
  already behind the macOS/Windows `#else`. Check before adding.
- **Loader-image conflicts:** pin `XR_RUNTIME_JSON` and share one `libvulkan`
  image; don't let a system-installed runtime shadow the dev build.

## Definition of done

- [ ] `build-green`: `scripts/build_linux.sh` compiles the demo on `ubuntu-latest` (CI).
- [ ] `on-screen`: runs against the dev Linux runtime, presents + weaves (needs Phase 1b + hardware).
- [ ] Loader pin == header rev == CI pin.
- [ ] Per-repo `CLAUDE.md` notes the Linux dev-build path.
