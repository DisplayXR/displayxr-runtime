# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a fork of **Monado**, an open source OpenXR runtime for VR/AR devices. This fork integrates **Leia SR SDK** for eye tracked 3D Display support. The project implements the OpenXR API standard from Khronos and runs on Linux, Android, and Windows.

## Build Commands

### Important: Do Not Build Locally on macOS
This project targets **Windows** with LeiaSR SDK and D3D11 support. Local builds on macOS will fail with platform-specific errors that are not relevant to the actual codebase.

**Instead of building locally**, use the `/ci-monitor` skill to:
- Commit and push changes
- Monitor the GitHub Actions Windows build
- Get actual build errors and auto-fix them

Example:
```bash
/ci-monitor "your commit message"
```

See the `/ci-monitor` skill section below for full details.

### Standard CMake Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
```

### With LeiaSR SDK Support
LeiaSR SDK is found via `find_package(simulatedreality CONFIG)` and `find_package(srDirectX CONFIG)`. Set `LEIASR_SDKROOT` environment variable to the SDK install path. The build will warn if LeiaSR SDK is not found.

### Running Tests
```bash
cd build
ctest
# Or run individual test:
./tests/tests_json --success
```

### Code Formatting
```bash
# Format all source files
scripts/format-project.sh

# Prefer git clang-format to only format your changes
git clang-format
```

The project uses clang-format (version 11+ preferred). A `.clang-format` file is in `src/xrt/`.

## Architecture

### Source Tree Structure (`src/xrt/`)
- **include/xrt/** - Core interface headers defining the internal APIs (`xrt_device.h`, `xrt_compositor.h`, `xrt_instance.h`, etc.)
- **auxiliary/** - Shared utilities: math (`m_*`), utilities (`u_*`), OS abstraction (`os_*`), Vulkan helpers (`vk_*`), tracking (`t_*`)
- **compositor/** - Display compositor handling distortion, layer rendering, and display output
  - `main/` - Vulkan compositor with LeiaSR weaver integration (`comp_renderer.c`)
  - `d3d11/` - Native D3D11 compositor (Windows) bypassing Vulkan for Intel GPU compatibility
- **drivers/** - Hardware drivers for various HMDs and controllers (Vive, WMR, PSVR, Rift S, etc.)
  - `leiasr/` - LeiaSR SDK driver providing Vulkan and D3D11 weavers for light field interlacing
- **state_trackers/oxr/** - OpenXR API implementation
- **ipc/** - Inter-process communication for service mode
- **targets/** - Build targets producing final binaries (monado-cli, monado-service, OpenXR runtime library)

### Key Interfaces
The codebase uses C interfaces with vtable-style polymorphism. Key structures:
- `struct xrt_device` - Abstract device interface (HMDs, controllers)
- `struct xrt_compositor` - Graphics compositor interface
- `struct xrt_instance` - Runtime instance
- `struct xrt_prober` - Device discovery

### LeiaSR SDK Integration
This fork adds LeiaSR SDK integration for eye-tracked light field displays:
- Controlled by `XRT_HAVE_LEIA_SR` CMake option (auto-enabled if SDK found)
- Vulkan weaver: `src/xrt/compositor/main/comp_renderer.c` using `leiasr/leiasr.cpp`
- D3D11 weaver: `src/xrt/compositor/d3d11/` using `leiasr/leiasr_d3d11.cpp`
- Eye tracking via SR SDK's LookaroundFilter for dynamic perspective rendering
- Display dimensions from SR::Display for Kooima asymmetric frustum projection

### D3D11 Native Compositor
A native D3D11 compositor (`src/xrt/compositor/d3d11/`) added to solve Intel GPU interop issues where D3D11→Vulkan texture import fails. Features:
- Direct D3D11 rendering pipeline bypassing Vulkan entirely
- Integrates with LeiaSR D3D11 weaver for light field interlacing
- Supports app-provided windows via XR_EXT_win32_window_binding extension
- Controlled by `OXR_ENABLE_D3D11_NATIVE_COMPOSITOR` env var (default: enabled)

### XR_EXT_win32_window_binding Extension
Custom OpenXR extension allowing apps to pass window handles (HWND) to the runtime:
- Apps chain `XrWin32WindowBindingCreateInfoEXT` to `XrSessionCreateInfo`
- Runtime renders into the app's window instead of creating its own
- Enables windowed mode, multi-app scenarios, and app-controlled input
- Header: `src/external/openxr_includes/openxr/XR_EXT_win32_window_binding.h`
- Integration: `src/xrt/state_trackers/oxr/oxr_session.c`

## Development Notes

### Languages and Standards
- C11 for core code
- C++17 where needed
- Python 3.6+ for build scripts

### Running Without Installing
```bash
XR_RUNTIME_JSON=./build/openxr_monado-dev.json ./your_openxr_app
```

### Changelog Fragments
For substantial changes, create a fragment in `doc/changes/<section>/mr.NUMBER.md` describing your change for the release notes (uses Proclamation tool).

### Key CMake Options
- `XRT_HAVE_LEIA_SR` - Enable LeiaSR SDK support (auto-enabled if SDK found)
- `XRT_HAVE_LEIA_SR_VULKAN` - Vulkan weaver available
- `XRT_HAVE_LEIA_SR_D3D11` - D3D11 weaver available (Windows only)
- `XRT_FEATURE_SERVICE` - Enable out-of-process service mode
- `XRT_BUILD_DRIVER_*` - Enable specific hardware drivers
- `BUILD_TESTING` - Enable test suite

### CMake Variable Notes
- `LEIASR_SDKROOT` - **Required environment variable** pointing to LeiaSR SDK install path
- `SR_PATH` - Internal variable in `src/xrt/drivers/CMakeLists.txt`, automatically set from `LEIASR_SDKROOT`
- The drivers CMakeLists.txt uses `SR_PATH` to locate SDK libraries (SimulatedRealityCore.lib, SimulatedRealityDirectX.lib, etc.)
- If `LEIASR_SDKROOT` is not set, a CMake warning is shown and SR weaver support is disabled

### GitHub Actions Build
The CI build in `.github/workflows/build-windows.yml` requires:
- `LEIASR_SDKROOT` env var set in the Generate step pointing to the downloaded SR SDK
- `CMAKE_PREFIX_PATH` pointing to the SR SDK for find_package to work
- Both are needed: PREFIX_PATH for find_package detection, LEIASR_SDKROOT for library paths

## Claude Code Skills

### /ci-monitor - Automated Build Workflow
Automates the complete CI workflow: commit → push → monitor → auto-fix.

**Usage:**
```bash
/ci-monitor "commit message"    # Commit with message and monitor build
/ci-monitor                      # Auto-generate commit message from changes
/ci-monitor --watch-only         # Just monitor current build without committing
```

**Features:**
- Launches subagent to preserve main conversation context
- Monitors GitHub Actions `build-windows.yml` workflow
- Auto-diagnoses common build errors (missing includes, undeclared identifiers, linker errors)
- Attempts up to 3 automatic fixes before reporting failure
- Reports success with artifact URL or failure with diagnostics

**Skill location:** `.claude/skills/ci-monitor/SKILL.md`

### /ask-gemini - Code Analysis with Gemini
Ask Gemini to analyze code and produce a read-only report. Automatically gathers relevant context based on your request.

**Usage:**
```bash
/ask-gemini <your request>
```

**Examples:**
```bash
/ask-gemini review the latest commit and flag potential issues
/ask-gemini explain the architecture of comp_renderer.c
/ask-gemini analyze the error handling patterns in oxr_session.c
```

**Features:**
- Launches subagent to preserve main conversation context
- Smart context detection based on keywords (commit, diff, file paths, etc.)
- Automatically gathers relevant git history, diffs, or file contents
- Constructs detailed prompts with READ-ONLY instructions
- Displays Gemini's analysis report

**Skill location:** `~/.claude/skills/ask-gemini/SKILL.md` (user-level, available across all projects)

## Debug Logs
- Use U_LOG_W (WARN) only for one-off init, error, and lifecycle events
- Use U_LOG_I (INFO) for recurring/throttled diagnostic logs (per-frame, per-keystroke, etc.)
- Never add per-frame U_LOG_W calls - they cause massive log bloat (~10K WARN lines per 6-min session)