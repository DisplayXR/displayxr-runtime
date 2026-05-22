# Building DisplayXR

## Prerequisites

### Windows
- Visual Studio 2022
- CMake
- Ninja
- Vendor display SDK — optional (see [Vendor SDK](#vendor-sdk) below). The **sim_display** driver works without any vendor SDK, providing a simulated 3D display with WASD + mouse eye control.

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

### Windows (recommended)
```bat
scripts\build_windows.bat all
```

The script auto-fetches everything needed: vcpkg (for cJSON +
runtime deps), the OpenXR loader release zip, and the LeiaSR SDK
release artifact (from this repo's `sr-sdk-v*` release). It then
runs CMake (Ninja Multi-Config) and builds the runtime + installer
in one go. Outputs land in `_package/`.

Available targets: `all` (default), `build` (runtime only,
fastest iteration), `installer`, `test-apps`, `generate`. See
the script's header for the full list.

Requires: VS 2022 with C++ workload, Ninja
(`winget install Ninja-build.Ninja`), Vulkan SDK
(`winget install KhronosGroup.VulkanSDK`), GitHub CLI
(`winget install GitHub.cli` + `gh auth login`).

### Windows (manual cmake fallback)
For advanced users who want fine-grained control:
```bat
:: Set vendor SDK path (optional but recommended for real hardware)
set LEIASR_SDKROOT=C:\path\to\LeiaSR-SDK

:: Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja -DCMAKE_PREFIX_PATH=%LEIASR_SDKROOT%
cmake --build .
```

### Standard CMake Build (no vendor SDK)
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
```
Builds the runtime with sim_display only — useful for local dev
without hardware.

## Vendor SDK

DisplayXR builds and runs without any vendor SDK — the **sim_display** driver simulates a tracked 3D display for development. To enable support for a specific display, obtain the SDK from the vendor and point CMake at it.

### Leia SR Displays

1. Get the Leia SR SDK from [Leia Inc](https://www.leiainc.com/) (requires a developer account)
2. Set the environment variable before building:
   ```bash
   set LEIASR_SDKROOT=C:\path\to\LeiaSR-SDK
   ```
3. CMake will auto-detect the SDK via `find_package(simulatedreality)` and enable `XRT_HAVE_LEIA_SR`

The SDK provides display-specific weavers (D3D11, D3D12, Vulkan) and eye tracking via LookaroundFilter. See the [Vendor Integration Guide](../guides/vendor-integration.md) for details on integrating other display hardware.

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

## Local Dev Iteration

The recommended workflow for iterating on the runtime — without ever installing it system-wide and without copying anything into `C:\Program Files\DisplayXR\Runtime\`.

### How the OpenXR loader picks a runtime

When an OpenXR app calls `xrCreateInstance`, the bundled Khronos `openxr_loader.dll` selects which runtime DLL to load using this precedence (Windows):

1. **`XR_RUNTIME_JSON` environment variable** — if set to the path of a valid manifest file, that manifest's `library_path` is the runtime DLL the loader maps. This is the primary dev-iteration knob.
2. **Registry fallback** — `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime`. The DisplayXR installer writes this to point at `C:\Program Files\DisplayXR\Runtime\DisplayXR_win64.json`, which in turn points at the installed `DisplayXRClient.dll`. End-user installations land here; `XR_RUNTIME_JSON` is not needed for them.

`scripts\build_windows.bat` auto-generates a dev manifest at `build\Release\openxr_displayxr-dev.json` with an absolute `library_path` to the worktree's freshly-built `DisplayXRClient.dll`. Every `_package\run_*_win.bat` script sets `XR_RUNTIME_JSON` to that dev manifest before launching the test app.

### Recommended workflow

Run dev test apps from a **non-elevated** terminal. From an admin terminal the Khronos loader silently refuses `XR_RUNTIME_JSON` (see [Elevated terminal caveat](#elevated-terminal-caveat) below).

```cmd
:: From a non-elevated terminal (cmd or PowerShell):
scripts\build_windows.bat build              :: rebuild runtime only (fast)
_package\run_cube_handle_d3d11_win.bat       :: launch cube against worktree's runtime
```

`scripts\build_windows.bat` doesn't itself need elevation. The only operations that do — writing to Program Files, modifying `HKLM\Software\...`, running the NSIS installer — are exactly the ones the dev-iteration workflow is built to avoid.

### Verifying which DLL was actually loaded

Every `xrCreateInstance` writes one WARN line near the top of the per-process runtime log at `%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.<pid>_<timestamp>.log`:

```
[oxr_instance_create] DisplayXR runtime v1.3.3 'v1.3.3' loaded from:
  <absolute path to loaded DLL>
  (XR_RUNTIME_JSON=<env var value, or "<unset>">)
```

The path is authoritative — it's `GetModuleFileNameW` on the live DLL handle, so it reflects what was actually mapped into the process regardless of what the loader claims. If you see your worktree's `build\…\DisplayXRClient.dll`, you're iterating against the dev build. If you see `C:\Program Files\DisplayXR\Runtime\DisplayXRClient.dll` while `XR_RUNTIME_JSON` was set, the env var was refused — either you're running elevated, or the manifest path is invalid.

### Parallel worktrees

Each worktree's `XR_RUNTIME_JSON` and `_package\run_*_win.bat` resolve to that worktree's own dev manifest and runtime DLL, so multiple worktrees iterate independently — no Program Files conflict, no DLL-stomping between simultaneously-running agents.

### Elevated terminal caveat

The Khronos `openxr_loader.dll` (confirmed in **both 1.1.38 and 1.1.43**) silently refuses `XR_RUNTIME_JSON` when the calling process token has high integrity (admin / UAC-elevated). The loader prints to stderr:

```
Error [GENERAL | platform_utils | OpenXR-Loader] : !!! WARNING !!!
Environment variable XR_RUNTIME_JSON is being ignored due to running
from an elevated context. The value '...' will NOT be used.
```

and falls through to the registry → Program Files runtime. There is no env-var or registry-side override; this is a Khronos loader security mitigation.

If you must run dev test apps from an elevated context, the legacy workaround is:

```cmd
copy /Y build\src\xrt\targets\openxr\Release\DisplayXRClient.dll ^
       "C:\Program Files\DisplayXR\Runtime\DisplayXRClient.dll"
```

…after each build. The signature log will then show the Program Files path. This breaks parallel-worktree iteration (one shared Program Files DLL), which is the original motivation for keeping the terminal non-elevated.

### Installing the runtime for end-user testing

For exercising the *installed* code paths (installer, registry, `ActiveRuntime`-driven discovery), build and run the installer instead.

**Windows:**

```cmd
scripts\build_windows.bat installer
_package\DisplayXRSetup-*.exe                :: UAC prompt, one-click install
```

This writes `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime` and `HKLM\Software\DisplayXR\*`, adds Program Files to system PATH, and registers the runtime as discoverable. After install, OpenXR apps with no `XR_RUNTIME_JSON` set will resolve through the registry to the installed runtime.

**macOS:**

```bash
./scripts/build_macos.sh --installer
sudo installer -pkg _package/DisplayXR-Installer-*.pkg -target /
```

This drops the runtime + sim-display plug-in tree into `/Library/Application Support/DisplayXR/`, symlinks `/etc/xdg/openxr/1/active_runtime.json` to the installed manifest, and registers the plug-in via the system-wide discovery root at `/Library/Application Support/DisplayXR/DisplayProcessors/`. After install, OpenXR apps with no `XR_RUNTIME_JSON` set will resolve through the symlink to the installed runtime.

Uninstall: `sudo "/Library/Application Support/DisplayXR/uninstall.sh"` — removes the install tree, the active_runtime symlink, the bundled test .app, and the `pkgutil` receipts.

> **Note on Gatekeeper (unsigned `.pkg`):** the installer is unsigned today, so double-clicking it in Finder triggers Gatekeeper's "DisplayXR cannot be verified" prompt. Two workarounds:
> - `sudo installer -pkg ... -target /` from the terminal — works regardless of signing, recommended for developer use.
> - Right-click the `.pkg` in Finder → Open → "Open" in the confirmation dialog — one-time bypass per file.
>
> Notarization is tracked in [#280](https://github.com/DisplayXR/displayxr-runtime/issues/280) (macOS) and [#281](https://github.com/DisplayXR/displayxr-runtime/issues/281) (Windows). Both ship today as unsigned-with-workaround.

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
