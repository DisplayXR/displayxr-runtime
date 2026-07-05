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
   `XR_EXT_win32_window_binding` / `XR_EXT_cocoa_window_binding`. On Linux there
   is no equivalent yet, so a **faithful port depends on runtime Phase 3**
   (`XR_EXT_xlib_window_binding` + the compositor's app-provided-window branch).
   Until Phase 3 lands, a demo can only run on Linux via a **hosted-NULL fallback**
   (pass no window; the runtime self-creates one) — acceptable as an interim
   "does it render" check, but it diverges from the win/mac windowed behavior and
   won't exercise window positioning / the workspace model.

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
build-essential cmake ninja-build pkg-config \
libvulkan-dev vulkan-validationlayers glslang-tools \
libxcb1-dev libxcb-randr0-dev libx11-xcb-dev   # only if the app itself opens an X window
```

Most demos are hosted-style (the runtime owns the window), so they don't link
xcb themselves — only the runtime does.

## Gotchas already hit (bake into every port)

- **stb / single-header impls:** `displayxr::common` ships some impl TUs
  macOS-gated (e.g. `stb_image_impl_macos.cpp`). If the shared `main.cpp` is
  declarations-only, add a small Linux impl TU (`#define STB_IMAGE_IMPLEMENTATION`)
  — else undefined `stbi_*` at link. (runtime `cube_hosted_legacy_vk_linux` did this.)
- **`XRT_OS_LINUX` ≠ desktop Linux:** Android also defines it. Any "desktop
  Linux, not Android" code must gate on `defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)`.
- **POSIX arms usually already exist:** executable-path (`/proc/self/exe`),
  `usleep`, etc. are often already behind the macOS/Windows `#else`. Check before
  adding.
- **Loader-image conflicts:** pin `XR_RUNTIME_JSON` and share one `libvulkan`
  image; don't let a system-installed runtime shadow the dev build.

## Definition of done

- [ ] `build-green`: `scripts/build_linux.sh` compiles the demo on `ubuntu-latest` (CI).
- [ ] `on-screen`: runs against the dev Linux runtime, presents + weaves (needs Phase 1b + hardware).
- [ ] Loader pin == header rev == CI pin.
- [ ] Per-repo `CLAUDE.md` notes the Linux dev-build path.
