# DisplayXR App Manifest (`.displayxr.json`)

| Field | Value |
|---|---|
| **Spec** | DisplayXR App Manifest |
| **Version** | 1.0 (draft) |
| **Status** | Stable contract — implemented by the DisplayXR Shell (reference workspace controller); usable by any workspace controller that opts in. |
| **Owner** | Workspace extensions (this is a public ecosystem contract, not a shell-internal format). |
| **Applies to** | Any application that wants to appear in a DisplayXR workspace controller's launcher. |

## 1. Purpose

A **DisplayXR workspace controller** — the DisplayXR Shell is the reference implementation; third-party verticals, kiosks, OEM-branded shells, and AI-agent drivers can all play this role — discovers installable apps by scanning a small set of filesystem locations and reading per-app **manifest sidecar files** named `.displayxr.json`. The manifest is the authoritative source of an app's display name, icons, category, and display-mode preferences.

This document specifies the manifest format and the discovery directory convention. Both are part of the **DisplayXR app-installation contract**: an installer that drops a manifest into one of the system-known directories below makes the app discoverable to *any* compliant workspace controller, not just the DisplayXR Shell. The contract is intentionally workspace-controller-agnostic so that an app installed once is found by every controller a user might run.

**Discovery is manifest-gated.** An executable with no manifest is NOT shown in the launcher, even if it imports `openxr_loader.dll`. This is intentional: it keeps the launcher curated, avoids false positives from unrelated OpenXR apps, and gives developers a single explicit contract to satisfy. Users can still manually add any executable through a workspace controller's **Browse for app…** flow (the DisplayXR Shell implements this; see §11); those entries bypass the manifest requirement at scan time but typically *write* a manifest so subsequent scans pick them up.

DisplayXR Unity and Unreal plugins are expected to generate a manifest automatically as part of their build/export pipelines. Native app developers author the manifest by hand.

**Not every executable in a DisplayXR SDK install ships a manifest.** In particular, test-only variants (`cube_hosted_legacy_*`, `cube_texture_*`, `cube_hosted_*`, and similar) intentionally omit a sidecar so they do not appear in the launcher — they exist to exercise runtime paths that are not meaningful to end users. Only apps that a user would plausibly launch from a workspace controller should ship a manifest. Inside this repo, only the `cube_handle_*_win` reference apps carry sidecars.

**Workspace controllers ship no built-in default apps.** When the controller's launcher state cache (e.g. the DisplayXR Shell's `registered_apps.json`) does not exist (first run), the registry starts empty. The scanner immediately populates whatever it finds via manifests; if it finds nothing, the launcher renders the empty-state hint instead of a tile grid. There is no pre-seeded "Notepad" or other system app — the DisplayXR app surface is exclusively a curated workspace surface, never a generic Windows app drawer.

## 2. Manifest modes and file locations

A manifest can live in one of two places, dispatched by whether it carries an `exe_path` field (see §3.2). The parser is the same for both modes — only the exe-resolution rule differs.

### 2.1 Sidecar mode (next to the exe)

The manifest sits in the same directory as its executable, with the filename stem matching the exe:

```
my_app/
├── my_app.exe
├── my_app.displayxr.json     ← manifest (no exe_path)
├── icon.png                  ← referenced from manifest
└── icon_sbs.png              ← optional 3D icon
```

- A workspace controller's scanner looks for `<exe_basename>.displayxr.json` next to each discovered `.exe` in the dev-tree paths (see §5).
- `exe_path` MUST be absent in sidecar mode — the exe is the sibling file.
- Sidecar mode is the natural fit for in-tree dev (`test_apps/`, `demos/`) and for app installs that bundle the manifest into their own install directory.

### 2.2 Registered mode (drop-in directory)

The manifest sits in a system-known discovery directory and points at any executable on disk via an absolute `exe_path`. This is how third-party apps install themselves into a DisplayXR workspace controller's launcher without having to live under `Program Files`.

```
%LOCALAPPDATA%\DisplayXR\apps\           ← per-user installs (no elevation)
└── my_unity_app.displayxr.json          ← exe_path points to user's build dir
%ProgramData%\DisplayXR\apps\            ← system-wide installs (installer-elevated)
└── vendor_app.displayxr.json
```

- The manifest filename can be anything ending in `.displayxr.json`. Recommended convention: `<sanitized_exe_basename>.displayxr.json`.
- `exe_path` MUST be present and resolve to an existing `.exe`. Manifests whose exe no longer exists are skipped with a warning.
- Icon paths are still resolved **relative to the manifest file**, so installers should drop icon files alongside the manifest (not next to the exe).

### 2.3 Common rules

- All paths inside the manifest are resolved **relative to the manifest file**, not the CWD and not the exe directory.
- The scanner never writes to the manifest. It is owned by whoever installed it (developer, installer, or the user via Browse-for-app).

## 3. Schema (v1.0)

```json
{
  "schema_version": 1,
  "name": "Cube D3D11",
  "type": "3d",
  "icon": "icon.png",
  "icon_3d": "icon_sbs.png",
  "icon_3d_layout": "sbs-lr",
  "category": "test",
  "display_mode": "auto",
  "description": "Reference cube demonstrating XR_EXT_win32_window_binding on D3D11."
}
```

### 3.1 Required fields

| Field | Type | Notes |
|---|---|---|
| `schema_version` | integer | Must be `1` for v1.0. Allows future schema migrations. |
| `name` | string | Display name shown on the tile. 1–64 characters. UTF-8. |
| `type` | string | `"3d"` for extension/hosted apps that create an OpenXR session, `"2d"` for legacy Win32 apps a workspace controller captures by HWND. Must match one of these values. |

### 3.2 Optional fields

| Field | Type | Default | Notes |
|---|---|---|---|
| `exe_path` | string | *none* | Absolute path to the executable. **Required in registered mode (§2.2), MUST be absent in sidecar mode (§2.1).** Forward or back slashes accepted; the scanner normalizes to backslashes. The referenced file must exist at scan time or the entry is skipped. |
| `icon` | string | *none* | Relative path to a 2D icon image. PNG or JPEG. Recommended 512×512. When absent, the tile is rendered with the app name as a text label on a category-colored background. |
| `icon_3d` | string | *none* | Relative path to a stereoscopic icon image. When present, the workspace controller renders the tile stereoscopically. Resolution matches `icon` aspect but doubled along the layout axis (e.g. 1024×512 for `sbs-lr`). Requires `icon` to also be set (as the 2D fallback). |
| `icon_3d_layout` | string | `"sbs-lr"` | How the stereo pair is packed in `icon_3d`. One of `"sbs-lr"` (left-right), `"sbs-rl"` (right-left), `"tb"` (top-bottom, left eye on top), `"bt"` (bottom-top). Ignored if `icon_3d` is absent. |
| `category` | string | `"app"` | Free-form tag used for grouping. Reserved values: `"test"`, `"demo"`, `"app"`, `"tool"`. Unknown values are accepted and displayed as-is. |
| `display_mode` | string | `"auto"` | Preferred display mode at launch. `"auto"` lets the runtime choose. Other values are forwarded to the runtime's mode selection. |
| `description` | string | `""` | One-line description shown in tooltips. Max 256 characters. |

### 3.3 Reserved for future versions

Names a workspace controller may consume in later schema versions — do not use for custom data:

`version`, `publisher`, `homepage`, `min_runtime`, `required_extensions`, `screenshots`, `trailer`, `pose`, `window_size`, `args`, `working_dir`.

## 4. 3D icons

The launcher's key visual hook is that app tiles are themselves 3D. A high-quality `icon_3d` is the single biggest contributor to the launcher "wow" moment.

### 4.1 Generating 3D icons

- **Unity / Unreal plugin** — the recommended path. Render two viewpoints from the runtime's stereo camera pair at a fixed convergence plane (typically 0.5–1.0 m), composited as specified by `icon_3d_layout`. A single gameplay snapshot is sufficient; no animation.
- **Native apps** — render two offset views with asymmetric frustums matching the target convergence, save as side-by-side PNG.
- **No 3D icon** — omit `icon_3d`. The tile falls back to `icon` rendered as a flat quad. The launcher still looks good, but without the 3D payoff.

### 4.2 Convergence guidance

A workspace controller's launcher renders tiles at ~40 cm in front of the viewer (the DisplayXR Shell's reference convergence; other controllers may differ). Stereo icons should be authored for a comfortable convergence at roughly that distance. Parallax budget: ±2% of image width.

## 5. Discovery behavior

A workspace controller's scanner walks two kinds of paths in this order. The directory conventions below are the ecosystem standard — every compliant controller scans the same registered-mode dirs so an app installed once is discovered everywhere.

**Registered-mode discovery dirs** (manifests own the exe via `exe_path`):
```
%LOCALAPPDATA%\DisplayXR\apps\          ← per-user, user/installer-writable
%ProgramData%\DisplayXR\apps\           ← system-wide, installer-writable (elevated)
```

These paths are **DisplayXR-runtime-branded, not workspace-controller-branded**. Installers write here regardless of which workspace controller(s) the user runs. Third-party controllers that don't honor this convention can still ship — they just won't see apps installed for the ecosystem.

**Sidecar-mode dev paths** (manifests sit next to the exe). The reference DisplayXR Shell scans these relative to its own exe:
```
<workspace-controller-exe>/../test_apps/*/build/
<workspace-controller-exe>/../demos/*/build/
<workspace-controller-exe>/../_package/bin/
%PROGRAMFILES%\DisplayXR\apps\
```

For each registered-mode dir, the scanner enumerates `*.displayxr.json` directly. For each manifest:

1. Parse + validate per §6.
2. Read `exe_path`. If missing or the file does not exist → skip with warning.
3. Add the entry to the registry; the manifest path is also recorded so the launcher's "remove" action can delete it.

For each sidecar-mode dir, the scanner enumerates `*.exe`. For each exe:

1. Look for `<exe_basename>.displayxr.json` next to it.
2. If absent → **skip the exe entirely**.
3. If present → parse + validate per §6, resolve icon paths.
4. Sanity check: verify the exe imports `openxr_loader.dll` (PE import scan). If not, log a warning and still add the entry (the manifest is authoritative — a 2D `type` app wouldn't import OpenXR anyway).

**Dedup**: across all dirs, entries are deduplicated by case-insensitive `exe_path`. Discovery order above defines precedence — `%LOCALAPPDATA%` wins over `%ProgramData%`, which wins over the dev/Program-Files sidecar paths. Later duplicates are dropped silently.

Manifest parse errors are logged by the workspace controller and the entry is dropped. Malformed manifests do not crash the scanner.

## 6. Validation rules

The scanner rejects a manifest if any of the following are true:

- `schema_version` is missing or not `1`.
- `name` is missing, empty, or longer than 64 characters.
- `type` is missing or not one of `"3d"` / `"2d"`.
- `exe_path` is required (registered mode) but missing, empty, or refers to a file that does not exist.
- `exe_path` is set but the manifest is in a sidecar location — `exe_path` is reserved for registered mode (warning only; the sibling exe still wins).
- `icon` is specified but the referenced file does not exist or is not a readable PNG/JPEG.
- `icon_3d` is specified but the file does not exist or is not a readable PNG/JPEG, or `icon` is not also set.
- `icon_3d_layout` is specified but not one of the four allowed values.

Rejected manifests are logged as warnings. The scanner does not attempt to recover partial data.

## 7. Example: minimal manifest

```json
{
  "schema_version": 1,
  "name": "My App",
  "type": "3d"
}
```

This is enough for the launcher to show a named tile. Without `icon` the tile renders as a text label on a category-colored background.

## 8. Example: full manifest with 3D icon

```json
{
  "schema_version": 1,
  "name": "Gaussian Splatting Demo",
  "type": "3d",
  "icon": "icon.png",
  "icon_3d": "icon_sbs.png",
  "icon_3d_layout": "sbs-lr",
  "category": "demo",
  "display_mode": "auto",
  "description": "Real-time 3D Gaussian splatting rendered on a tracked 3D display."
}
```

## 9. Reusing the 2D icon as the Windows app icon

A workspace controller's launcher renders `icon` as the tile face. The same image should also be the **embedded application icon** in the executable, so Windows uses it for the Start Menu, Desktop shortcut, and taskbar — i.e. the user sees the same icon whether they launch from a DisplayXR workspace controller or from a regular Windows shortcut.

There is no DisplayXR plumbing for this — engines already embed an icon resource into the PE during build. Authors should configure both sides from a single source asset:

- **Unity** — set the icon in *Project Settings → Player → Icon* (standalone platform). The DisplayXR Unity plugin's manifest settings asset has a separate `icon` field that should reference the **same source texture**. Mismatch produces an editor warning.
- **Unreal** — set the icon in *Project Settings → Platforms → Windows → Game Icon*. The DisplayXR Unreal plugin's manifest settings has a separate `icon` field that should reference the **same source asset**. Mismatch produces an editor warning.
- **Native apps** — embed an `.ico` resource in your `.rc`/manifest the way you normally would, and point `icon` in the manifest at the corresponding `.png` next to your manifest.

A workspace controller does NOT extract icons from the PE for sidecar/registered manifests — it always reads the path in `icon`. PE-icon-extraction is only used by an **interactive browse-and-register flow** when the user adds an arbitrary executable that does not already ship a manifest. The DisplayXR Shell implements this flow as **Browse for app…** (see §11); other workspace controllers may implement equivalent flows differently or omit the feature entirely.

## 10. Versioning

Breaking changes bump `schema_version`. A workspace controller will refuse to parse manifests with a `schema_version` it does not understand, and log the unsupported version. Additive changes (new optional fields) keep `schema_version: 1`. The `exe_path` field added in this revision is additive — older controllers that read a registered manifest will fall back to sidecar resolution (look for sibling exe, fail, skip the entry); they will never crash. Registered-mode manifests therefore require a workspace controller ≥ the version that introduced this field.

## 11. Browse-for-app and registered-apps state cache (DisplayXR Shell reference implementation)

This section describes the **DisplayXR Shell's** implementation of optional per-controller features that other workspace controllers may implement, omit, or replace. The manifest format itself is fixed by the spec; the state-cache filename, location, and Browse-for-app flow are *not* — they are reference-implementation choices.

`registered_apps.json` at `%LOCALAPPDATA%\DisplayXR\registered_apps.json` is the DisplayXR Shell's **state cache** — saved window poses, MRU order, hide flags, and other per-tile UI state. It is **not** the source of truth for which apps exist; that role belongs entirely to manifests under §5's discovery dirs. Other workspace controllers may use a different cache filename or skip persistence entirely.

The DisplayXR Shell's launcher exposes a **Browse for app…** entry that registers an arbitrary executable by **writing a manifest** into `%LOCALAPPDATA%\DisplayXR\apps\`:

1. The user picks an `.exe`.
2. The shell extracts the embedded PE icon to `<sanitized_basename>.png` next to the new manifest. If extraction fails the manifest omits `icon`.
3. The shell writes `<sanitized_basename>.displayxr.json` with `schema_version:1`, `name` derived from the exe basename, `type:"3d"`, `category:"app"`, `exe_path` set to the picked path, and `icon` if extraction succeeded.
4. The scanner re-runs and the new manifest is picked up like any other registered entry.

Because the manifest gets written into the ecosystem-standard discovery dir, **other workspace controllers will pick up the result** — Browse-for-app in the DisplayXR Shell registers the app for any compliant controller the user later runs.

The DisplayXR Shell's "remove" action on a tile **deletes the manifest file** when it lives under `%LOCALAPPDATA%\DisplayXR\apps\` (per-user, no elevation needed) or `%ProgramData%\DisplayXR\apps\` (system-wide, may fail without elevation — falls back to hiding the tile via state cache). Sidecar manifests in dev paths are never deleted by the launcher; they are developer-owned.

Developers should never edit `registered_apps.json` directly — edit the manifest and relaunch the workspace controller.
