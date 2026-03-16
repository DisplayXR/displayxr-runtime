# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Status

DisplayXR is a lightweight standalone OpenXR runtime purpose-built for 3D displays. The foundation work (issue #23) is complete — stripped from 500+ files to ~150, with native compositors for every major graphics API.

### Milestone Progress

See the [milestone tracker](https://github.com/dfattal/openxr-3d-display/milestones) for full status.

- **M1: Foundation** — Done. Stripped 34 VR drivers, removed Vulkan server compositor, cleaned CMake, extracted stereo math.
- **M2: Native Compositors** — Done. D3D11, D3D12, Metal, OpenGL, Vulkan all shipping.
- **M3: Test Coverage** — #30, #31, #33 open.
- **M4: Display Extensions** — Next major focus. Lock down extension API surface (#3, #8, #38). #5 closed (superseded by #77).
- **M5: Interface Standardization** — #45, #46, #47 open.
- **M6: Spatial Shell** — #43, #44 open.

### Architecture

```
App (any graphics API)
        |
   OpenXR State Tracker
        |
   Core xrt interfaces
        |
   +----+-----+--------+--------+
   |    |     |        |        |
 D3D11 D3D12 Vulkan  Metal   OpenGL   <-- native compositors
   |    |     |        |        |
   Display Processor (LeiaSR / sim_display)
        |
   Display
```

Each graphics API gets a native compositor — no interop, no Vulkan intermediary.

For layer boundaries and what each layer owns, see `docs/architecture/separation-of-concerns.md`.
Why each API gets its own compositor: `docs/adr/ADR-001-native-compositors-per-graphics-api.md`.

### Four App Classes

| Class | Suffix | Description | Compositor path |
|-------|--------|-------------|----------------|
| **Window-handle** | `_ext` | App provides its own window via `XR_EXT_*_window_binding` | Native compositor directly in-process |
| **Shared-texture** | `_shared` | App provides textures, runtime composites into its own window | Native compositor directly in-process |
| **Runtime-managed** | `_rt` | Runtime creates window and rendering targets (standard OpenXR/WebXR) | Native compositor directly in-process |
| **IPC/Service** | `_ipc` | App runs out-of-process via client compositor → IPC → server multi-compositor | Client compositor → IPC transport → multi-compositor → native compositor in server |

Test app naming: `cube_ext_metal_macos`, `cube_shared_gl_macos`, `cube_rt_vk_macos` (runtime-managed), `cube_ipc_d3d11` (service mode).

The first three classes all use a native compositor in-process. The `_ipc` class is fundamentally different: the app links a **client compositor** that serializes compositor calls over IPC to a **server process** running the multi-compositor (`compositor/multi/`), which fans out to native compositors. This is the multi-app path and the foundation for the spatial shell (#43, #44).

**Key code paths by class:**
- `_ext` / `_shared` / `_rt` → `compositor/{d3d11,d3d12,metal,gl,vk_native}/` (in-process)
- `_ipc` → `compositor/client/` → `ipc/` → `compositor/multi/` → native compositor (out-of-process)

For in-process vs service details, see `docs/architecture/in-process-vs-service.md`.
For the spatial shell multi-app vision, see `docs/architecture/spatial-os.md` and `docs/architecture/3d-shell.md`.

### Extension Apps vs Legacy Apps

Orthogonal to the four app classes above, apps are either **extension apps** or **legacy apps** based on whether they enable `XR_EXT_display_info`:

| Type | Detection | Rendering modes | Swapchain sizing | Mode switching |
|------|-----------|----------------|-----------------|----------------|
| **Extension app** | Enables `XR_EXT_display_info` | Enumerates all modes, handles `XrEventDataRenderingModeChangedEXT` | `max(tileColumns[i] * scaleX[i] * displayW)` across all modes | All modes: V toggle + 1/2/3 direct selection |
| **Legacy app** | Does not enable `XR_EXT_display_info` | Unaware of modes, always renders stereo | `recommendedImageRectWidth * 2` (compromise scale) | Only V toggle between mode 0 (2D) and mode 1 (default 3D) |

`_ext` and `_shared` apps are always extension apps (they need the extension for window binding). `_rt` apps can be either — a DisplayXR-aware `_rt` app enables `XR_EXT_display_info`, while a generic OpenXR `_rt` app (e.g. WebXR, third-party) is legacy.

For the full multiview tiling algorithm and atlas layout, see `docs/specs/multiview-tiling.md`.
For legacy app compromise scaling rationale (Case A/B), see `docs/specs/legacy-app-support.md`.

Legacy app compromise scaling is computed in `oxr_system_fill_in()` — see `docs/specs/legacy-app-support.md` for the full algorithm. The `legacy_app_tile_scaling` flag on `xrt_system_compositor_info` disables 1/2/3 key mode selection for legacy apps (V toggle only).

### Key Architectural Notes
- Compositor vtable has 56 methods — use `comp_base` helper for boilerplate
- IPC/service mode (`ipc/`, `compositor/client/`, `compositor/multi/`) must be preserved for `_ipc` apps, WebXR, and multi-app spatial shell
- `compositor/null/` — headless compositor for testing
- **Two distinct swapchains per compositor:**
  - **App swapchain** — runtime-allocated (`xrCreateSwapchain`), worst-case sized at init. App renders atlas of tiled views into this.
  - **Target swapchain** — compositor's output to the display, window-sized. DP writes interlaced output here.
  - Pipeline: app swapchain → compositor crops atlas to content dims → DP interlaces → target swapchain → present.
  - These are unrelated — the app swapchain flows in, the target swapchain flows out.
  - See `docs/specs/multiview-tiling.md` "Compositor-Side Contract" section.

- **Canvas concept:** View dimensions and Kooima projection must be based on **canvas** size (the sub-rect of the window where 3D content appears), not display size. Critical for `_shared` apps where the canvas may be smaller than the display. See `docs/specs/multiview-tiling.md` "Terminology: Display, Window, Canvas".

For the vendor isolation rule and layer "must NOT contain" constraints, see `docs/architecture/separation-of-concerns.md`.
For display processor vtable design (all 5 API variants), see `docs/specs/vendor-integration.md`.

## Project Overview

This is a fork of **Monado**, an open source OpenXR runtime for VR/AR devices. This fork integrates **Leia SR SDK** for eye tracked 3D Display support. The project implements the OpenXR API standard from Khronos and runs on Windows, macOS, and Android.

## Build Commands

### Local macOS Build
```bash
# Prerequisites: brew install cmake ninja eigen vulkan-sdk
./scripts/build_macos.sh
```
Builds the runtime, OpenXR loader, and test apps. The Vulkan compositor will fail at runtime with `VK_ERROR_EXTENSION_NOT_PRESENT` (MoltenVK limitation, not a build issue).

### Windows CI Build (Primary)
```bash
/ci-monitor "your commit message"
```
Commits, pushes, monitors GitHub Actions (Windows + macOS), auto-fixes common build errors.

### Standard CMake Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
```

### With LeiaSR SDK Support
Set `LEIASR_SDKROOT` environment variable. Found via `find_package(simulatedreality CONFIG)` and `find_package(srDirectX CONFIG)`.

### Running Tests
```bash
cd build && ctest
```

### Code Formatting
```bash
git clang-format    # Format only your changes (preferred)
scripts/format-project.sh   # Format all
```

## Architecture

### Source Tree Structure (`src/xrt/`)
- **include/xrt/** — Core interface headers (`xrt_device.h`, `xrt_compositor.h`, `xrt_instance.h`, etc.)
- **auxiliary/** — Shared utilities: math (`m_*`), utilities (`u_*`), OS abstraction (`os_*`), Vulkan helpers (`vk_*`)
- **compositor/** — Native compositors (D3D11, D3D12, Metal, GL, Vulkan, multi, client, null). See `docs/architecture/project-structure.md`.
- **drivers/** — `leia/` (LeiaSR SDK), `sim_display/` (simulation), `qwerty/` (keyboard/mouse controllers)
- **state_trackers/oxr/** — OpenXR API implementation
- **ipc/** — Inter-process communication for service mode
- **targets/** — Build targets (runtime library, displayxr-cli, displayxr-service)

### Key Interfaces
C interfaces with vtable-style polymorphism:
- `struct xrt_device` — Abstract device interface
- `struct xrt_compositor` — Graphics compositor interface
- `struct xrt_instance` — Runtime instance
- `struct xrt_prober` — Device discovery

For the full interface catalog including display processor vtables (5 API variants), see `docs/specs/vendor-integration.md`.

### LeiaSR SDK Integration
- `XRT_HAVE_LEIA_SR` CMake option (auto-enabled if SDK found)
- D3D11 weaver: `compositor/d3d11/` via `drivers/leia/leiasr_d3d11.cpp`
- Eye tracking via SR SDK's LookaroundFilter
- Display dimensions from SR::Display for Kooima asymmetric frustum projection

### Native Compositors
Each bypasses Vulkan entirely for its graphics API:
- **D3D11** (`compositor/d3d11/`) — Shipping. LeiaSR D3D11 weaver, `XR_EXT_win32_window_binding`
- **D3D12** (`compositor/d3d12/`) — Shipping. `XR_EXT_win32_window_binding`
- **Metal** (`compositor/metal/`) — Shipping. sim_display weaver, `XR_EXT_cocoa_window_binding`
- **OpenGL** (`compositor/gl/`) — Shipping. Windows + macOS
- **Vulkan** (`compositor/vk_native/`) — Shipping. Windows + macOS (MoltenVK)

Why native compositors instead of Vulkan interop: `docs/adr/ADR-001-native-compositors-per-graphics-api.md`.
Compositor never weaves — that's the DP's job: `docs/adr/ADR-007-compositor-never-weaves.md`.

### Custom OpenXR Extensions
- `XR_EXT_win32_window_binding` — App passes HWND to runtime
- `XR_EXT_cocoa_window_binding` — App passes NSWindow to runtime
- `XR_EXT_display_info` — Display dimensions, eye tracking modes
- `XR_EXT_android_surface_binding` — Android surface binding

Full extension specs: `docs/specs/XR_EXT_display_info.md`, `docs/specs/XR_EXT_win32_window_binding.md`, `docs/specs/XR_EXT_cocoa_window_binding.md`.
Eye tracking MANAGED vs MANUAL contract: `docs/specs/eye-tracking-modes.md`.

## Development Notes

### Languages and Standards
- C11 for core code, C++17 where needed, Python 3.6+ for build scripts

### Running Without Installing
```bash
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./your_openxr_app
```

### Key CMake Options
- `XRT_HAVE_LEIA_SR` — LeiaSR SDK support
- `XRT_HAVE_LEIA_SR_VULKAN` / `XRT_HAVE_LEIA_SR_D3D11` — API-specific weavers
- `XRT_FEATURE_SERVICE` — Out-of-process service mode
- `BUILD_TESTING` — Test suite

### CMake Variable Notes
- `LEIASR_SDKROOT` — Required env var for LeiaSR SDK path
- `SR_PATH` — Internal, auto-set from `LEIASR_SDKROOT`

### GitHub Actions Build
**Windows** (`.github/workflows/build-windows.yml`):
- `LEIASR_SDKROOT` + `CMAKE_PREFIX_PATH` both needed
- Artifact: `DisplayXR`

**macOS** (`.github/workflows/build-macos.yml`):
- Vulkan SDK via MoltenVK, bundles libvulkan + OpenXR loader
- Artifact: `DisplayXR-macOS`

## Claude Code Skills

### /ci-monitor - Automated Build Workflow
Automates commit, push, GitHub Actions monitoring, auto-fix. See `.claude/skills/ci-monitor/SKILL.md`.

### /ask-gemini - Code Analysis with Gemini
Ask Gemini to analyze code and produce a read-only report. See `~/.claude/skills/ask-gemini/SKILL.md`.

## macOS Test App Local Builds

Copy binaries to `_package/DisplayXR-macOS/bin/`. Run scripts exec from `$DIR/bin/`.

| Test App | Build Output | Package Binary | Run Script |
|----------|-------------|---------------|------------|
| cube_rt_vk_macos | `test_apps/cube_rt_vk_macos/build/cube_rt_vk_macos` | `_package/.../bin/cube_rt_vk_macos` | `run_cube_rt_vk.sh` |
| cube_ext_vk_macos | `test_apps/cube_ext_vk_macos/build/cube_ext_vk_macos` | `_package/.../bin/cube_ext_vk_macos` | `run_cube_ext_vk.sh` |
| cube_rt_gl_macos | `test_apps/cube_rt_gl_macos/build/cube_rt_gl_macos` | `_package/.../bin/cube_rt_gl_macos` | `run_cube_rt_gl.sh` |
| cube_ext_metal_macos | `test_apps/cube_ext_metal_macos/build/cube_ext_metal_macos` | `_package/.../bin/cube_ext_metal_macos` | `run_cube_ext_metal.sh` |
| cube_rt_metal_macos | `test_apps/cube_rt_metal_macos/build/cube_rt_metal_macos` | `_package/.../bin/cube_rt_metal_macos` | `run_cube_rt_metal.sh` |
| cube_shared_metal_macos | `test_apps/cube_shared_metal_macos/build/cube_shared_metal_macos` | `_package/.../bin/cube_shared_metal_macos` | `run_cube_shared_metal.sh` |
| cube_shared_gl_macos | `test_apps/cube_shared_gl_macos/build/cube_shared_gl_macos` | `_package/.../bin/cube_shared_gl_macos` | `run_cube_shared_gl.sh` |
| cube_shared_vk_macos | `test_apps/cube_shared_vk_macos/build/cube_shared_vk_macos` | `_package/.../bin/cube_shared_vk_macos` | `run_cube_shared_vk.sh` |
| gaussian_splatting_ext_vk_macos | `demos/gaussian_splatting_ext_vk_macos/build/gaussian_splatting_ext_vk_macos` | `_package/.../bin/gaussian_splatting_ext_vk_macos` | `run_gaussian_splatting_ext_vk.sh` |

## Documentation

See `docs/README.md` for a complete index. Key docs by task:

| When you need to... | Read |
|---|---|
| Understand layer boundaries (what goes where) | `docs/architecture/separation-of-concerns.md` |
| Add a new display vendor | `docs/specs/vendor-integration.md` |
| Understand multiview tiling / atlas layout | `docs/specs/multiview-tiling.md` |
| Understand extension API (display_info, window bindings) | `docs/specs/XR_EXT_display_info.md` |
| Know why an architectural decision was made | `docs/adr/` (9 ADRs) |
| Understand legacy vs extension app differences | `docs/specs/legacy-app-support.md` |
| Understand eye tracking MANAGED/MANUAL contract | `docs/specs/eye-tracking-modes.md` |
| Add a new OpenXR extension | `docs/notes/implementing-extension.md` |
| Write a device driver | `docs/notes/writing-driver.md` |
| Understand stereo math / Kooima projection | `docs/architecture/stereo3d-math.md` |

## Debug Logs
- Use U_LOG_W (WARN) only for one-off init, error, and lifecycle events
- Use U_LOG_I (INFO) for recurring/throttled diagnostic logs (per-frame, per-keystroke, etc.)
- Never add per-frame U_LOG_W calls — they cause massive log bloat
