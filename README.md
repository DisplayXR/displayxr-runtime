# DisplayXR Runtime

Core OpenXR runtime purpose-built for 3D displays. Implements the OpenXR API standard with native compositors for every major graphics API — no Vulkan intermediary.

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

## Supported Platforms

- **Windows** — D3D11, D3D12, OpenGL, Vulkan
- **macOS** — Metal, OpenGL, Vulkan (MoltenVK)
- **Android** — Planned

## App Classes

| Class | Description |
|-------|-------------|
| **Window-handle** (`_ext`) | App provides its own window via `XR_EXT_*_window_binding` |
| **Shared-texture** (`_shared`) | App provides textures, runtime composites into its own window |
| **Runtime-managed** (`_rt`) | Runtime creates window and rendering targets (standard OpenXR/WebXR) |
| **IPC/Service** (`_ipc`) | Out-of-process via client compositor → IPC → multi-compositor |

## Building

```bash
# macOS
./scripts/build_macos.sh

# Standard CMake
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
```

## Running

```bash
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./your_openxr_app
```

## Related Repos

| Repo | Description |
|------|-------------|
| [displayxr-unity](https://github.com/DisplayXR/displayxr-unity) | Unity engine plugin (UPM package) |
| [displayxr-unreal](https://github.com/DisplayXR/displayxr-unreal) | Unreal Engine plugin |
| [displayxr-extensions](https://github.com/DisplayXR/displayxr-extensions) | OpenXR extension specs and headers |
| [kooima-projection](https://github.com/DisplayXR/kooima-projection) | Off-axis frustum projection math library |
| [displayxr-demos](https://github.com/DisplayXR/displayxr-demos) | Demo applications |
| [displayxr-shell](https://github.com/DisplayXR/displayxr-shell) | Spatial shell / 3D window manager |

## License

Boost Software License 1.0
