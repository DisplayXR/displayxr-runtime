# Building DisplayXR

## Developer quickstart (build from source)

Fresh clone → a runtime you can run test apps against, in one command.

**Windows** (run the one-liner from an **elevated** prompt — it writes the registry):
```bat
git clone https://github.com/DisplayXR/displayxr-runtime && cd displayxr-runtime
scripts\dev-setup.bat                  REM build runtime + register sim-display + set active runtime + VS solution
scripts\dev-setup.bat --leia           REM ...also build + register the Leia SR plug-in from source
```
Then run a test app (from a **non-elevated** prompt):
```bat
scripts\build_windows.bat test-apps
_package\run_cube_handle_d3d11_win.bat
```

Prefer the explicit steps? Same result:
```bat
scripts\build_windows.bat all          REM 1. build runtime + installer + test apps
scripts\register_dev_plugin.bat        REM 2. register the sim-display plug-in (ELEVATED) — REQUIRED, see note
scripts\register_dev_plugin.bat leia C:\path\to\DisplayXR-LeiaSR.dll   REM (optional) register a dev-built Leia plug-in
_package\run_cube_handle_d3d11_win.bat REM 3. run a test app
```

> **Windows requires an explicit plug-in registration step.** Building from
> source produces the sim-display plug-in DLL but does **not** register it, and
> Windows plug-in discovery is **registry-only**
> (`HKLM\Software\DisplayXR\DisplayProcessors`). Without step 2 the runtime
> starts but finds no display processor and fails with
> `XRT_ERROR_DEVICE_CREATION_FAILED` / "Failed to initialize OpenXR".
> `dev-setup.bat` and `register_dev_plugin.bat` perform this registration for
> you. (POSIX/macOS needs no registration — the loader finds plug-ins next to
> the runtime.)
>
> **A registered plug-in can still fail this check.** If you previously ran the
> released installer, an older `sim-display` / `leia-sr` is registered — but a
> from-source runtime built ahead of that release **ABI-rejects** it
> (`negotiate returned -17` in the log, "no registered plug-in claimed the
> system"), producing the *same* `XRT_ERROR_DEVICE_CREATION_FAILED` / "Failed to
> initialize OpenXR". A registered plug-in is not necessarily an ABI-matched one.
> The fix is the same — register your freshly-built plug-in (step 2), which
> overwrites the stale entry. Diagnose headlessly first:
> ```bat
> _package\bin\displayxr-cli.exe selftest   REM PASS = DP found & ABI-matched; FAIL dumps the reason
> ```
> The runtime also logs the underlying cause (`negotiate returned …`, which
> plug-in was rejected) to its per-process log at
> `%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.<pid>_<timestamp>.log`.
>
> **An elevated prompt re-triggers the same failure even after a correct
> registration.** Registration is machine-wide and permanent
> (`HKLM\Software\DisplayXR\DisplayProcessors`) — it is *not* per-session. But
> *which runtime loads* is: `run_*.bat` points `XR_RUNTIME_JSON` at your
> from-source runtime, and the Khronos loader **silently ignores
> `XR_RUNTIME_JSON` in an elevated/admin process** (see
> [Elevated terminal caveat](#elevated-terminal-caveat)), falling back to the
> *installed* runtime via `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime`. That
> older runtime then ABI-rejects your freshly-registered plug-in, failing with
> `XRT_ERROR_DEVICE_CREATION_FAILED` / "Failed to initialize OpenXR" — so it
> looks like the registration "only applied to the one prompt where you ran it".
> It didn't.
> Either **run test apps from a non-elevated prompt** (so `XR_RUNTIME_JSON` is
> honored), or run `scripts\dev-setup.bat` once so `ActiveRuntime` points at your
> dev build and *every* app — any prompt, no env var needed — loads it.

**macOS:**
```bash
brew install cmake ninja eigen vulkan-sdk
git clone https://github.com/DisplayXR/displayxr-runtime && cd displayxr-runtime
./scripts/build_macos.sh               # runtime + sim-display + test apps
_package/DisplayXR-macOS/run_cube_handle_metal.sh   # run a test app (sim-display found automatically)
```

To **debug in Visual Studio**, see [Debugging in Visual Studio](#debugging-in-visual-studio).
The rest of this doc is reference (prerequisites, CMake options, the loader /
`ActiveRuntime` mechanism, vendor displays).

## Prerequisites

### Windows
- Visual Studio 2022 (with the **C++ workload**; gives you MSVC + the VS generator)
- CMake
- Ninja — **install via `winget install Ninja-build.Ninja`.** `scripts\build_windows.bat`
  locates Ninja only on `PATH` / at the winget install path; the copy bundled with VS's
  "CMake" component is **not** picked up, so the script's `where ninja` check fails if Ninja
  was installed any other way (or not at all).
- Vulkan SDK — `winget install KhronosGroup.VulkanSDK`
- GitHub CLI — `winget install GitHub.cli` then `gh auth login` (the script fetches the OpenXR loader)
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

Builds the runtime, OpenXR loader, and test apps. The macOS Vulkan native compositor runs via MoltenVK over a CAMetalLayer-backed surface (see `cube_handle_vk_macos`); an earlier `VK_ERROR_EXTENSION_NOT_PRESENT` runtime failure was a MoltenVK-era issue that has since been resolved. The remaining macOS-Vulkan dev gotcha is a two-`libvulkan` loader-image conflict (the dev build's image vs the installed runtime's) — share one loader image / pin `XR_RUNTIME_JSON` to avoid it.

### Linux

> **Preview — hardware-validated on NVIDIA / Ubuntu 22.04, pre-GA.** Linux is Vulkan-only (native compositor over an X11/XCB surface) and distributed as a user-level tarball; no installer asset ships on releases yet. Phase status: `docs/roadmap/linux-support.md`.

```bash
./scripts/build_linux.sh              # headless build + selftest (deps list in the script header)
./scripts/build_linux.sh --service    # + displayxr-service / IPC
./scripts/package_linux.sh            # tarball + install.sh (user-level install, #705)
```

`package_linux.sh` emits `dist/displayxr-runtime-linux-<arch>-<ver>.tar.gz`; its
`install.sh` registers the OpenXR ActiveRuntime (`~/.config/openxr/1/`), the
sim-display display processor (`~/.local/share/DisplayXR/DisplayProcessors/`),
and a systemd `--user` unit — no root. `sudo ./install.sh --system` for
machine-wide. Dev iteration without installing stays `XR_RUNTIME_JSON` +
`XRT_PLUGIN_SEARCH_PATH` per `docs/roadmap/linux-support.md`.

### Windows (recommended)
```bat
scripts\build_windows.bat all
```

The script auto-fetches everything needed: vcpkg (for cJSON +
runtime deps) and the OpenXR loader release zip. It then runs CMake
(Ninja Multi-Config) and builds the runtime + installer in one go.
Outputs land in `_package/`. No vendor SDK is fetched — the runtime
DLL holds zero vendor identifiers (ADR-019); vendor display processors
ship as separate plug-ins (see [Vendor displays](#vendor-displays)
below).

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
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
```

### Standard CMake Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
```
Builds the runtime with sim_display as the in-tree vendor-neutral
display processor. No vendor SDK is needed at build time.

### Build troubleshooting (Windows)

The build is correct, but a couple of *environment* issues can make a clean clone look
broken. All of these succeed on a retry / clean reconfigure — they are not code defects.

- **`LINK : fatal error LNK1105: cannot close file '…\*.lib'; error code 1224`** (or random
  link/`Detecting CXX compiler ABI info - failed` failures) — a real-time **antivirus / EDR
  scanner** is holding a just-written file open while MSVC's parallel build races to use it
  (`1224` = `ERROR_USER_MAPPED_FILE`). It is **not** specific to any folder. Fixes: add the
  build tree (and ideally `%TEMP%`) to your AV's real-time-scan exclusions, or just re-run
  `scripts\build_windows.bat all` — the Ninja build is incremental and resumes where it
  stopped.
- **A test app fails with `LNK1120: NN unresolved externals` on `xr*` symbols** (e.g.
  `xrCreateSwapchain`, `xrEndFrame`) — the app linked against a **wrong-architecture**
  `openxr_loader.lib`. This happens when a per-app CMake configure was interrupted (so the
  compiler ABI / `CMAKE_SIZEOF_VOID_P` wasn't detected) and `FindOpenXR` fell back to a
  **stale machine-wide OpenXR SDK** (e.g. a hand-installed `C:\dev\openxr_sdk` that ships both
  `Win32\` and `x64\`). Fixes: delete the offending `test_apps\<app>\build\` folder and
  re-run `scripts\build_windows.bat test-apps`; and remove/rename any stale machine-level
  OpenXR SDK (the script supplies its own versioned loader and passes `-DOpenXR_ROOT`).
- **`ninja not found`** even though Ninja seems installed — see the Ninja prerequisite above;
  install it with `winget install Ninja-build.Ninja` (the VS-bundled Ninja is not used).
- **VS solution builds but apps fail at runtime** ("Failed to initialize OpenXR") — the VS
  (Debug) tree and the Ninja (`_package`, Release) tree are separate; after building
  `XRT.sln` you **must** repoint the active runtime with **`scripts\setup-vs-runtime.bat`**.
  See [Debugging in Visual Studio](#debugging-in-visual-studio).

## Vendor displays

The runtime DLL holds zero vendor identifiers in its link line
(ADR-019, issues #256/#263). To run on real vendor hardware:

1. Install the runtime via `DisplayXRSetup-*.exe`.
2. Install the matching vendor plug-in installer.

### Leia SR Displays

Install `DisplayXRLeiaSRSetup-*.exe` from
[DisplayXR/displayxr-leia-plugin](https://github.com/DisplayXR/displayxr-leia-plugin/releases).
The plug-in installer hard-requires the runtime (reads
`HKLM\Software\DisplayXR\Runtime\InstallPath`) and registers itself at
`HKLM\Software\DisplayXR\DisplayProcessors\leia-sr` so the runtime's
registry-driven discovery loads it at `xrCreateInstance` time.

The plug-in repo also vends the vendor SDK fetched at build time (headers +
import stubs only) — no end-user steps to install the vendor SDK separately
for runtime use. The vendor's platform/runtime installer (for Leia, the SR
Platform installer) remains a hard prereq for the service that hosts eye
tracking and weaver instances on the hardware side.

### Other vendors

See [vendor plug-in onboarding guide](../guides/vendor-plugin-onboarding.md)
(post-#263) for the contract a new vendor's plug-in repo must satisfy:
implement `xrt_plugin_iface`, install to `$RuntimeInstall\Plugins\<id>\`,
register at `HKLM\Software\DisplayXR\DisplayProcessors\<id>`.

## Key CMake Options

| Option | Description |
|--------|-------------|
| `XRT_FEATURE_SERVICE` | Out-of-process service mode (IPC) |
| `BUILD_TESTING` | Build test suite |

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

## Debugging in Visual Studio

`scripts\build_windows.bat vs2022` generates `build_vs2022\XRT.sln` (the
quickstart's `dev-setup.bat` runs this for you). Two things to understand first:

- **There are two build trees.** `dev-setup.bat` / `build_windows.bat build`
  produce the **Ninja, Release** runtime (`build\` → `_package\`); `vs2022`
  produces a **separate VS, Debug** tree (`build_vs2022\`). The active OpenXR
  runtime (`HKLM\Software\Khronos\OpenXR\1\ActiveRuntime`) decides which one apps
  load — after `dev-setup.bat` that's `_package`, so **you must repoint it at the
  VS build before debugging there.**
- **Co-location.** The VS build puts every binary together in
  `build_vs2022\bin\<cfg>\` (mirroring `_package\bin`), so the service finds the
  runtime core DLL when run from the build tree.

### Steps

```bat
:: 1. generate, then open build_vs2022\XRT.sln and build ALL_BUILD (Debug)
scripts\build_windows.bat vs2022

:: 2. point the active runtime at the VS build (ELEVATED). Uses the dev manifest
::    the VS build already generates — nothing to hand-write/paste.
scripts\setup-vs-runtime.bat            REM Debug, sim-display
scripts\setup-vs-runtime.bat --restore  REM undo (restore previous runtime)

:: 3. verify
build_vs2022\bin\Debug\displayxr-cli.exe selftest
```

Then in VS set **`displayxr-service`** as the startup project and **F5** —
breakpoints in the runtime, compositor, and plug-in bind. To debug an app too,
launch it (from a **non-elevated** prompt — see the elevated-terminal caveat
above) and use **Debug ▸ Attach to Process**. (The test apps are separate CMake
projects, so they're not in `XRT.sln`.)

### Real Leia weaving in the debugger (not just sim)

The SR SDK ships **Release-only**, so a *Debug* plug-in CRT-mismatches it. Build
the whole stack **RelWithDebInfo** (Release CRT + PDBs):

```bat
:: build XRT.sln in RelWithDebInfo, then ELEVATED:
scripts\setup-vs-runtime.bat RelWithDebInfo --leia
```

`--leia` builds the Leia plug-in against this checkout (ABI/CRT-matched) and
registers it ahead of sim-display; it falls back to sim if the build/load fails.

### Gotchas

- **"Failed to initialize OpenXR"** — `ActiveRuntime` points at a missing
  manifest. Re-run `setup-vs-runtime.bat` (don't hand-write the JSON — pasted
  `reg`/`echo` get smart-quoted and a typo'd path breaks it).
- **"No tests ran" running a project in `XRT.sln`** — the `tests_comp_*` projects
  are hidden Catch2 cases, not the sample apps.
- **Purple/blank on the wrong monitor** — sim-display fallback, not Leia (the
  `dev-setup --leia` plug-in was built against the *Release* runtime, so the
  *Debug* runtime rejects it). Use the RelWithDebInfo `--leia` flow for real
  weaving. `…\DisplayXR_<exe>.*.log` shows which DP loaded / was rejected.

### Alternative: VS Code (CMake Tools)

Prefer VS Code over the Visual Studio IDE? It debugs the **Ninja** build (no
`XRT.sln`) via the CMake Tools extension and the in-tree `CMakePresets.json`.

1. **Extensions:** install *CMake Tools* and *C/C++* (Microsoft). Prereqs are the
   same as above (VS 2022 **Build Tools** for the MSVC compiler, Ninja, Vulkan
   SDK, GitHub CLI). Run `scripts\build_windows.bat build` once first so vcpkg and
   the OpenXR loader are fetched (the presets reference them).
2. **Open the repo folder.** CMake Tools picks up `CMakePresets.json`; choose the
   **"Debug service"** configure preset (Ninja, `CMAKE_BUILD_TYPE=Debug`, with the
   vcpkg toolchain + `OpenXR_ROOT` already wired). **Configure**, then build the
   **`install`** target → a Debug runtime + sim-display land in `_package\`.
3. **Activate it (elevated, once):** `scripts\register_dev_plugin.bat` registers
   the `_package` sim-display, and point the active runtime at the Debug build:
   `reg add "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime /t REG_SZ /d "%CD%\_package\DisplayXR_win64.json" /f` (or just `scripts\dev-setup.bat --no-vs`,
   which builds + registers + activates in one shot — Release, fine for stepping
   the runtime). Verify: `_package\bin\displayxr-cli.exe selftest`.
4. **Debug:** add a launch config (`.vscode/launch.json`, `type: cppvsdbg`) that
   either launches `${command:cmake.launchTargetPath}` (e.g. `displayxr-cli` with
   `args: ["selftest"]`, or `displayxr-service`) or **attaches** to a running app /
   `displayxr-service`. Breakpoints in the runtime + plug-in bind. For real Leia
   weaving the same RelWithDebInfo/CRT rule applies — build the *Release service*
   preset and register a RelWithDebInfo Leia plug-in (`scripts\register_dev_plugin.bat leia <dll>`).

> The Visual Studio (`XRT.sln`) flow above is the primary, best-tested path;
> VS Code is the lighter-weight alternative and shares the same registry /
> active-runtime mechanics.

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
