# Shell Phase 5 — App Discovery Findings

**Status:** Exploration deliverable (Task 5.1). Pre-implementation. Awaiting user review before Step 2.
**Date:** 2026-04-10
**Branch:** `feature/shell-phase5`

## Goal

Decide how the shell should auto-populate `registered_apps.json` so users don't hand-edit JSON. The launcher must answer: *which executables on this machine are DisplayXR / OpenXR apps, and what should I show for each?*

## Findings

### 1. PE import scan — feasible, no DisplayXR-only signal

Windows ships `Dbghelp.dll` with `ImageDirectoryEntryToDataEx`, which can read a PE's import table without `LoadLibrary`. No PE-parsing code exists in the repo today; it would be a new ~150 LOC module.

We can reliably detect **OpenXR** apps by scanning imports for `openxr_loader.dll` — every test app and demo links it.

We **cannot** detect "DisplayXR app" specifically. There is no `DisplayXRClient.dll` and no DisplayXR-only DLL/symbol. `XR_EXT_win32_window_binding` is an OpenXR extension, not a DisplayXR marker. Conclusion: PE scan tells us "is OpenXR," nothing more.

### 2. Sidecar `.displayxr.json` is the cleanest authoritative metadata source

A per-app sidecar lets developers ship name, icon, category, and preferred display mode without resource hacks. Other XR ecosystems (SteamVR `appmanifest_*.acf`, Oculus library JSON, WMR AppX) all use the same per-app-manifest pattern. Sidecar is the strongest signal "this app intends to run on DisplayXR."

**Proposed schema:**

```json
{
  "name": "Cube D3D11",
  "icon": "icon.png",
  "type": "3d",
  "category": "test",
  "display_mode": "auto"
}
```

`type` matches the existing Phase 4C `registered_app.type` field (`"3d"` / `"2d"`). `icon` is relative to the sidecar. All fields except `name` are optional.

### 3. Existing test apps already carry usable metadata in VERSIONINFO

11 of the 12 cube test apps populate `FileDescription` in `resource.rc` (e.g. *"SR Cube OpenXR Ext Test Application"*). `GetFileVersionInfoW` + `VerQueryValueW` gives us a no-touch fallback display name, so a sidecar-less app still gets a sensible label.

No `.ico` files exist anywhere in the repo. Icon extraction via `PrivateExtractIconsW` would work but is fiddly — defer to a v2. v1 tiles render text-only when no sidecar `icon` is provided.

### 4. Scan paths

Walk these (relative to the shell exe, then absolute fallbacks):

| Path | Why |
|---|---|
| `test_apps/*/build/*.exe` | Dev scenario — 12 cube variants live here |
| `demos/*/build/*.exe` | Gaussian splatting, spatial OS demos |
| `_package/bin/*.exe` | Installed runtime layout |
| `%PROGRAMFILES%\DisplayXR\apps\` | Future production install (currently empty — harmless) |

### 5. Runtime knows running clients but not exe paths

`ipc_call_system_get_clients` + `ipc_call_system_get_client_info` returns `struct ipc_app_state` with `pid`, `info.application_name`, and session flags — but **no exe path**. To match a running client to a registry entry we either:

- match by `application_name` string (simple; what Phase 4C already does), or
- resolve `pid` → exe via `QueryFullProcessImageNameW` (more robust but needs `PROCESS_QUERY_LIMITED_INFORMATION` access).

Recommend starting with name match and adding PID→path as a fallback only if collisions show up.

## Recommended hybrid strategy

1. **Sidecar `.displayxr.json` (preferred).** Authoritative metadata. Developers add it to test_apps and demos in Step 2.
2. **Filesystem scan with PE import gate.** For each `.exe` under the scan paths, check `openxr_loader.dll` import. If matched: use sidecar if present, else `VERSIONINFO.FileDescription`, else exe filename.
3. **Merge with `registered_apps.json`.** Existing user entries get `source: "user"` and are never overwritten. Scan-discovered entries get `source: "scan"`. User-removed scan entries get `hidden: true` (sticky tombstone) so the next scan doesn't re-add them.
4. **Running tag at launcher open.** Query `ipc_call_system_get_clients` and tag matching tiles. Click on a running tile = focus existing instance, not relaunch.

This avoids requiring sidecars (so existing test apps "just work") while giving developers a clean upgrade path for richer metadata.

## Open questions for the user

Please answer before Step 2 starts:

1. **Sidecar location** — next to the `.exe`, or in `%LOCALAPPDATA%\DisplayXR\app-metadata\<hash>.json`? Next-to-exe is simpler and lets the app's own installer ship it; central dir lets the shell own metadata for apps it doesn't control.
2. **v1 strictness** — require sidecars (cleaner registry, fewer false positives) or accept VERSIONINFO + generic-icon fallback (existing test apps just work, but any random OpenXR app a user installs will appear)?
3. **Scan timing** — every shell startup, or once-on-first-run plus an explicit "Refresh" button in the launcher?
4. **Right-click "Remove"** — permanent (`hidden: true` tombstone in JSON) or session-only (re-appears on next start)?
5. **Layout persistence** — should grid order / pinned apps survive across sessions, or always sort alphabetically?
6. **Scope check** — is "OpenXR app discovered, no sidecar" something we want to **show** in the launcher, or **filter out** until the developer ships a sidecar?

Once these are answered I'll proceed to Step 2 (scanner + merge implementation) per the plan in `.claude/plans/sorted-painting-stroustrup.md`.
