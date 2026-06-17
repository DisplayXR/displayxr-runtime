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

The VS build already **generates** a dev runtime manifest at
`build_vs2022\<cfg>\openxr_displayxr-dev.json` whose `library_path` points at the
build-tree `DisplayXRClient.dll`. Just make it the active runtime and register
the VS-built sim-display — run the committed helper from an **elevated** prompt:

```bat
scripts\setup-vs-runtime.bat            REM Debug (default)
scripts\setup-vs-runtime.bat Release
scripts\setup-vs-runtime.bat --restore  REM put the previous active runtime back
```

> **Do not hand-write the JSON or paste `reg`/`echo` commands from chat/Slack** —
> they smart-quote straight quotes into `"` `"`, which the shell rejects, and a
> mistyped path leaves `ActiveRuntime` pointing at a file that doesn't exist
> (→ "Failed to initialize OpenXR"). The script uses the manifest the build
> already generated, so there's nothing to mis-paste.

Verify:

```bat
build_vs2022\bin\Debug\displayxr-cli.exe selftest
```

`selftest` should pass and list the sim-display. For a **Release** session, build
`ALL_BUILD` in Release and run `scripts\setup-vs-runtime.bat Release`.

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

## Real Leia weaving in the debugger (not just sim)

The steps above run on **sim-display**. To debug against the actual Leia plug-in
on a Leia display, the plug-in must be built with a **matching CRT**: the SR SDK
ships **Release-only**, so a *Debug* plug-in CRT-mismatches it. Use
**RelWithDebInfo** for the whole stack (Release CRT + PDBs):

```bat
REM 1) build the solution in RelWithDebInfo (not Debug) in XRT.sln
REM 2) then, elevated:
scripts\setup-vs-runtime.bat RelWithDebInfo --leia
```

`--leia` clones/locates the sibling `displayxr-leia-plugin`, builds it **against
this runtime checkout** in the same config (so it's ABI- and CRT-matched), and
registers it at `leia-sr` (ProbeOrder 50 — wins over sim-display). F5
`displayxr-service` (RelWithDebInfo) and breakpoints in the runtime *and* the
plug-in bind. If the plug-in build or load fails it stays on sim-display so you
can still debug the runtime.

> Why not Debug for Leia? Debug links the Debug CRT; the prebuilt SR SDK is
> Release CRT — mixing them crashes on the first CRT object that crosses. Debug
> is fine for **sim-only** debugging (no SR SDK in the loaded plug-in).

## Restore the previous runtime

```bat
scripts\setup-vs-runtime.bat --restore
```

(Or re-run `scripts\dev-setup.bat`, which sets the active runtime back to
`_package`.)

## Troubleshooting

- **"Failed to initialize OpenXR" after step 2** — `ActiveRuntime` points at a
  manifest that doesn't exist (a mis-pasted/hand-written JSON). Re-run
  `scripts\setup-vs-runtime.bat`, which uses the build-generated manifest.
- **Run apps from a NON-elevated terminal.** The bundled OpenXR loader *ignores*
  `XR_RUNTIME_JSON` in elevated processes and falls back to `ActiveRuntime`. So
  `run_cube_*.bat` launched from an admin prompt won't use its own `XR_RUNTIME_JSON`
  — it uses whatever `ActiveRuntime` is. (Step 2's `ActiveRuntime` repoint is what
  makes elevated launches work too.)
- **"No tests ran" when you Run a project in `XRT.sln`** — the `tests_comp_*`
  projects are Catch2 unit tests, and the relevant case is hidden (`[.]`), so it
  needs explicit selection. Those aren't the sample/cube apps — the cube apps are
  separate CMake projects and **aren't in `XRT.sln`** (debug them via *Attach to
  Process*, above).
- **App runs but shows a purple/blank screen on the wrong monitor** — you're on
  **sim-display** (the fallback), not Leia. The Leia plug-in registered by
  `dev-setup --leia` was built against the *Ninja/Release* runtime, so the
  *VS/Debug* runtime ABI-rejects it and falls back to sim-display, which opens on
  the primary monitor. Check `%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.*.log` for
  which DP loaded / was rejected. For real Leia weaving inside a VS debug session
  you must build the Leia plug-in **Debug against this runtime checkout** and
  register that DLL (`scripts\register_dev_plugin.bat leia <path-to-debug-dll>`);
  otherwise sim-display on the desktop is expected.

## Why not just one build?

The Ninja build is what packaging/CI/`dev-setup` use; the VS build is for
interactive debugging. They coexist because CMake's VS (multi-config) generator
and Ninja generator can't share a build dir. Step 2 is the small bridge that
tells the OS which one is "live" while you're debugging.
