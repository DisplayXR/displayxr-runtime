# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Branch Mission: Lightweight Runtime (Issue #23)

This branch implements **issue #23** — transforming the Monado fork into a lightweight standalone OpenXR runtime purpose-built for 3D displays. Target: ~150-200 files (vs 500+ today) where every file is relevant.

### Sub-issues (execution order)

**Phase 1: Strip down**
- [ ] #24 — Remove 34 unused VR drivers (keep leia, sim_display, qwerty)
- [ ] #25 — Remove Vulkan server compositor (`compositor/main/`, `render/`, `shaders/`) and tracking infrastructure
- [ ] #26 — Clean up CMake build system (remove 30+ unused toggles, dead find modules)
- [ ] #32 — Move stereo math (`display3d_view.c`, `camera3d_view.c`) into `auxiliary/math/`

**Phase 2: Add native compositors**
- [ ] #27 — Native D3D12 compositor
- [ ] #28 — Native Vulkan compositor (direct submit, not server)
- [ ] #29 — Native OpenGL compositor

**Phase 3: Complete test app coverage**
- [ ] #30 — Non-ext test apps (cube_d3d12, cube_vk, cube_gl)
- [ ] #31 — Shared texture test apps (cube_shared_d3d11, cube_shared_d3d12, cube_shared_vk, cube_shared_vk_macos, cube_shared_gl)

**Phase 4: Conformance**
- [ ] #33 — OpenXR conformance testing (Khronos CTS)

### Architecture Goal

```
App (any graphics API)
        |
   OpenXR State Tracker (from Monado)
        |
   Core xrt interfaces
        |
   +----+-----+--------+--------+
   |    |     |        |        |
 D3D11 D3D12 Vulkan  Metal   OpenGL   <-- native compositors
   |    |     |        |        |
   Weaver (LeiaSR / CNSDK / sim_display)
        |
   Display
```

Each graphics API gets a native compositor — no interop, no Vulkan intermediary.

### Two App Classes

1. **3D Display-aware apps** — use window binding + display info extensions, Kooima projection
2. **Standard OpenXR/WebXR apps** — runtime provides window, qwerty simulates controllers, eye tracking drives perspective

### Key Risks to Watch
- Compositor vtable has 56 methods — use `comp_base` helper for boilerplate
- IPC/service mode (39 files, 12K LOC) must be preserved for WebXR + multi-app
- Keep `compositor/multi/` (multi-client), `compositor/client/`, `compositor/null/`, `compositor/util/`
- `display3d_view.c` / `camera3d_view.c` are load-bearing — extract before removing dependencies

## Project Overview

This is a fork of **Monado**, an open source OpenXR runtime for VR/AR devices. This fork integrates **Leia SR SDK** for eye tracked 3D Display support. The project implements the OpenXR API standard from Khronos and runs on Linux, Android, and Windows.

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
- **compositor/** — Display compositors
  - `d3d11/` — Native D3D11 compositor (Windows)
  - `d3d11_service/` — D3D11 service compositor
  - `metal/` — Native Metal compositor (macOS)
  - `multi/` — Multi-client compositor (IPC/multi-app)
  - `client/` — Client compositor bindings
  - `null/` — Null compositor (testing)
  - `util/` — Compositor utilities
  - `main/` — Vulkan server compositor (TO BE REMOVED in #25)
  - `render/` — Vulkan render helpers (TO BE REMOVED in #25)
  - `shaders/` — Vulkan shaders (TO BE REMOVED in #25)
- **drivers/** — Hardware drivers
  - `leia/` — LeiaSR SDK driver (Vulkan + D3D11 weavers)
  - `sim_display/` — Simulation display driver
  - `qwerty/` — Keyboard/mouse simulated controllers
  - 34 unused VR drivers (TO BE REMOVED in #24)
- **state_trackers/oxr/** — OpenXR API implementation
- **ipc/** — Inter-process communication for service mode
- **targets/** — Build targets (runtime library, monado-cli, monado-service)

### Key Interfaces
C interfaces with vtable-style polymorphism:
- `struct xrt_device` — Abstract device interface
- `struct xrt_compositor` — Graphics compositor interface
- `struct xrt_instance` — Runtime instance
- `struct xrt_prober` — Device discovery

### LeiaSR SDK Integration
- `XRT_HAVE_LEIA_SR` CMake option (auto-enabled if SDK found)
- Vulkan weaver: `compositor/main/comp_renderer.c` via `leiasr/leiasr.cpp`
- D3D11 weaver: `compositor/d3d11/` via `leiasr/leiasr_d3d11.cpp`
- Eye tracking via SR SDK's LookaroundFilter
- Display dimensions from SR::Display for Kooima asymmetric frustum projection

### Native Compositors
Each bypasses Vulkan entirely for its graphics API:
- **D3D11** (`compositor/d3d11/`) — Direct D3D11 pipeline, LeiaSR D3D11 weaver, `XR_EXT_win32_window_binding`
- **Metal** (`compositor/metal/`) — Direct Metal pipeline, sim_display weaver, `XR_EXT_cocoa_window_binding`
- **D3D12** — Planned (#27)
- **Vulkan** — Planned (#28), direct submit (not the Monado server compositor)
- **OpenGL** — Planned (#29)

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
XR_RUNTIME_JSON=./build/openxr_monado-dev.json ./your_openxr_app
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
- Artifact: `SRMonado`

**macOS** (`.github/workflows/build-macos.yml`):
- Vulkan SDK via MoltenVK, bundles libvulkan + OpenXR loader
- Artifact: `SRMonado-macOS`

## Claude Code Skills

### /ci-monitor - Automated Build Workflow
Automates commit, push, GitHub Actions monitoring, auto-fix. See `.claude/skills/ci-monitor/SKILL.md`.

### /ask-gemini - Code Analysis with Gemini
Ask Gemini to analyze code and produce a read-only report. See `~/.claude/skills/ask-gemini/SKILL.md`.

## macOS Test App Local Builds

Copy binaries to `_package/SRMonado-macOS/bin/`. Run scripts exec from `$DIR/bin/`.

| Test App | Build Output | Package Binary | Run Script |
|----------|-------------|---------------|------------|
| cube_vk_macos | `test_apps/cube_vk_macos/build/cube_vk_macos` | `_package/.../bin/cube_vk_macos` | `run_cube_vk.sh` |
| cube_ext_vk_macos | `test_apps/cube_ext_vk_macos/build/cube_ext_vk_macos` | `_package/.../bin/cube_ext_vk_macos` | `run_cube_ext_vk.sh` |
| cube_ext_metal_macos | `test_apps/cube_ext_metal_macos/build/cube_ext_metal_macos` | `_package/.../bin/cube_ext_metal_macos` | `run_cube_ext_metal.sh` |
| cube_shared_metal_macos | `test_apps/cube_shared_metal_macos/build/cube_shared_metal_macos` | `_package/.../bin/cube_shared_metal_macos` | `run_cube_shared_metal.sh` |
| gaussian_splatting_vk_macos | `demos/gaussian_splatting_vk_macos/build/gaussian_splatting_vk_macos` | `_package/.../bin/gaussian_splatting_vk_macos` | `run_gaussian_splatting.sh` |

## Debug Logs
- Use U_LOG_W (WARN) only for one-off init, error, and lifecycle events
- Use U_LOG_I (INFO) for recurring/throttled diagnostic logs (per-frame, per-keystroke, etc.)
- Never add per-frame U_LOG_W calls — they cause massive log bloat
