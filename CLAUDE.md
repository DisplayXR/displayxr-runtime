# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Status

DisplayXR is a lightweight standalone OpenXR runtime purpose-built for 3D displays. The foundation work (issue #23) is complete — stripped from 500+ files to ~150, with native compositors for every major graphics API.

### Milestone Progress

See the [milestone tracker](https://github.com/dfattal/openxr-3d-display/milestones) for full status.

- **M1: Foundation** — Done. Stripped 34 VR drivers, removed Vulkan server compositor, cleaned CMake, extracted stereo math.
- **M2: Native Compositors** — Done. D3D11, D3D12, Metal, OpenGL, Vulkan all shipping.
- **M3: Test Coverage** — #30, #31, #33 open.
- **M4: Display Extensions** — Next major focus. Lock down extension API surface (#3, #5, #8, #38).
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

### Extension Apps vs Legacy Apps

Orthogonal to the four app classes above, apps are either **extension apps** or **legacy apps** based on whether they enable `XR_EXT_display_info`:

| Type | Detection | Rendering modes | Swapchain sizing | Mode switching |
|------|-----------|----------------|-----------------|----------------|
| **Extension app** | Enables `XR_EXT_display_info` | Enumerates all modes, handles `XrEventDataRenderingModeChangedEXT` | `max(tileColumns[i] * scaleX[i] * displayW)` across all modes | All modes: V toggle + 1/2/3 direct selection |
| **Legacy app** | Does not enable `XR_EXT_display_info` | Unaware of modes, always renders stereo | `recommendedImageRectWidth * 2` (compromise scale) | Only V toggle between mode 0 (2D) and mode 1 (default 3D) |

`_ext` and `_shared` apps are always extension apps (they need the extension for window binding). `_rt` apps can be either — a DisplayXR-aware `_rt` app enables `XR_EXT_display_info`, while a generic OpenXR `_rt` app (e.g. WebXR, third-party) is legacy.

**Legacy app compromise view scale** (in `oxr_system_fill_in`):
- **Case A** — Default 3D mode has `view_count == 2` and `scaleX <= 0.5` and `scaleY <= 0.5` (typical SBS): runtime reports `recommendedViewScale = 0.5×1.0`. App renders half-width, full-height tiles. Compositor must downscale Y for 3D, stretch X for 2D.
- **Case B** — Otherwise: runtime uses the 3D mode's actual scale. Compositor stretches for 2D.

The `legacy_app_tile_scaling` flag on `xrt_system_compositor_info` signals compositors to gate 1/2/3 key mode selection and (future) perform tile scaling before the display processor.

### Key Architectural Notes
- Compositor vtable has 56 methods — use `comp_base` helper for boilerplate
- IPC/service mode (`ipc/`, `compositor/client/`, `compositor/multi/`) must be preserved for `_ipc` apps, WebXR, and multi-app spatial shell
- `compositor/null/` — headless compositor for testing

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
- **compositor/** — Native compositors
  - `d3d11/` — D3D11 compositor (Windows)
  - `d3d11_service/` — D3D11 service compositor
  - `d3d12/` — D3D12 compositor (Windows)
  - `metal/` — Metal compositor (macOS)
  - `gl/` — OpenGL compositor (Windows + macOS)
  - `vk_native/` — Vulkan compositor (Windows + macOS)
  - `multi/` — Multi-client compositor (IPC/multi-app)
  - `client/` — Client compositor bindings
  - `null/` — Null compositor (testing)
  - `util/` — Compositor utilities
- **drivers/** — Hardware drivers
  - `leia/` — LeiaSR SDK driver (Vulkan + D3D11 weavers)
  - `sim_display/` — Simulation display driver
  - `qwerty/` — Keyboard/mouse simulated controllers
- **state_trackers/oxr/** — OpenXR API implementation
- **ipc/** — Inter-process communication for service mode
- **targets/** — Build targets (runtime library, displayxr-cli, displayxr-service)

### Key Interfaces
C interfaces with vtable-style polymorphism:
- `struct xrt_device` — Abstract device interface
- `struct xrt_compositor` — Graphics compositor interface
- `struct xrt_instance` — Runtime instance
- `struct xrt_prober` — Device discovery

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

### Custom OpenXR Extensions
- `XR_EXT_win32_window_binding` — App passes HWND to runtime
- `XR_EXT_cocoa_window_binding` — App passes NSWindow to runtime
- `XR_EXT_display_info` — Display dimensions, eye tracking modes
- `XR_EXT_android_surface_binding` — Android surface binding

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

## Debug Logs
- Use U_LOG_W (WARN) only for one-off init, error, and lifecycle events
- Use U_LOG_I (INFO) for recurring/throttled diagnostic logs (per-frame, per-keystroke, etc.)
- Never add per-frame U_LOG_W calls — they cause massive log bloat
