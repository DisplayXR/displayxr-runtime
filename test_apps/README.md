# Test Apps

Minimal OpenXR cube applications that exercise every app class and graphics API supported by DisplayXR. Each app renders a rotating cube with head-tracked stereo on a 3D display (or the sim_display driver for development without hardware).

## App Classes

DisplayXR supports three app classes, differing in who owns the window and rendering targets:

| Class | Description | When to use |
|-------|-------------|-------------|
| **Handle** (`_handle`) | App creates its own window and passes the handle (HWND, NSView) to the runtime via `XR_EXT_win32_window_binding` or `XR_EXT_cocoa_window_binding`. | Native apps that need full control over their window. Most common class. |
| **Texture** (`_texture`) | App renders to offscreen textures, runtime composites into its own window. | Apps embedding 3D content in a larger 2D UI. |
| **Hosted** (`_hosted`) | Runtime creates everything — window, swapchains, rendering targets. Standard OpenXR path. | Simplest integration. Also the path for WebXR content. |

All three classes use a native compositor in-process — no IPC, no server process needed. See [App Classes](../docs/getting-started/app-classes.md) for the full reference.

### Extension vs Legacy

Each class has **extension** and **legacy** variants:

- **Extension apps** (default) use `XR_EXT_display_info` to query display properties, select rendering modes (2D/stereo/multiview), and control tile layout.
- **Legacy apps** (`_legacy` suffix) don't use `XR_EXT_display_info`. The runtime auto-detects them and computes tile scaling via a compromise formula. This is the path for unmodified OpenXR apps and WebXR content.

## Naming Convention

```
cube_{class}_{api}_{platform}
```

- **class**: `handle`, `texture`, `hosted`, `hosted_legacy`
- **api**: `d3d11`, `d3d12`, `vk`, `gl`, `metal`
- **platform**: `win`, `macos`

## Test App Matrix

### Windows — Handle Apps (all APIs)

| App | API | Description |
|-----|-----|-------------|
| `cube_handle_d3d11_win` | D3D11 | Primary test app. Window binding via HWND. |
| `cube_handle_d3d12_win` | D3D12 | D3D12 native compositor path. |
| `cube_handle_vk_win` | Vulkan | Vulkan native compositor path. |
| `cube_handle_gl_win` | OpenGL | OpenGL native compositor path. |

### Windows — Texture Apps

| App | API | Description |
|-----|-----|-------------|
| `cube_texture_d3d11_win` | D3D11 | App provides offscreen textures, runtime composites. |
| `cube_texture_d3d12_win` | D3D12 | D3D12 texture path. |

### Windows — Hosted Apps

| App | API | Description |
|-----|-----|-------------|
| `cube_hosted_d3d11_win` | D3D11 | Runtime creates window and targets. Simplest path. |

### Windows — Legacy Apps (no XR_EXT_display_info)

| App | API | Description |
|-----|-----|-------------|
| `cube_hosted_legacy_d3d11_win` | D3D11 | Legacy hosted mode, runtime computes tile scaling. |
| `cube_hosted_legacy_d3d12_win` | D3D12 | Legacy hosted, D3D12. |
| `cube_hosted_legacy_vk_win` | Vulkan | Legacy hosted, Vulkan. |
| `cube_hosted_legacy_gl_win` | OpenGL | Legacy hosted, OpenGL. |

### macOS — Handle Apps

| App | API | Description |
|-----|-----|-------------|
| `cube_handle_metal_macos` | Metal | Window binding via NSView. Primary macOS test app. |
| `cube_handle_vk_macos` | Vulkan | Vulkan via MoltenVK. |
| `cube_handle_gl_macos` | OpenGL | OpenGL native compositor. |

### macOS — Texture & Hosted Apps

| App | API | Description |
|-----|-----|-------------|
| `cube_texture_metal_macos` | Metal | App provides Metal textures, runtime composites. |
| `cube_hosted_metal_macos` | Metal | Runtime creates window and targets. |

### macOS — Legacy Apps

| App | API | Description |
|-----|-----|-------------|
| `cube_hosted_legacy_metal_macos` | Metal | Legacy hosted, Metal. |
| `cube_hosted_legacy_vk_macos` | Vulkan | Legacy hosted, Vulkan via MoltenVK. |
| `cube_hosted_legacy_gl_macos` | OpenGL | Legacy hosted, OpenGL. |

## Building

### Windows

```bash
scripts\build_windows.bat test-apps
```

Or build individually:
```bash
cd test_apps\cube_handle_d3d11_win
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT=path\to\openxr_sdk
cmake --build .
```

### macOS

Test apps are built automatically by `scripts/build_macos.sh`. Binaries go to `test_apps/{app}/build/`.

## Running

Point `XR_RUNTIME_JSON` at the dev build:

```bash
# macOS
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos
```

On Windows, use the generated run scripts in `_package/`:
```bash
_package\run_cube_handle_d3d11_win.bat
```

### Running in the Shell

The [DisplayXR Shell](https://github.com/DisplayXR/displayxr-shell-releases) launches standard handle apps and manages multi-app compositing via IPC transparently — no app changes needed:

```bash
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

## Shared Code

`test_apps/common/` contains shared utilities used across test apps (D3D helpers, OpenXR boilerplate, cube geometry).
