# DisplayXR App Manifest (`.displayxr.json`)

| Field | Value |
|---|---|
| **Spec** | DisplayXR App Manifest |
| **Version** | 1.0 (draft) |
| **Status** | Phase 5 — spatial launcher discovery |
| **Owner** | Shell team |
| **Applies to** | Any application that wants to appear in the DisplayXR shell launcher |

## 1. Purpose

The DisplayXR shell's spatial launcher discovers installable apps by scanning a small set of filesystem locations and reading per-app **manifest sidecar files** named `.displayxr.json`. The manifest is the authoritative source of an app's display name, icons, category, and display-mode preferences.

**Discovery is manifest-gated.** An executable with no manifest is NOT shown in the launcher, even if it imports `openxr_loader.dll`. This is intentional: it keeps the launcher curated, avoids false positives from unrelated OpenXR apps, and gives developers a single explicit contract to satisfy. Users can still manually add any executable through the launcher's **Browse for app…** entry; those entries bypass the manifest requirement.

DisplayXR Unity and Unreal plugins are expected to generate a manifest automatically as part of their build/export pipelines. Native app developers author the manifest by hand.

**Not every executable in a DisplayXR SDK install ships a manifest.** In particular, test-only variants (`cube_hosted_legacy_*`, `cube_texture_*`, `cube_hosted_*`, and similar) intentionally omit a sidecar so they do not appear in the launcher — they exist to exercise runtime paths that are not meaningful to end users. Only apps that a user would plausibly launch from the shell should ship a manifest. Inside this repo, only the `cube_handle_*_win` reference apps carry sidecars.

**The shell ships no built-in default apps.** When `registered_apps.json` does not exist (first run), the registry starts empty. The scanner immediately populates whatever it finds via sidecars; if it finds nothing, the launcher renders the empty-state hint instead of a tile grid. There is no pre-seeded "Notepad" or other system app — the launcher is exclusively a curated DisplayXR app surface, never a generic Windows app drawer.

## 2. File location

The manifest lives **next to the executable** it describes, with the filename stem matching the executable:

```
my_app/
├── my_app.exe
├── my_app.displayxr.json     ← manifest
├── icon.png                  ← referenced from manifest
└── icon_sbs.png              ← optional 3D icon
```

- The scanner looks for `<exe_basename>.displayxr.json` in the same directory as each discovered `.exe`.
- All paths inside the manifest are resolved **relative to the manifest file**, not the CWD.
- The scanner never writes to the manifest. It is developer-owned.

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
| `type` | string | `"3d"` for extension/hosted apps that create an OpenXR session, `"2d"` for legacy Win32 apps the shell captures by HWND. Must match one of these values. |

### 3.2 Optional fields

| Field | Type | Default | Notes |
|---|---|---|---|
| `icon` | string | *none* | Relative path to a 2D icon image. PNG or JPEG. Recommended 512×512. When absent, the tile is rendered with the app name as a text label on a category-colored background. |
| `icon_3d` | string | *none* | Relative path to a stereoscopic icon image. When present, the shell renders the tile stereoscopically. Resolution matches `icon` aspect but doubled along the layout axis (e.g. 1024×512 for `sbs-lr`). Requires `icon` to also be set (as the 2D fallback). |
| `icon_3d_layout` | string | `"sbs-lr"` | How the stereo pair is packed in `icon_3d`. One of `"sbs-lr"` (left-right), `"sbs-rl"` (right-left), `"tb"` (top-bottom, left eye on top), `"bt"` (bottom-top). Ignored if `icon_3d` is absent. |
| `category` | string | `"app"` | Free-form tag used for grouping. Reserved values: `"test"`, `"demo"`, `"app"`, `"tool"`. Unknown values are accepted and displayed as-is. |
| `display_mode` | string | `"auto"` | Preferred display mode at launch. `"auto"` lets the runtime choose. Other values are forwarded to the runtime's mode selection. |
| `description` | string | `""` | One-line description shown in tooltips. Max 256 characters. |

### 3.3 Reserved for future versions

Names the shell may consume in later schema versions — do not use for custom data:

`version`, `publisher`, `homepage`, `min_runtime`, `required_extensions`, `screenshots`, `trailer`, `pose`, `window_size`.

## 4. 3D icons

The launcher's key visual hook is that app tiles are themselves 3D. A high-quality `icon_3d` is the single biggest contributor to the launcher "wow" moment.

### 4.1 Generating 3D icons

- **Unity / Unreal plugin** — the recommended path. Render two viewpoints from the runtime's stereo camera pair at a fixed convergence plane (typically 0.5–1.0 m), composited as specified by `icon_3d_layout`. A single gameplay snapshot is sufficient; no animation.
- **Native apps** — render two offset views with asymmetric frustums matching the target convergence, save as side-by-side PNG.
- **No 3D icon** — omit `icon_3d`. The tile falls back to `icon` rendered as a flat quad. The launcher still looks good, but without the 3D payoff.

### 4.2 Convergence guidance

The shell renders launcher tiles at ~40 cm in front of the viewer. Stereo icons should be authored for a comfortable convergence at roughly that distance. Parallax budget: ±2% of image width.

## 5. Discovery behavior

The shell scanner walks a fixed set of paths (see `shell-phase5-plan.md` Part 1):

```
<shell-exe>/../test_apps/*/build/
<shell-exe>/../demos/*/build/
<shell-exe>/../_package/bin/
%PROGRAMFILES%\DisplayXR\apps\
```

For each `.exe` found:

1. Look for `<exe_basename>.displayxr.json` in the same directory.
2. If absent → **skip the exe entirely**.
3. If present → parse, validate against this spec, resolve icon paths, add to the registry with `source: "scan"`.
4. Sanity check: verify the exe imports `openxr_loader.dll` (PE import scan). If not, log a warning and still add the entry (the manifest is authoritative — a 2D `type` app wouldn't import OpenXR anyway).

Manifest parse errors are logged to the shell log and the entry is dropped. Malformed manifests do not crash the scanner.

## 6. Validation rules

The scanner rejects a manifest if any of the following are true:

- `schema_version` is missing or not `1`.
- `name` is missing, empty, or longer than 64 characters.
- `type` is missing or not one of `"3d"` / `"2d"`.
- `icon` is specified but the referenced file does not exist or is not a readable PNG/JPEG.
- `icon_3d` is specified but the file does not exist or is not a readable PNG/JPEG, or `icon` is not also set.
- `icon_3d_layout` is specified but not one of the four allowed values.

Rejected manifests are logged as warnings. The shell does not attempt to recover partial data.

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

## 9. Versioning

Breaking changes bump `schema_version`. The shell will refuse to parse manifests with a `schema_version` it does not understand, and log the unsupported version. Additive changes (new optional fields) keep `schema_version: 1`.

## 10. Relationship to `registered_apps.json`

`registered_apps.json` at `%LOCALAPPDATA%\DisplayXR\registered_apps.json` is the shell's local registry — the cached result of scanning plus any user-added entries. Entries discovered from sidecars are written there with `source: "scan"`; entries added via **Browse for app…** get `source: "user"`. The registry is rebuilt from sidecars on each shell startup; `source: "user"` entries are preserved across rebuilds.

Developers should never edit `registered_apps.json` directly — edit the sidecar next to your app and relaunch the shell.
