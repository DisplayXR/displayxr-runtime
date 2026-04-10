# Shell Phase 5 Plan: Spatial App Launcher

**Branch:** `feature/shell-phase5`
**Tracking issue:** #43 (Spatial OS) / #44 (3D Shell)
**Depends on:** Phase 4C complete (merged to main)

## Overview

Phase 4C shipped a minimal app launcher — `Ctrl+L` launches the first registered app from `%LOCALAPPDATA%\DisplayXR\registered_apps.json`. There is no visual UI for browsing apps, no auto-discovery of installed DisplayXR apps, and no way to tell running-in-shell apps apart from not-yet-launched apps.

Phase 5 delivers the **Spatial App Launcher**: a 3D panel inside the shell that shows installed DisplayXR apps as tiles with icons, a "currently running" section, and a "quick launch" flow. The launcher is summoned by Ctrl+L (or a new hotkey), rendered as a spatial window in the multi-compositor, and supports mouse navigation + keyboard focus.

**Phase 5 has two main deliverables:**

1. **App discovery** — figure out which apps on the system are DisplayXR / OpenXR apps, without requiring the user to hand-edit JSON.
2. **Spatial launcher UI** — render an app grid in the shell window, handle input, launch selected apps.

Phase 5 starts with an **exploration phase** on app discovery before committing to an implementation.

## Part 1: App Discovery (Exploration → Design → Implementation)

### Exploration phase

Before writing code, the agent must explore and report on these questions. The Phase 4C research already produced a summary — the exploration should validate and extend it:

1. **Can we reliably detect OpenXR apps from their executable?** Options:
   - Scan PE imports for `openxr_loader.dll`
   - Scan for DisplayXR-specific imports or strings
   - Parse embedded manifests / version resources
   - None of the above (rely on user registration)

2. **Is a `.displayxr.json` sidecar convention viable?** Design:
   - File format (name, icon path, display mode requirements, category, etc.)
   - Discovery locations (next to .exe, in `%LOCALAPPDATA%\DisplayXR\apps\`, etc.)
   - Who writes it: app developer, installer, or the launcher scanner on first run
   - Fallback when no sidecar exists

3. **What existing DisplayXR install locations should we scan?**
   - `_package/bin/` (installed runtime dir)
   - `test_apps/*/build/` (dev builds — shell dev scenario)
   - `%PROGRAMFILES%\DisplayXR\apps\` (hypothetical production install)
   - `demos/*/build/` (demo repo install)

4. **How do other XR ecosystems handle this?**
   - SteamVR: `steamapps.vdf`, `appmanifest_*.acf`
   - Oculus: library JSON in `%APPDATA%\Oculus`
   - Windows Mixed Reality: AppX manifest
   - Is there a lightweight pattern we can borrow without adopting a full app store?

5. **What can the RUNTIME tell us about apps?** Even without discovery, the runtime knows:
   - Running IPC clients (name, PID, display mode index, extensions used) via `system_get_client_info`
   - Apps that have called `xrCreateInstance` in the past (does the runtime log this? should it?)

**Exploration deliverables:**
- A short report (~500 words) in `docs/roadmap/shell-phase5-discovery-findings.md`
- A **recommendation** for the discovery strategy (or a hybrid of strategies)
- A proposed `.displayxr.json` spec if that's part of the recommendation

### Design phase (after exploration approval)

Based on the exploration findings, draft the concrete design:

- App metadata schema (what fields per app)
- Discovery scan logic (when, what paths, how often)
- Cache format (update `registered_apps.json` or new format)
- Manual add/remove UI from the launcher

### Implementation tasks

Depends on the design outcome. Likely tasks:

| Task | Description |
|------|-------------|
| 5.1 | Exploration report + recommendation |
| 5.2 | Define `.displayxr.json` sidecar spec (if adopted) — add to test_apps/ and demos/ |
| 5.3 | Filesystem scanner — walk a set of paths, find executables, read sidecars / extract metadata |
| 5.4 | Icon extraction — from sidecar, from `.exe` via `PrivateExtractIconsW`, or placeholder |
| 5.5 | Merge scanned results with existing `registered_apps.json` (preserve user edits) |
| 5.6 | Running-app detection via existing `system_get_client_info` — tag entries as "running" |

## Part 2: Spatial Launcher UI

The launcher is a spatial window rendered by the multi-compositor, similar to how app windows are rendered. It appears when the user presses Ctrl+L (or whatever trigger we decide), floats in front of the user, and supports click-to-launch.

### UI layout (initial design — refine during implementation)

```
┌──────────────────────────────────────────────┐
│  DisplayXR Launcher                      [x] │
├──────────────────────────────────────────────┤
│                                              │
│  Running (2)                                 │
│  ┌────┐ ┌────┐                               │
│  │ C  │ │ V  │   (running apps — click to    │
│  │Cube│ │ VK │    bring to focus)            │
│  └────┘ └────┘                               │
│                                              │
│  Installed (5)                               │
│  ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐          │
│  │ D11│ │ D12│ │ GL │ │ VK │ │Demo│          │
│  │Cube│ │Cube│ │Cube│ │Cube│ │ GS │          │
│  └────┘ └────┘ └────┘ └────┘ └────┘          │
│                                              │
│  [Browse for app...]                         │
└──────────────────────────────────────────────┘
```

### Rendering approach

The launcher can be rendered in one of two ways:

- **Option A: Native multi-compositor UI** — extend the multi-compositor shell window chrome (which already draws title bars, borders, buttons) to draw a launcher panel directly. Uses existing `d3d11_service` drawing primitives. Fastest to implement, no new windowing.
- **Option B: Dedicated launcher HWND via runtime-owned window** — a small child window created by the service for the launcher, captured into the shell like any other window. More flexible but heavier.

**Recommendation:** Start with Option A. The shell already has title bars, text rendering via the bitmap font atlas (see `d3d11_bitmap_font.h`), and button hit-testing. A panel of tiles is the same primitives, just laid out differently.

### Interaction

| Action | Trigger |
|--------|---------|
| Open launcher | `Ctrl+L` (replacing current minimal "launch first app" behavior) |
| Close launcher | `Esc` / click outside / click [x] |
| Navigate tiles | Mouse hover + click; arrow keys + Enter |
| Launch app | Click tile or Enter |
| Browse for exe | Click "Browse for app..." → file dialog → add to registry |
| Remove from launcher | Right-click tile → "Remove" |

### Tasks

| Task | Description |
|------|-------------|
| 5.7 | Launcher panel render — quad in multi-comp at a fixed position, drawn with existing blit shader |
| 5.8 | Tile layout — grid of thumbnails, text labels from app metadata |
| 5.9 | Hit testing — mouse ray → tile index, hover highlight, click dispatch |
| 5.10 | Launch dispatch — click tile → spawn process with correct env vars (reuse Phase 4C `shell_launch_registered_app`) |
| 5.11 | Running indicator — highlight tiles whose app is currently an IPC client |
| 5.12 | Keyboard navigation — arrow keys + Enter, tab out to shell |
| 5.13 | Remove / reorder via right-click context menu |
| 5.14 | Browse-for-app file dialog integration (reuses existing Ctrl+O plumbing where applicable) |

## Out of scope for Phase 5

- Steam/Oculus-style app store
- Cloud app sync
- Icons extracted from arbitrary exe resources (use sidecar or fallback for v1)
- Custom app categories / tags beyond "running" vs "installed"
- Multi-monitor launcher positioning (single display for v1)
- Localization

## Critical files

| File | Expected changes |
|------|------------------|
| `src/xrt/targets/shell/main.c` | Ctrl+L now opens launcher panel instead of launching first app; add scanner call on startup |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Launcher panel rendering + hit testing |
| `src/xrt/compositor/d3d11_service/d3d11_bitmap_font.h` | Reused for tile labels |
| `test_apps/*/build/*.displayxr.json` | New sidecar files if convention is adopted |
| `docs/roadmap/shell-phase5-discovery-findings.md` | NEW — exploration report |
| `docs/roadmap/shell-phase5-status.md` | Task checklist, updated as work progresses |

## Verification

1. **Discovery scan** — Launch shell with no `registered_apps.json`. Verify scanner finds all `cube_handle_*` test apps in `test_apps/` and populates the registry.
2. **Launcher opens** — Ctrl+L opens the panel, shows all scanned apps + "Browse for app..." button.
3. **Running indicator** — Launch cube_handle_d3d11_win. Open launcher. Verify the cube tile shows as "running."
4. **Click to launch** — Click a tile, verify the app launches and the panel closes.
5. **Bring to front** — Click a "running" tile, verify the existing instance gets focus (not a second instance).
6. **Close via Esc** — Panel closes, shell continues.
7. **Round-trip with Ctrl+Space** — Open launcher, deactivate shell, re-activate, reopen launcher, state persists.

## Risks

- **Discovery false positives:** scanning exe imports could classify any OpenXR app as a DisplayXR app (not necessarily wrong, but could clutter the launcher). Mitigation: prefer sidecar files, use exe scan only as fallback.
- **UI complexity creep:** keyboard nav + hit testing + animations in the multi-compositor is non-trivial. Start with the simplest possible grid and iterate.
- **Icon quality:** extracting icons from exes is fiddly. Use a fallback icon for v1 if sidecar doesn't provide one.
