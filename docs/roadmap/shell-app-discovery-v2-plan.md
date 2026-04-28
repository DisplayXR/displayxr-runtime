# Shell App Discovery v2: Drop-in Manifest Registration

**Branch:** `in-shell-apps-manifest`
**Tracking issue:** TBD (this repo, label `shell`)
**Depends on:** Phase 5 launcher (merged) and the v1.0 manifest spec

## Why this work

The v1 launcher only discovers apps in a closed set of paths — dev-tree
`test_apps/*/build/`, `demos/*/build/`, `_package/bin/`, plus the single production
path `%PROGRAMFILES%\DisplayXR\apps\` (`shell_app_scan.c::shell_scan_apps`). Real
third-party apps (Unity / Unreal builds in `Documents\Unity\…`,
`Documents\Unreal Projects\…`, etc.) never get discovered. The Browse-for-app
fallback works but produces a default-iconed tile with `type:"3d"`, no description,
no display-mode preference — and the entry is stored as raw fields in
`registered_apps.json`, which mixes "what apps exist" state with "where windows
were last placed" state.

We want third-party app installers (and engine plugins) to be able to register an
app by dropping a single `.displayxr.json` file into a known directory, without
the app needing to be installed under `Program Files`. We also want the
Browse-for-app path to flow through the same registration mechanism so user-added
apps are first-class citizens with the same metadata pipeline as installer-added
apps.

## Design

### Two manifest modes, one parser

`.displayxr.json` v1.0 grows one optional field — `exe_path`. The field's
presence is the dispatch:

- **Sidecar mode** (existing): manifest sits next to the exe in a scanned
  dev path; `exe_path` MUST be absent; the exe is the sibling file.
- **Registered mode** (new): manifest sits in a discovery dir and `exe_path`
  is required.

Schema-wise this is additive — same `schema_version: 1`, older shells just
fall back to sidecar resolution and skip registered manifests gracefully. See
`docs/specs/displayxr-app-manifest.md` §2, §3.2.

### Discovery dirs (registered mode)

```
%LOCALAPPDATA%\DisplayXR\apps\          ← per-user (no elevation)
%ProgramData%\DisplayXR\apps\           ← system-wide (installer needs elevation)
```

Scanner walks these first, then the existing dev-tree sidecar paths, then
`%PROGRAMFILES%\DisplayXR\apps\`. Dedup is by case-insensitive
slash-normalized `exe_path` — discovery order defines precedence. Per-user
wins over system-wide, which lets a user override a vendor registration
without uninstalling.

### Browse-for-app rewrite

`shell_browse_and_add_app` (currently `main.c:1292`) stops appending raw
fields to `g_registered_apps[]`. New behavior:

1. `GetOpenFileNameA` → exe path.
2. Sanitize basename (`[^A-Za-z0-9._-]` → `_`).
3. Extract the embedded PE icon via `ExtractIconExA` → save as
   `<sanitized>.png` in `%LOCALAPPDATA%\DisplayXR\apps\` (GDI+ encode). If
   extraction fails, omit `icon`.
4. Write `<sanitized>.displayxr.json` with `schema_version:1`, `name` from
   the basename (or PE `FileDescription` if cheap to read), `type:"3d"`,
   `category:"app"`, `exe_path` set, optional `icon`.
5. Trigger a re-scan + push to service.

`registered_apps.json` becomes a state cache only — saved poses, MRU,
hide flags. The `source:"user"` vs `source:"scan"` distinction is removed:
everything is registered via a manifest file.

### Remove → manifest delete

Launcher's remove action (currently `main.c:2152`) deletes the manifest file
when it's under one of the registered dirs. ProgramData deletes may fail
without elevation; on `ERROR_ACCESS_DENIED` we hide the tile via state
cache instead. Sidecar manifests in dev paths are never deleted (they're
developer-owned).

## Files touched

| File | Change |
|---|---|
| `docs/specs/displayxr-app-manifest.md` | Spec bump — `exe_path` field, two modes, two new dirs, icon-as-Windows-app-icon section |
| `docs/roadmap/shell-app-discovery-v2-plan.md` | This doc |
| `src/xrt/targets/shell/shell_app_scan.h` | `shell_scanned_app` gains `manifest_path` so the launcher can delete it on remove |
| `src/xrt/targets/shell/shell_app_scan.c` | New `scan_registered_dir` walks `*.displayxr.json`; manifest parser refactored to be exe-source-agnostic; dedup-by-`exe_path`; `shell_scan_apps` calls registered-dir scan first |
| `src/xrt/targets/shell/main.c` | `shell_browse_and_add_app` writes manifest + extracts PE icon; `registered_apps_load` drops user/scan distinction; remove path deletes manifest file |

## Existing utilities reused

- `parse_sidecar` in `shell_app_scan.c:109` — already takes `exe_path` as a
  parameter, so registered mode just supplies it from the JSON instead of
  the sibling file.
- `read_file_text` and `resolve_icon_path` in `shell_app_scan.c` — reused
  unchanged for registered mode.
- `exe_path_equal` in `main.c:298` — slash-normalized case-insensitive
  comparison for dedup.
- `ExtractIconExA` (Win32 `<shellapi.h>`) — extracts the PE app icon for
  the Browse flow.
- GDI+ `Bitmap::Save` — encodes the extracted icon to PNG.

## Plugin-side follow-ups

Plugin-repo issues track the engine-side work:

- **`DisplayXR/displayxr-unity`** — `DisplayXRBuildProcessor.cs` already
  writes a sidecar manifest next to the built exe. Add a "Register with
  DisplayXR shell" toggle to `DisplayXRManifestSettings`; when on, ALSO
  write a registered manifest to `%LOCALAPPDATA%\DisplayXR\apps\` with
  `exe_path` set + copy icons there. Add an editor warning when Player
  Settings → Icon and the manifest icon both exist but differ.

- **`DisplayXR/displayxr-unreal`** — no manifest pipeline today. Add a
  `UDisplayXRManifestSettings` (`UDeveloperSettings` subclass) for the
  same fields as Unity. Editor module exports settings to a small JSON in
  `Saved/Config/`; `Scripts/PackageApp.py` reads it post-cook and writes
  the manifest next to the staged exe + optional registered manifest. Same
  icon-mismatch warning vs. Project Settings → Game Icon.

## Verification

End-to-end on Windows:

1. `scripts\build_windows.bat build` — rebuild runtime + shell.
2. Copy a Unity build's exe to `Documents\test-app\`. Hand-author
   `%LOCALAPPDATA%\DisplayXR\apps\test_unity.displayxr.json` with
   `exe_path` pointing at it + an `icon.png` sibling. Launch shell —
   tile appears with the right icon and is launchable.
3. Move the exe so `exe_path` no longer resolves. Restart shell — tile is
   skipped with a warning in the shell log.
4. Use Browse-for-app on a fresh exe. Verify a `.displayxr.json` + `.png`
   appear under `%LOCALAPPDATA%\DisplayXR\apps\`. Restart shell — same
   tile re-appears (proves persistence is via manifest, not
   `registered_apps.json`).
5. Drop a manifest with the same `exe_path` into
   `%ProgramData%\DisplayXR\apps\`. Verify the per-user one wins.
6. Click remove on a Browse-added tile. Verify the manifest file is
   gone from `%LOCALAPPDATA%\DisplayXR\apps\`.
7. Capture compositor screenshot via `%TEMP%\shell_screenshot_trigger`
   to confirm tile rendering.

## Out of scope

- Vendor-prefixed manifest filenames (`<vendor>.<app>.displayxr.json`) —
  current dedup-by-`exe_path` handles collisions; revisit if real-world
  collisions show up.
- App update / version tracking — `version`, `min_runtime`, etc. are
  reserved in the spec for a future schema bump.
- Per-app launch args / working directory — `args`, `working_dir`
  reserved for a future bump.
- Cross-platform discovery (macOS, Linux). Registered mode is Windows-only
  in this slice; the macOS shell port is deferred per CLAUDE.md.
