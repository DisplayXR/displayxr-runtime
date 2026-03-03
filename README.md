# OpenXR Runtime for Tracked 3D Displays

An open-source [OpenXR](https://www.khronos.org/openxr/) runtime that brings standardized XR support to glasses-free 3D displays — autostereoscopic monitors and laptops that deliver head-tracked stereoscopic 3D without worn hardware.

## The Problem

OpenXR assumes a head-mounted display. The runtime owns the screen, creates rendering targets, and locks views to the user's head. None of this maps to a tracked 3D display, where:

- The display is a **shared desktop monitor**, not a private headset — apps need to render into their own windows
- The display has **fixed physical geometry** — apps may need raw screen dimensions for custom camera models (e.g., Kooima off-axis projection)
- Views are **eye-tracked, not head-locked** — the runtime must expose viewer eye positions relative to a fixed screen

Without a standard API, every display vendor ships a proprietary SDK, fragmenting the ecosystem and forcing developers to write separate codepaths for each vendor.

## Proposed Extensions

This project implements three OpenXR extensions to close that gap:

| Extension | Purpose |
|-----------|---------|
| `XR_EXT_win32_window_binding` | App provides its own Win32 HWND for OpenXR rendering (windowed mode, multi-app) |
| `XR_EXT_android_surface_binding` | Same concept for Android — app provides a Surface for rendering |
| `XR_EXT_display_info` | Exposes physical display geometry, canonical viewing pyramid, nominal viewer position, and recommended render resolution scaling |

See the [full extension proposal](doc/extensions/XR_EXT_tracked_3d_display_proposal.md) for the formal specification.

## Quick Start

### Windows (Primary Platform)

Requires Visual Studio 2022, CMake, Ninja, and the [Leia SR SDK](https://www.leiainc.com/).

```bash
# Set SDK path
set LEIASR_SDKROOT=C:\path\to\SimulatedReality

# Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja -DCMAKE_PREFIX_PATH=%LEIASR_SDKROOT%
cmake --build .
```

### macOS (Development / Testing)

macOS builds are useful for development and testing with the simulation driver. No vendor hardware required.

```bash
# Prerequisites
brew install cmake ninja eigen vulkan-sdk

# Build
./scripts/build_macos.sh
```

> **Note:** The Vulkan compositor won't function at runtime on macOS due to MoltenVK lacking `VK_KHR_external_memory_fd`. This is expected — use the sim_display driver for testing.

### Running Without Installing

```bash
XR_RUNTIME_JSON=./build/openxr_monado-dev.json ./your_openxr_app
```

See [CLAUDE.md](CLAUDE.md) for full build details, CMake options, and CI configuration.

## Simulation Driver

You don't need a 3D display to develop against this runtime. The **sim_display** driver provides a simulated tracked 3D display with keyboard-controlled eye position, letting you test the full OpenXR pipeline on any machine:

```bash
# After building, run the test cube app
XR_RUNTIME_JSON=./build/openxr_monado-dev.json ./build/test_apps/cube_vk_macos/cube_vk_macos
```

Use WASD + mouse to move the simulated eye position and observe perspective-correct stereo rendering.

## Branch Structure

| Branch | Purpose |
|--------|---------|
| `main` | Active development — submit PRs here |
| `upstream-monado` | Tracks upstream [Monado](https://gitlab.freedesktop.org/monado/monado) (locked, read-only) |
| `cnsdk` | Historical reference — early integration with Leia CNSDK (archived) |

## Contributing

We welcome contributions! The workflow:

1. Fork the repository
2. Create a feature branch off `main`
3. Submit a PR to `main`
4. CI (Windows + macOS) must pass
5. Review by a maintainer

See [CONTRIBUTING.md](CONTRIBUTING.md) for detailed guidelines.

### For Display Vendors

If you're a display vendor looking to integrate your hardware with this runtime, see the [vendor integration guide](doc/extensions/vendor_integration_guide.md) and the [vendor abstraction refactor](doc/extensions/vendor_abstraction_refactor.md) design document.

## Key Documentation

- [Extension Proposal](doc/extensions/XR_EXT_tracked_3d_display_proposal.md) — formal specification of the three proposed extensions
- [Implementation Plan](doc/extensions/OpenXR_Tracked_3D_Display_Extensions.md) — design and implementation guidance
- [Vendor Integration Guide](doc/extensions/vendor_integration_guide.md) — how to add support for a new display vendor
- [Vendor Abstraction Refactor](doc/extensions/vendor_abstraction_refactor.md) — architecture for multi-vendor support

## Roadmap

Active work items tracked as [GitHub Issues](https://github.com/dfattal/openxr-3d-display/issues):

| # | Item | Status |
|---|------|--------|
| [#1](https://github.com/dfattal/openxr-3d-display/issues/1) | Genericize IPC server view pose computation | refactor |
| [#2](https://github.com/dfattal/openxr-3d-display/issues/2) | Remove legacy CNSDK interlacing path | cleanup |
| [#3](https://github.com/dfattal/openxr-3d-display/issues/3) | Event system: display mode + eye tracking state changes | extension |
| [#4](https://github.com/dfattal/openxr-3d-display/issues/4) | Vendor rendering mode API | design needed |
| [#5](https://github.com/dfattal/openxr-3d-display/issues/5) | Multiview support (raise XRT_MAX_VIEWS + register view config) | extension |
| [#6](https://github.com/dfattal/openxr-3d-display/issues/6) | D3D12 native compositor | future |

## Architecture

This is a fork of [Monado](https://monado.freedesktop.org/), the open-source OpenXR runtime by Collabora. Key additions:

- **LeiaSR driver** (`src/xrt/drivers/leiasr/`) — Vulkan and D3D11 weavers for light field interlacing
- **D3D11 native compositor** (`src/xrt/compositor/d3d11/`) — bypasses Vulkan for Intel GPU compatibility
- **Simulation driver** (`src/xrt/drivers/sim_display/`) — virtual tracked 3D display for development
- **Window binding extension** — `XR_EXT_win32_window_binding` implementation in the OpenXR state tracker

See [CLAUDE.md](CLAUDE.md) for a detailed source tree walkthrough.

## Unity Plugin

The **Monado 3D Display** Unity plugin lives in a separate repository:
[**dfattal/unity-3d-display**](https://github.com/dfattal/unity-3d-display)

Install via Unity Package Manager with the git URL:
```
https://github.com/dfattal/unity-3d-display.git
```

The plugin intercepts Unity's OpenXR pipeline to provide Kooima asymmetric frustum projection for stereo rendering on 3D displays. It has no source dependency on this runtime — install the runtime separately and point `XR_RUNTIME_JSON` to it.

## License

This project is licensed under the [ISC License](LICENSE), the same as upstream Monado.

## Acknowledgments

Built on [Monado](https://monado.freedesktop.org/) by [Collabora](https://www.collabora.com/) and the open-source XR community. Leia SR SDK integration by [Leia Inc.](https://www.leiainc.com/)
