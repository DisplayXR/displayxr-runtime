---
name: make-app-logos
description: |
  Generate REAL 2D + 3D workspace logos and a valid displayxr.json manifest for a
  DisplayXR app from a live atlas capture. Launches the app, waits until it's focused
  and rendering, file-triggers a projection-only atlas capture, then splits the centre
  two views into a 512x512 2D logo + a 1024x512 two-view 3D logo and writes/updates the
  schema-v1 manifest. Complements /new-displayxr-app (which ships only placeholder icons).
  Windows-first.
  Usage: /make-app-logos <exe-path> [name="Display Name"] [args="..."] [type=3d|2d]
         [category=app] [description="..."] [crop-nudge=DX,DY] [wait=2] [--register]
  Examples:
    /make-app-logos test_apps/cube_handle_d3d11_win/build/cube_handle_d3d11_win.exe name="Cube D3D11"
    /make-app-logos C:/apps/photo.exe name="Photo" args="C:/pics/sample.jpg" --register
allowed-tools: Read, Grep, Glob, Bash, Edit, Write
---

# make-app-logos — real logos + manifest from a live capture

Making an app workspace/shell-ready needs a 2D logo, a 3D two-view logo, and a valid
`displayxr.json` manifest. This skill produces all three from the app's **own rendered
content** — vs `/new-displayxr-app`, which only drops correctly-sized **placeholder** art.

The heavy lifting (force-opaque, tile split, square crop, resize, manifest serialization)
lives in **`scripts/make_app_logos.py`**; this skill orchestrates the live capture and
wiring around it. The output is gated by **`scripts/check_displayxr_app.py`** (INV-9.x).

## Why the file-trigger projection path
The runtime exposes a file-triggered, projection-only atlas capture that works for **any**
running 3D DisplayXR app with no app cooperation and no synthetic key injection (which the
`I`-key `xrCaptureAtlasEXT` path would need, and which Unity-style apps reject). Output lands
at a fixed, deterministic path. Mechanism (see `test_apps/common/atlas_capture.h` header doc
and `u_capture_intent.h`):
- **Trigger:** create empty `%TEMP%\displayxr_atlas_trigger.projection`.
- **Output:** runtime writes `%TEMP%\displayxr_atlas.projection.png` within a frame or two —
  the per-tile projection atlas before any window-space/HUD compose.
- The PNG is force-opaque on recent runtimes; the helper force-opaques again regardless.

## Inputs
Resolve from the invocation; ask (AskUserQuestion) only if the exe is missing/ambiguous:
- **exe-path** (required) — the built app executable to capture.
- **name** — launcher display name (1–64 chars). Default: a title-cased exe stem.
- **args** — launch args (e.g. a file/dir to open). Default: none.
- **type** `3d|2d` (default `3d`), **category** (default `app`), **description**, **display_mode**.
- **crop-nudge** `DX,DY` px — shift the square crop centre. Default `0,0`.
- **wait** seconds to render after FOCUSED before capturing. Default `2`.
- **--register** — also install into `%LOCALAPPDATA%\DisplayXR\apps\` for an immediate
  launcher test (registered mode: `exe_path` + app-specific icon names).

Manifest/icon destination: **`<exe-dir>\displayxr\`** (sidecar) for an in-repo or self-bundled
app. The manifest stem is the exe stem (`<exe-stem>.displayxr.json`).

## Procedure

1. **Resolve inputs.** Derive the manifest stem from the exe stem; pick the `displayxr/`
   output dir next to the exe (create it). Note whether the app is in-repo (`test_apps/...`)
   for the CMake step.

2. **Pre-flight.** Ensure a DisplayXR dev runtime is active for the app — either the installed
   runtime (registry) or a dev build via `XR_RUNTIME_JSON`. Use a **non-elevated** terminal
   (the bundled loader ignores `XR_RUNTIME_JSON` when elevated). Delete any stale capture so you
   don't read an old frame:
   ```bash
   rm -f "$TEMP/displayxr_atlas.projection.png" "$TEMP/displayxr_atlas_trigger.projection"
   ```
   (PowerShell: `$env:TEMP`.) The app must run in a **3D mode** (tiles cols/rows > 1) for a
   meaningful atlas — native cube apps default to one; for a mono app the 3D logo degenerates.

   **The target app must be the ONLY DisplayXR app rendering.** The projection trigger is
   **process-global**: every running native compositor polls the same `%TEMP%` trigger file, and
   the first to poll consumes it and writes *its* atlas — so a second app (e.g. a video player)
   will silently win the race and you'll capture the wrong content. Check for other DisplayXR
   clients before launching and abort (or warn the user) if any are up:
   ```bash
   powershell.exe -NoProfile -Command "Get-Process | ? { \$_.ProcessName -match 'cube_|_handle_|_hosted_|_texture_|mediaplayer|modelviewer|gaussian|depthview|XRInteractive' } | Select Id,ProcessName"
   ```
   (The `displayxr-service` IPC compositor is a separate, non-global path and does not race this.)

3. **Launch + wait for FOCUSED.** Run the exe (with `args`) via Bash `run_in_background`,
   capturing stdout to a log. Poll the log for the `FOCUSED` session-state line (printed by
   `xr_session_common.cpp`), then sleep `wait` seconds so content is rendered. If `FOCUSED`
   never appears within a reasonable window, report and stop (likely no runtime / wrong DLL —
   check the `loaded from:` line in `%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.*.log`).

4. **Trigger + collect.** Create the trigger, then poll for the output PNG and copy it aside:
   ```bash
   : > "$TEMP/displayxr_atlas_trigger.projection"
   # poll up to ~3s for the PNG, then:
   cp "$TEMP/displayxr_atlas.projection.png" "$TEMP/make_app_logos_atlas.png"
   ```

5. **Stop the app.** Kill **only** the PID you launched (never by process age — that can kill
   the user's terminal).

6. **Generate logos + manifest.**
   ```bash
   python3 scripts/make_app_logos.py \
     --atlas "$TEMP/make_app_logos_atlas.png" \
     --out-dir "<exe-dir>/displayxr" \
     --name "<name>" --type <type> --category <category> \
     --description "<description>" [--crop-nudge DX,DY]
   ```
   Writes `icon.png` (512x512), `icon_sbs.png` (1024x512, `sbs-lr`), and
   `<stem>.displayxr.json`.

7. **CMake wiring (in-repo apps only).** Ensure the app's `CMakeLists.txt` copies `displayxr/`
   next to the built exe. Most cube apps already do; if not, add (per INV-8):
   ```cmake
   include(${CMAKE_CURRENT_SOURCE_DIR}/../common/displayxr_manifest.cmake)
   displayxr_install_manifest(<target> "${CMAKE_CURRENT_SOURCE_DIR}/displayxr")
   ```
   For an out-of-repo app, report the `displayxr/` dir and that the build/installer must copy it
   next to the exe (or use `--register`).

8. **Optional `--register`.** Re-run the helper to install into the shared drop-in dir with an
   absolute, forward-slash `exe_path` and app-specific icon names:
   ```bash
   python3 scripts/make_app_logos.py --atlas "$TEMP/make_app_logos_atlas.png" \
     --out-dir "$LOCALAPPDATA/DisplayXR/apps" --name "<name>" \
     --manifest-name "<stem>" --exe-path "<abs/exe/path>"
   ```

9. **Lint gate.** Run `PYTHONUTF8=1 python3 scripts/check_displayxr_app.py <exe-dir>` (or the app
   source dir). Must be ERROR-free with no icon-dimension WARNs. Re-run until clean. (`PYTHONUTF8=1`
   avoids the linter crashing on its `✓` success line under a non-UTF-8 Windows console.)

10. **Report.** The capture used, the files written, and how to see it: build the app so CMake
    copies `displayxr/` next to the exe, then launch via the workspace/shell launcher (registered
    apps appear immediately). Per the screenshot-reliability rule, ask the user to eyeball the
    live tile if visual correctness matters.

## Guardrails
- **Only the target app may be rendering** during capture — the projection trigger is
  process-global; a concurrent DisplayXR app will win the race and you'll capture its frame
  (verified the hard way against a running media player). See pre-flight step 2.
- **No key injection** — the file-trigger path needs none; never `SendInput`/`keybd_event` the app.
- **Force-opaque always** (the helper does) — alpha-blended renderers leave atlas alpha << 1.
- **Real JSON serializer + forward-slash `exe_path`** (the helper does) — never string-template
  a manifest; `\U...` etc. produce invalid JSON the scanner rejects.
- **Icon sizes are fixed:** 2D 512x512, 3D 1024x512 `sbs-lr`. The linter WARNs on drift.
- In prose/identifiers use **tile / view / atlas** language; only the literal manifest value
  `sbs-lr` and the file name `icon_sbs.png` are exempt (established API surface).
- Don't modify the compositor/runtime to make capture work — this is an app-side tool.
