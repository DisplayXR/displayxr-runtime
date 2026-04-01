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

See `docs/getting-started/app-classes.md` for the full reference (handle, texture, hosted, IPC).

**Key code paths by class:**
- `_handle` / `_texture` / `_hosted` → `compositor/{d3d11,d3d12,metal,gl,vk_native}/` (in-process)
- `_ipc` → `compositor/client/` → `ipc/` → `compositor/multi/` → native compositor (out-of-process)

Test app naming: `cube_{class}_{api}_{platform}` — e.g. `cube_handle_metal_macos`, `cube_texture_d3d11_win`, `cube_ipc_d3d11_win`.

### Extension Apps vs Legacy Apps

See `docs/architecture/extension-vs-legacy.md` for the full reference.

Key facts for AI context: `_handle` and `_texture` are always extension apps. `_hosted` can be either. Legacy app compromise scaling is computed in `oxr_system_fill_in()`. The `legacy_app_tile_scaling` flag on `xrt_system_compositor_info` disables 1/2/3 key mode selection for legacy apps (V toggle only).

### Key Architectural Notes
- Compositor vtable has 56 methods — use `comp_base` helper for boilerplate
- IPC/service mode (`ipc/`, `compositor/client/`, `compositor/multi/`) must be preserved for `_ipc` apps, WebXR, and multi-app spatial shell
- `compositor/null/` — headless compositor for testing
- **Two distinct swapchains** — see `docs/specs/swapchain-model.md`
- **Canvas concept** — view dims and Kooima projection use canvas size, not display size. See `docs/specs/swapchain-model.md`.
- **Compositor pipeline** — see `docs/architecture/compositor-pipeline.md`

For the vendor isolation rule and layer "must NOT contain" constraints, see `docs/architecture/separation-of-concerns.md`.
For display processor vtable design (all 5 API variants), see `docs/guides/vendor-integration.md`.

## Project Overview

This is a fork of **Monado**, an open source OpenXR runtime for VR/AR devices. This fork integrates **Leia SR SDK** for eye tracked 3D Display support. The project implements the OpenXR API standard from Khronos and runs on Windows, macOS, and Android.

## Build Commands

### Local macOS Build
```bash
# Prerequisites: brew install cmake ninja eigen vulkan-sdk
./scripts/build_macos.sh
```
Builds the runtime, OpenXR loader, and test apps. The Vulkan compositor will fail at runtime with `VK_ERROR_EXTENSION_NOT_PRESENT` (MoltenVK limitation, not a build issue).

### Local Windows Build
```bat
scripts\build_windows.bat all        REM Full build (generate + runtime + installer + test apps)
scripts\build_windows.bat build      REM Runtime only (fastest iteration)
scripts\build_windows.bat test-apps  REM Test apps only (uses existing runtime build)
scripts\build_windows.bat generate   REM CMake generate only
```
Downloads all dependencies on first run (SR SDK, vcpkg, OpenXR loader). Requires VS 2022 with C++ workload, Ninja, Vulkan SDK, and GitHub CLI. Outputs to `_package/` (runtime) and `test_apps/*/build/` (test apps).

**When on a Windows machine with a Leia SR display, prefer local builds over CI** — iterate faster with `scripts\build_windows.bat build` and test directly. Run scripts are generated in `_package/` (see Windows Test App section below).

### CI Build (Remote)
```bash
/ci-monitor "your commit message"
```
Commits, pushes, monitors GitHub Actions (Windows + macOS), auto-fixes common build errors. Use when not on a local dev machine or for final validation before merge.

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

### Publishing to DisplayXR (Public Release)

The public repo `DisplayXR/displayxr-runtime` is the `displayxr` remote. Push CI-validated commits from `origin` (dfattal) when ready for a public release:

```bash
# Push current main to public repo (full commit history preserved)
git push displayxr main

# For a tagged release:
git tag v1.x.x
git push origin v1.x.x          # triggers Leia CI for the tag
git push displayxr v1.x.x       # publishes tag to public repo
git push displayxr main          # ensure main is up to date
```

CI only runs on dfattal (guarded by `github.repository` check in workflows). The public repo shows "skipped" for CI jobs — this is expected. Never push untested commits directly to displayxr. Always validate via `/ci-monitor` first.

## Architecture

### Source Tree Structure (`src/xrt/`)
- **include/xrt/** — Core interface headers (`xrt_device.h`, `xrt_compositor.h`, `xrt_instance.h`, etc.)
- **auxiliary/** — Shared utilities: math (`m_*`), utilities (`u_*`), OS abstraction (`os_*`), Vulkan helpers (`vk_*`)
- **compositor/** — Native compositors (D3D11, D3D12, Metal, GL, Vulkan, multi, client, null). See `docs/architecture/project-structure.md`.
- **drivers/** — `leia/` (LeiaSR SDK), `sim_display/` (simulation), `qwerty/` (keyboard/mouse controllers)
- **state_trackers/oxr/** — OpenXR API implementation
- **ipc/** — Inter-process communication for service mode
- **targets/** — Build targets (runtime library, displayxr-cli, displayxr-service, displayxr-shell)

### Key Interfaces
C interfaces with vtable-style polymorphism:
- `struct xrt_device` — Abstract device interface
- `struct xrt_compositor` — Graphics compositor interface
- `struct xrt_instance` — Runtime instance
- `struct xrt_prober` — Device discovery

For the full interface catalog including display processor vtables (5 API variants), see `docs/guides/vendor-integration.md`.

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
**Important:** Always include the related GitHub issue number in commit messages — e.g., `Fix linker error (#93)`. Check conversation context and recent commits to determine the issue number.

### /ask-gemini - Code Analysis with Gemini
Ask Gemini to analyze code and produce a read-only report. See `~/.claude/skills/ask-gemini/SKILL.md`.

## macOS Test App Local Builds

Copy binaries to `_package/DisplayXR-macOS/bin/`. Run scripts exec from `$DIR/bin/`.

| Test App | Build Output | Package Binary | Run Script |
|----------|-------------|---------------|------------|
| cube_handle_vk_macos | `test_apps/cube_handle_vk_macos/build/cube_handle_vk_macos` | `_package/.../bin/cube_handle_vk_macos` | `run_cube_handle_vk.sh` |
| cube_handle_metal_macos | `test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos` | `_package/.../bin/cube_handle_metal_macos` | `run_cube_handle_metal.sh` |
| cube_handle_gl_macos | `test_apps/cube_handle_gl_macos/build/cube_handle_gl_macos` | `_package/.../bin/cube_handle_gl_macos` | `run_cube_handle_gl.sh` |
| cube_texture_metal_macos | `test_apps/cube_texture_metal_macos/build/cube_texture_metal_macos` | `_package/.../bin/cube_texture_metal_macos` | `run_cube_texture_metal.sh` |
| cube_hosted_metal_macos | `test_apps/cube_hosted_metal_macos/build/cube_hosted_metal_macos` | `_package/.../bin/cube_hosted_metal_macos` | `run_cube_hosted_metal.sh` |
| cube_hosted_legacy_metal_macos | `test_apps/cube_hosted_legacy_metal_macos/build/cube_hosted_legacy_metal_macos` | `_package/.../bin/cube_hosted_legacy_metal_macos` | `run_cube_hosted_legacy_metal.sh` |
| gaussian_splatting_handle_vk_macos | `demos/gaussian_splatting_handle_vk_macos/build/gaussian_splatting_handle_vk_macos` | `_package/.../bin/gaussian_splatting_handle_vk_macos` | `run_gaussian_splatting_handle_vk.sh` |

## Windows Test App Local Builds

`scripts\build_windows.bat test-apps` builds test apps and generates run scripts in `_package/`. Each run script sets `XR_RUNTIME_JSON` to the dev build so the installed runtime (from CI) is not used.

**Standalone apps:**

| Test App | Run Script |
|----------|------------|
| cube_handle_d3d11_win | `_package\run_cube_handle_d3d11_win.bat` |
| cube_hosted_d3d11_win | `_package\run_cube_hosted_d3d11_win.bat` |
| cube_handle_d3d12_win | `_package\run_cube_handle_d3d12_win.bat` |
| cube_handle_gl_win | `_package\run_cube_handle_gl_win.bat` |
| cube_handle_vk_win | `_package\run_cube_handle_vk_win.bat` |

**Shell mode — single command (recommended):**

`displayxr-shell.exe` auto-starts the service, activates shell mode via IPC, launches apps with correct env vars, and monitors clients.

```
_package\bin\displayxr-shell.exe app1.exe app2.exe
```

Example with two cube apps:
```
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

Optional per-app window pose (`--pose x,y,z,width_m,height_m` before each app path):
```
_package\bin\displayxr-shell.exe --pose -0.1,0.05,0,0.14,0.08 app1.exe --pose 0.1,0.05,0,0.14,0.08 app2.exe
```

**Shell mode — legacy multi-terminal (still works):**

| Terminal | Script | Description |
|----------|--------|-------------|
| 1 | `_package\run_shell_service.bat` | Starts `displayxr-service --shell` |
| 2 | `_package\run_shell_app.bat` | First app → slot 0 (left-upper, 40% of display) |
| 3 | `_package\run_shell_app.bat` | Second app → slot 1 (right-upper, 40% of display) |

**Shell controls:** Left-click=focus window, Right-click-drag=move window, Scroll=resize window, TAB=cycle focus, DELETE=close app, ESC=dismiss shell, V=toggle 2D/3D, WASD/left-click-drag=app input.

**When launching from Claude Code:** Use `displayxr-shell.exe` — it handles service auto-start, `XR_RUNTIME_JSON`, and `DISPLAYXR_SHELL_SESSION=1` automatically. Use `run_in_background: true` on the Bash tool call and `timeout: 600000`. See `docs/roadmap/shell-phase1-status.md` for the full test procedure.

## Documentation

See `docs/README.md` for a complete index. Key docs by task:

| When you need to... | Read |
|---|---|
| Understand layer boundaries (what goes where) | `docs/architecture/separation-of-concerns.md` |
| Add a new display vendor | `docs/guides/vendor-integration.md` |
| Understand multiview tiling / atlas layout | `docs/specs/multiview-tiling.md` |
| Understand extension API (display_info, window bindings) | `docs/specs/XR_EXT_display_info.md` |
| Know why an architectural decision was made | `docs/adr/` (10 ADRs) |
| Understand legacy vs extension app differences | `docs/architecture/extension-vs-legacy.md` |
| Understand eye tracking MANAGED/MANUAL contract | `docs/specs/eye-tracking-modes.md` |
| Add a new OpenXR extension | `docs/guides/implementing-extension.md` |
| Write a device driver | `docs/guides/writing-driver.md` |
| Understand Leia SR DX weaver internals | `docs/reference/leia-sr-dx-weaving.md` |
| Understand Kooima projection math | `docs/architecture/kooima-projection.md` |
| Understand the compositor pipeline | `docs/architecture/compositor-pipeline.md` |
| Understand the swapchain model / canvas | `docs/specs/swapchain-model.md` |
| Track shell implementation progress | `docs/roadmap/shell-tasks.md` |
| Understand the 3D capture pipeline | `docs/roadmap/3d-capture.md` |
| Understand shell/runtime IPC contract | `docs/roadmap/shell-runtime-contract.md` |
| Understand the overall product vision | `docs/roadmap/spatial-desktop-prd.md` |

## Debug Logs

See `docs/reference/debug-logging.md` for full conventions.
- Use U_LOG_W (WARN) only for one-off init, error, and lifecycle events
- Use U_LOG_I (INFO) for recurring/throttled diagnostic logs (per-frame, per-keystroke, etc.)
- Never add per-frame U_LOG_W calls — they cause massive log bloat
