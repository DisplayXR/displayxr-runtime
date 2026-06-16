---
status: Active
owner: David Fattal
updated: 2026-06-16
---

# Debugging DisplayXR in Visual Studio

Manual steps to build the runtime in Visual Studio and debug it (and the apps
that talk to it) with breakpoints. Windows only.

## The one thing to understand first: there are two build trees

`scripts\build_windows.bat build` (and the `dev-setup.bat` orchestrator) produce
a **Ninja, Release** runtime in `build\` → installed to `_package\`. The Visual
Studio solution (`scripts\build_windows.bat vs2022`) is a **separate, Debug**
build in `build_vs2022\`. They are independent.

The active OpenXR runtime — what every app actually loads — is whatever
`HKLM\Software\Khronos\OpenXR\1\ActiveRuntime` points at. After a normal
`dev-setup.bat`, that points at the **Ninja/_package** build. So if you build in
VS but don't repoint it, you're debugging a runtime nothing uses, and your apps
keep loading the Ninja one. **Step 2 below repoints it at your VS build** — do it
once per config (Debug/Release), and everything lines up.

Plug-in discovery is registry-only on Windows
(`HKLM\Software\DisplayXR\DisplayProcessors\*`), independent of which runtime is
active — so step 2 also re-registers the **VS-built** sim-display.

## Step 1 — generate and build the VS solution

```bat
scripts\build_windows.bat vs2022
```

Open `build_vs2022\XRT.sln`, pick the **Debug** configuration, and build
`ALL_BUILD`. Thanks to the build-tree co-location, every binary lands together in
`build_vs2022\bin\Debug\` (mirroring `_package\bin`): `DisplayXRClient.dll`,
`displayxr-service.exe`, `DisplayXR-SimDisplay.dll`, the CLI, etc. — this
co-location is what lets the service find the runtime core DLL when run from the
build tree.

## Step 2 — point the active runtime at the VS build (once per config)

Run from an **elevated** `cmd` **at the repo root**:

```bat
REM repo root -> absolute path
set "REPO=%CD%"
set "CFG=Debug"
set "BIN=%REPO%\build_vs2022\bin\%CFG%"

REM 1) Write a runtime manifest JSON pointing at the VS-built core DLL.
REM    (forward slashes are JSON-safe and valid for LoadLibrary on Windows)
set "CORE=%BIN%/DisplayXRClient.dll"
set "CORE=%CORE:\=/%"
> "%REPO%\build_vs2022\openxr_vs_%CFG%.json" echo {"file_format_version":"1.0.0","runtime":{"library_path":"%CORE%"}}

REM 2) Make it the active OpenXR runtime (backs nothing up — see "Restore" below).
reg add "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime /t REG_SZ /d "%REPO%\build_vs2022\openxr_vs_%CFG%.json" /f

REM 3) Register the VS-built sim-display plug-in (ABI-matched to this build).
reg add "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /v Binary      /t REG_SZ    /d "%BIN%\DisplayXR-SimDisplay.dll" /f
reg add "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /v ProbeOrder  /t REG_DWORD /d 200 /f
reg add "HKLM\Software\DisplayXR\DisplayProcessors\sim-display" /v DisplayName /t REG_SZ    /d "Simulated 3D Display (VS Debug)" /f
```

Verify:

```bat
build_vs2022\bin\Debug\displayxr-cli.exe dp list
build_vs2022\bin\Debug\displayxr-cli.exe selftest
```

`selftest` should pass, listing the sim-display you just registered. (If
`DisplayXR-SimDisplay.dll` isn't directly in `bin\Debug\`, search the tree:
`where /r build_vs2022\bin\Debug DisplayXR-SimDisplay.dll` and use that path.)

For a **Release** debug session, repeat step 2 with `set "CFG=Release"`.

## Step 3 — debug

**Debug the runtime / compositor / plug-in path:** set **`displayxr-service`** as
the startup project and press **F5**. It loads the VS-built `DisplayXRClient.dll`
+ the VS-built sim-display; breakpoints in the runtime, the compositor, and the
plug-in all bind. (`displayxr-cli` is an even faster F5 target for plug-in / DP
init — it runs the runtime in-process with no window: set its debug arguments to
`selftest` or `info`.)

**Debug an app together with the runtime:** start an OpenXR app (an installed
demo, or a `_package\run_cube_*.bat`) — it now loads your **VS Debug** runtime
via `ActiveRuntime`. Then **Debug ▸ Attach to Process** in the VS solution and
attach to the app (and/or `displayxr-service`); breakpoints in the runtime and
plug-in bind because the loaded DLLs are your VS build with matching PDBs.

> The test apps are separate CMake projects, so they aren't in `XRT.sln`. To get
> a one-click **F5 the app** target (instead of attach), see the experimental
> `XRT_VS_LAUNCH_APP` fold in the root `CMakeLists.txt` — not on by default yet.

## Restore the previous runtime

To go back to the installed / Ninja runtime, point `ActiveRuntime` back at it
(e.g. the installer's manifest, or your `_package\DisplayXR_win64.json`):

```bat
reg add "HKLM\Software\Khronos\OpenXR\1" /v ActiveRuntime /t REG_SZ /d "%CD%\_package\DisplayXR_win64.json" /f
```

(Or re-run `scripts\dev-setup.bat`, which sets the active runtime to `_package`.)

## Why not just one build?

The Ninja build is what packaging/CI/`dev-setup` use; the VS build is for
interactive debugging. They coexist because CMake's VS (multi-config) generator
and Ninja generator can't share a build dir. Step 2 is the small bridge that
tells the OS which one is "live" while you're debugging.
