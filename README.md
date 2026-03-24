<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/displayxr_white.png" width="120">
  <source media="(prefers-color-scheme: light)" srcset="doc/displayxr.png" width="120">
  <img alt="DisplayXR" src="doc/displayxr.png" width="120">
</picture>

# DisplayXR Runtime

An open-source [OpenXR](https://www.khronos.org/openxr/) runtime for glasses-free 3D displays — autostereoscopic monitors and laptops that deliver head-tracked stereoscopic 3D without worn hardware.

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
# macOS
brew install cmake ninja eigen vulkan-sdk && ./scripts/build_macos.sh

# Windows (with optional vendor SDK)
set LEIASR_SDKROOT=C:\path\to\SimulatedReality
mkdir build && cd build && cmake .. -G Ninja && cmake --build .

# Run without installing
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./your_openxr_app
```

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
- [XR_EXT_display_info](docs/specs/XR_EXT_display_info.md) — display properties and rendering mode extension
- [Kooima Projection](docs/architecture/kooima-projection.md) — stereo math and projection pipelines
- [Separation of Concerns](docs/architecture/separation-of-concerns.md) — layer boundaries

## Related Repos

| Repo | Description |
|------|-------------|
| [displayxr-unity](https://github.com/DisplayXR/displayxr-unity) | Unity engine plugin (UPM package) |
| [displayxr-unreal](https://github.com/DisplayXR/displayxr-unreal) | Unreal Engine plugin |
| [displayxr-extensions](https://github.com/DisplayXR/displayxr-extensions) | OpenXR extension specs and headers |
| [kooima-projection](https://github.com/DisplayXR/kooima-projection) | Off-axis frustum projection math library |
| [displayxr-demos](https://github.com/DisplayXR/displayxr-demos) | Demo applications |
| [displayxr-shell](https://github.com/DisplayXR/displayxr-shell) | Spatial shell / 3D window manager |

## Contributing

We welcome contributions! See the [contributing guide](docs/guides/contributing.md) for workflow, code style, and CI expectations.

## License

Boost Software License 1.0
