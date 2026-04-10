# Shell Phase 5 Status: Spatial App Launcher

**Branch:** `feature/shell-phase5`
**Status:** Not started — exploration phase pending
**Date:** 2026-04-10

## Scope

Phase 5 delivers a proper spatial launcher panel in the shell, replacing the Phase 4C minimal Ctrl+L (launch-first-app) stub. Two main deliverables:

1. **App discovery** — figure out which apps on the system are DisplayXR / OpenXR apps without requiring the user to hand-edit JSON.
2. **Spatial launcher UI** — render an app grid inside the multi-compositor shell window, handle input, launch selected apps.

**Full plan:** [shell-phase5-plan.md](shell-phase5-plan.md)

## Part 1: App Discovery

### Exploration phase

| Status | Task | Description |
|--------|------|-------------|
| [ ] | 5.1 | Exploration report — `shell-phase5-discovery-findings.md` with recommendation |

### Design + implementation (after exploration approval)

| Status | Task | Description |
|--------|------|-------------|
| [ ] | 5.2 | `.displayxr.json` sidecar spec (if adopted) — add to test_apps and demos |
| [ ] | 5.3 | Filesystem scanner — walks paths, finds exes, reads sidecars |
| [ ] | 5.4 | Icon extraction — sidecar, PE resource, or fallback |
| [ ] | 5.5 | Merge scanned results with user-edited `registered_apps.json` |
| [ ] | 5.6 | Running-app detection via `system_get_client_info` |

## Part 2: Spatial Launcher UI

| Status | Task | Description |
|--------|------|-------------|
| [ ] | 5.7 | Launcher panel render — quad in multi-comp, blit shader |
| [ ] | 5.8 | Tile layout — grid with icons and labels |
| [ ] | 5.9 | Hit testing — mouse ray → tile index, hover highlight |
| [ ] | 5.10 | Launch dispatch — click → `shell_launch_registered_app` |
| [ ] | 5.11 | Running indicator — highlight tiles for running clients |
| [ ] | 5.12 | Keyboard navigation — arrow keys + Enter |
| [ ] | 5.13 | Right-click context menu (remove, reorder) |
| [ ] | 5.14 | Browse-for-app file dialog |

## Commits

_(none yet)_

## Design Decisions

_(to be filled in during exploration and implementation)_

## Known Issues

_(to be filled in during implementation)_

## Open Questions (for exploration)

- Is there a reliable way to detect OpenXR apps by scanning their PE imports?
- Should we adopt a `.displayxr.json` sidecar convention, and who writes it?
- Which filesystem paths should the scanner walk by default?
- How do we handle icons — sidecar-provided, exe-extracted, or fallback-only for v1?
- Should the launcher persist its layout across sessions (grid order, pinned apps)?
- Should right-click remove delete from `registered_apps.json` permanently, or just hide for this session?
