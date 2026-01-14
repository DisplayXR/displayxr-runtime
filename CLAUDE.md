# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a fork of **Monado**, an open source OpenXR runtime for VR/AR devices. This fork integrates **CNSDK (Leia SDK)** for light field display support. The project implements the OpenXR API standard from Khronos and runs on Linux, Android, and Windows.

## Build Commands

### Standard CMake Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
```

### With CNSDK Support
CNSDK is found via `find_package(CNSDK CONFIG)`. Ensure CNSDK is installed and discoverable by CMake. The build will warn if CNSDK is not found.

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
- **compositor/** - Display compositor handling distortion, layer rendering, and display output. CNSDK integration lives here in `main/comp_renderer.c`
- **drivers/** - Hardware drivers for various HMDs and controllers (Vive, WMR, PSVR, Rift S, etc.)
- **state_trackers/oxr/** - OpenXR API implementation
- **ipc/** - Inter-process communication for service mode
- **targets/** - Build targets producing final binaries (monado-cli, monado-service, OpenXR runtime library)

### Key Interfaces
The codebase uses C interfaces with vtable-style polymorphism. Key structures:
- `struct xrt_device` - Abstract device interface (HMDs, controllers)
- `struct xrt_compositor` - Graphics compositor interface
- `struct xrt_instance` - Runtime instance
- `struct xrt_prober` - Device discovery

### CNSDK Integration
This fork adds Leia SDK integration for light field displays:
- Controlled by `XRT_HAVE_CNSDK` CMake option (auto-enabled if CNSDK found)
- Interlacer integration in `src/xrt/compositor/main/comp_renderer.c`
- Uses `leia_interlacer_vulkan` for Vulkan-based interlacing

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
- `XRT_HAVE_CNSDK` - Enable CNSDK/Leia SDK support
- `XRT_FEATURE_SERVICE` - Enable out-of-process service mode
- `XRT_BUILD_DRIVER_*` - Enable specific hardware drivers
- `BUILD_TESTING` - Enable test suite
