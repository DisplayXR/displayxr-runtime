# Building DisplayXR

## Prerequisites

### Windows
- Visual Studio 2022
- CMake
- Ninja
- Vendor SDK (e.g., [Leia SR SDK](https://www.leiainc.com/)) — optional, sim_display works without it

### macOS
```bash
brew install cmake ninja eigen vulkan-sdk
```

## Quick Build

### macOS
```bash
./scripts/build_macos.sh
```

Builds the runtime, OpenXR loader, and test apps. The Vulkan compositor will fail at runtime with `VK_ERROR_EXTENSION_NOT_PRESENT` (MoltenVK limitation, not a build issue).

### Windows
```bash
# Set vendor SDK path (optional)
set LEIASR_SDKROOT=C:\path\to\SimulatedReality

# Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja -DCMAKE_PREFIX_PATH=%LEIASR_SDKROOT%
cmake --build .
```

### Standard CMake Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
```

## Key CMake Options

| Option | Description |
|--------|-------------|
| `XRT_HAVE_LEIA_SR` | LeiaSR SDK support (auto-enabled if SDK found) |
| `XRT_HAVE_LEIA_SR_VULKAN` | Vulkan-specific Leia weaver |
| `XRT_HAVE_LEIA_SR_D3D11` | D3D11-specific Leia weaver |
| `XRT_FEATURE_SERVICE` | Out-of-process service mode (IPC) |
| `BUILD_TESTING` | Build test suite |

### CMake Variables

- `LEIASR_SDKROOT` — Environment variable pointing to Leia SR SDK path
- `SR_PATH` — Internal, auto-set from `LEIASR_SDKROOT`

## Running Without Installing

Point `XR_RUNTIME_JSON` at the build output:

```bash
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./your_openxr_app
```

## Running Tests

```bash
cd build && ctest
```

## Code Formatting

```bash
git clang-format    # Format only your changes (preferred)
scripts/format-project.sh   # Format all
```

## CI Builds

**Windows** (`.github/workflows/build-windows.yml`):
- Requires `LEIASR_SDKROOT` + `CMAKE_PREFIX_PATH`
- Artifact: `DisplayXR`

**macOS** (`.github/workflows/build-macos.yml`):
- Vulkan SDK via MoltenVK, bundles libvulkan + OpenXR loader
- Artifact: `DisplayXR-macOS`
