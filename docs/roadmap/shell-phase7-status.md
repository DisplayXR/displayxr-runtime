# Shell Phase 7 Status: 3D Icon Rendering

**Branch:** `feature/shell-phase7`
**Status:** Not started
**Date:** 2026-04-12

## Scope

Wire icon textures end-to-end: IPC icon path plumbing → D3D11 texture loading → textured tile rendering → SBS per-eye sampling for 3D icons.

**Full plan:** [shell-phase7-plan.md](shell-phase7-plan.md)

## Tasks

| Status | Task | Description |
|--------|------|-------------|
| [ ] | 7.1 | Bump IPC_BUF_SIZE, extend ipc_launcher_app with icon fields |
| [ ] | 7.2 | Per-app icon SRV storage + load on push |
| [ ] | 7.3 | Textured tile render (fallback to solid color when no icon) |
| [ ] | 7.4 | SBS per-eye UV sub-rect for icon_3d |
| [ ] | 7.5 | Placeholder test icons for cube_handle apps |

## Commits

_(none yet)_

## Design Decisions

_(to be filled in during implementation)_

## Known Issues

_(to be filled in during implementation)_
