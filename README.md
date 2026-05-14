<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/displayxr_white.png" width="120">
  <source media="(prefers-color-scheme: light)" srcset="assets/displayxr.png" width="120">
  <img alt="DisplayXR" src="assets/displayxr.png" width="120">
</picture>

# DisplayXR Runtime

[![Build Windows](https://github.com/DisplayXR/displayxr-runtime/actions/workflows/build-windows.yml/badge.svg)](https://github.com/DisplayXR/displayxr-runtime/actions/workflows/build-windows.yml)
[![Build macOS](https://github.com/DisplayXR/displayxr-runtime/actions/workflows/build-macos.yml/badge.svg)](https://github.com/DisplayXR/displayxr-runtime/actions/workflows/build-macos.yml)
[![License: BSL-1.0](https://img.shields.io/badge/License-BSL--1.0-blue.svg)](LICENSE)

An open-source [OpenXR](https://www.khronos.org/openxr/) runtime for spatial displays — 3D monitors and laptops with tracked stereo and multiview lightfield display technology.

Built on [Monado](https://monado.freedesktop.org/) by Collabora, DisplayXR strips away headset-centric infrastructure (34 VR drivers, Vulkan server compositor, tracking subsystems) and replaces it with a lightweight runtime purpose-built for 3D displays: ~150 files, 3 drivers, native compositors for every graphics API.

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
 D3D11 D3D12 Vulkan  Metal   OpenGL   ← native compositors
   |    |     |        |        |
   Display Processor (LeiaSR / sim_display)
        |
   Display
```

Every graphics API gets its own native compositor — no Vulkan intermediary, no interop overhead. Vendor-specific processing (interlacing, lenticular weaving) is isolated in the display processor layer.

| API | Windows | macOS |
|-----|---------|-------|
| D3D11 | Shipping | — |
| D3D12 | Shipping | — |
| Metal | — | Shipping |
| OpenGL | Shipping | Shipping |
| Vulkan | Shipping | Shipping |

## Quick Start

```bash
# Windows — recommended (auto-fetches vcpkg, OpenXR loader, LeiaSR SDK)
scripts\build_windows.bat all
# Outputs: _package/DisplayXRSetup-*.exe (installer) + _package/bin/

# macOS
brew install cmake ninja eigen vulkan-sdk && ./scripts/build_macos.sh

# Run without installing
XR_RUNTIME_JSON=./build/Release/openxr_displayxr-dev.json ./your_openxr_app
```

Pre-built installers from [GitHub Releases](https://github.com/DisplayXR/displayxr-runtime/releases). For the spatial-workspace experience, also install the [DisplayXR Shell](https://github.com/DisplayXR/displayxr-shell-releases/releases) (separate installer, registers with the runtime via the [workspace controller contract](docs/specs/runtime/workspace-controller-registration.md)).

See [Building DisplayXR](docs/getting-started/building.md) for full instructions and CMake options.

## Simulation Driver

No 3D display required. The **sim_display** driver provides a simulated tracked display with WASD + mouse eye position control:

```bash
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./build/test_apps/cube_handle_vk_macos/cube_handle_vk_macos
```

## Documentation

| I want to... | Start here |
|---|---|
| **Build apps** for 3D displays | [Getting Started](docs/getting-started/overview.md) |
| **Contribute** to DisplayXR | [Contributing Guide](docs/guides/contributing.md) |
| **Integrate my display** hardware | [Vendor Integration Guide](docs/guides/vendor-integration.md) |
| See the full docs index | [Documentation Index](docs/README.md) |
| See the project roadmap | [Roadmap](docs/roadmap/overview.md) |

### Key References

- [App Classes](docs/getting-started/app-classes.md) — handle, texture, hosted, IPC
- [XR_EXT_display_info](docs/specs/extensions/XR_EXT_display_info.md) — display properties and rendering mode extension
- [Kooima Projection](docs/architecture/kooima-projection.md) — stereo math and projection pipelines
- [Separation of Concerns](docs/architecture/separation-of-concerns.md) — layer boundaries
- [displayxr-mcp](https://github.com/DisplayXR/displayxr-mcp) — embeddable MCP server framework. End users opt in to AI-agent / voice control by installing **DisplayXR MCP Tools** ([releases](https://github.com/DisplayXR/displayxr-mcp/releases)), which writes `HKLM\Software\DisplayXR\Capabilities\MCP\Enabled=1`; the runtime reads this at startup and spawns a per-app MCP server. `DISPLAYXR_MCP=1` (or `=0`) is still supported as a process-local override for CI / dev. Runtime registers Phase A handle-app introspection tools (`list_sessions`, `get_display_info`, `capture_frame`, `tail_log`, …) per app process; the reference shell hosts Phase B workspace tools. Spec at [`displayxr-mcp/docs/mcp-spec.md`](https://github.com/DisplayXR/displayxr-mcp/blob/main/docs/mcp-spec.md).

## Related Repos

| Repo | Description |
|------|-------------|
| [displayxr-shell-releases](https://github.com/DisplayXR/displayxr-shell-releases) | DisplayXR Shell — spatial workspace controller (installer + bug reports) |
| [displayxr-extensions](https://github.com/DisplayXR/displayxr-extensions) | OpenXR extension specs and headers |
| [displayxr-mcp](https://github.com/DisplayXR/displayxr-mcp) | Embeddable MCP server framework + **DisplayXR MCP Tools** installer (end-user opt-in for agent / voice control) |
| [displayxr-demo-gaussiansplat](https://github.com/DisplayXR/displayxr-demo-gaussiansplat) | 3D Gaussian Splatting reference demo |
| [displayxr-unity](https://github.com/DisplayXR/displayxr-unity) | Unity engine plugin (UPM package) |
| [displayxr-unreal](https://github.com/DisplayXR/displayxr-unreal) | Unreal Engine plugin |
| [kooima-projection](https://github.com/DisplayXR/kooima-projection) | Off-axis frustum projection math library |

## Contributing

We welcome contributions! See the [contributing guide](docs/guides/contributing.md) for workflow, code style, and CI expectations.

## License

Boost Software License 1.0
