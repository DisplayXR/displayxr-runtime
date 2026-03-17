# DisplayXR Runtime

An open-source [OpenXR](https://www.khronos.org/openxr/) runtime for glasses-free 3D displays — autostereoscopic monitors and laptops that deliver head-tracked stereoscopic 3D without worn hardware.

Built on [Monado](https://monado.freedesktop.org/) by Collabora, DisplayXR strips away headset-centric infrastructure (34 VR drivers, Vulkan server compositor, tracking subsystems) and replaces it with a lightweight runtime purpose-built for 3D displays: ~150 files, 3 drivers, native compositors for every graphics API.

## Design Principles

### Native compositors — no Vulkan intermediary

Every graphics API gets its own compositor that talks directly to the display pipeline. D3D11 apps render through a D3D11 compositor; Metal apps through Metal; and so on. No format conversion, no interop overhead, no GPU compatibility surprises.

### Lightweight and focused

Only what 3D displays need: 3 drivers (leia, sim_display, qwerty), native compositors, and the OpenXR state tracker. Every file in the tree is relevant.

### Four app classes

| Class | Suffix | Description |
|-------|--------|-------------|
| **Handle** | `_handle` | App provides its own window handle via `XR_EXT_*_window_binding`. Runtime composites into it. |
| **Texture** | `_texture` | App provides textures. Runtime composites into its own window. |
| **Hosted** | `_hosted` | Runtime creates the window and rendering targets (standard OpenXR/WebXR path). |
| **IPC/Service** | `_ipc` | App runs out-of-process via client compositor → IPC → server multi-compositor. Foundation for multi-app spatial shell. |

Test app naming follows the pattern `cube_{class}_{api}_{platform}`: `cube_handle_metal_macos`, `cube_texture_d3d11_win`, `cube_hosted_d3d11_win` (hosted), `cube_ipc_d3d11` (service mode).

## Compositor Status

| API | Windows | macOS |
|-----|---------|-------|
| D3D11 | Shipping | — |
| D3D12 | Shipping | — |
| Metal | — | Shipping |
| OpenGL | Shipping | Shipping |
| Vulkan | Shipping | Shipping |

## OpenXR Extensions

| Extension | Purpose |
|-----------|---------|
| `XR_EXT_win32_window_binding` | App provides its own Win32 HWND for OpenXR rendering (windowed mode, multi-app) |
| `XR_EXT_cocoa_window_binding` | App provides a Cocoa NSView for rendering (macOS) |
| `XR_EXT_android_surface_binding` | App provides an Android Surface for rendering |
| `XR_EXT_display_info` | Exposes physical display geometry, canonical viewing pyramid, nominal viewer position, and recommended render resolution scaling |

See the [full extension proposal](docs/specs/XR_EXT_display_info.md) for the formal specification.

## Quick Start

### Windows (Primary Platform)

Requires Visual Studio 2022, CMake, Ninja, and a vendor SDK (e.g., [Leia SR SDK](https://www.leiainc.com/)).

```bash
# Set SDK path
set LEIASR_SDKROOT=C:\path\to\SimulatedReality

# Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja -DCMAKE_PREFIX_PATH=%LEIASR_SDKROOT%
cmake --build .
```

### macOS

macOS builds use the sim_display driver as a vendor-neutral test target. No vendor hardware required.

```bash
# Prerequisites
brew install cmake ninja eigen vulkan-sdk

# Build
./scripts/build_macos.sh
```

### Running Without Installing

```bash
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./your_openxr_app
```

See [CLAUDE.md](CLAUDE.md) for full build details, CMake options, and CI configuration.

## Simulation Driver

You don't need a 3D display to develop against this runtime. The **sim_display** driver provides a simulated tracked 3D display with keyboard-controlled eye position, letting you test the full OpenXR pipeline on any machine:

```bash
# After building, run the test cube app
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./build/test_apps/cube_handle_vk_macos/cube_handle_vk_macos
```

Use WASD + mouse to move the simulated eye position and observe perspective-correct stereo rendering.

## Architecture

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

### Source tree (`src/xrt/`)

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
- **auxiliary/** — Shared utilities: math, OS abstraction, Vulkan/D3D helpers
- **targets/** — Build targets (runtime library, displayxr-cli, displayxr-service)

## Future Direction

- **Multi-compositor spatial shell** — platform-native window manager for 3D displays ([#43](https://github.com/dfattal/openxr-3d-display/issues/43), [#44](https://github.com/dfattal/openxr-3d-display/issues/44))
- **Display extensions** — rendering mode enumeration ([#8](https://github.com/dfattal/openxr-3d-display/issues/8)), multiview support ([#5](https://github.com/dfattal/openxr-3d-display/issues/5)), display mode events ([#3](https://github.com/dfattal/openxr-3d-display/issues/3))
- **Interface standardization** — unified display processor interface ([#45](https://github.com/dfattal/openxr-3d-display/issues/45)), display spatial model ([#46](https://github.com/dfattal/openxr-3d-display/issues/46))

See the [milestone tracker](https://github.com/dfattal/openxr-3d-display/milestones) for the full roadmap.

## Unity Plugin

The **DisplayXR** Unity plugin lives in a separate repository:
[**dfattal/unity-3d-display**](https://github.com/dfattal/unity-3d-display)

Install via Unity Package Manager with the git URL:
```
https://github.com/dfattal/unity-3d-display.git
```

The plugin intercepts Unity's OpenXR pipeline to provide Kooima asymmetric frustum projection for stereo rendering on 3D displays. It has no source dependency on this runtime — install the runtime separately and point `XR_RUNTIME_JSON` to it.

## Branch Structure

| Branch | Purpose |
|--------|---------|
| `main` | Active development — submit PRs here |
| `legacy-monado-ci` | Full Monado codebase (34 VR drivers, Vulkan server compositor) — archived reference |
| `upstream-monado` | Tracks upstream [Monado](https://gitlab.freedesktop.org/monado/monado) (locked, read-only) |

## Contributing

We welcome contributions! The workflow:

1. Fork the repository
2. Create a feature branch off `main`
3. Submit a PR to `main`
4. CI (Windows + macOS) must pass
5. Review by a maintainer

See [CONTRIBUTING.md](CONTRIBUTING.md) for detailed guidelines.

### For Display Vendors

If you're a display vendor looking to integrate your hardware with this runtime, see the [vendor integration guide](docs/specs/vendor-integration.md).

## Key Documentation

- [Extension Proposal](docs/specs/XR_EXT_display_info.md) — formal specification of the four proposed extensions
- [Project Structure](docs/architecture/project-structure.md) — architecture and source tree reference
- [Stereo 3D Math](docs/architecture/stereo3d-math.md) — Kooima projection and stereo math reference
- [Vendor Integration Guide](docs/specs/vendor-integration.md) — how to add support for a new display vendor
- [Documentation Index](docs/README.md) — complete documentation index

## License

This project is licensed under the [ISC License](LICENSE), the same as upstream Monado.

## Acknowledgments

Built on [Monado](https://monado.freedesktop.org/) by [Collabora](https://www.collabora.com/) and the open-source XR community. Leia SR SDK integration by [Leia Inc.](https://www.leiainc.com/)
