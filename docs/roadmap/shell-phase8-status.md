# Shell Phase 8 Status: 3D Capture MVP

**Branch:** `feature/shell-phase8`
**Status:** Not started
**Date:** 2026-04-13

## Scope

Deliver the MVP of the 3D capture pipeline: `Ctrl+Shift+3` in the shell captures the pre-weave L/R stereo pair to disk with a metadata sidecar. Promotes the Phase 7 file-trigger screenshot to a shell-owned, IPC-driven feature with L/R separation and metadata.

**Full plan:** [shell-phase8-plan.md](shell-phase8-plan.md)
**Full spec:** [3d-capture.md](3d-capture.md)

## Tasks

| Status | Task | Description |
|--------|------|-------------|
| [ ] | 8.1 | IPC protocol: add capture flags, `ipc_capture_result` struct, `shell_capture_frame` call |
| [ ] | 8.2 | Service: refactor screenshot into `capture_frame_impl` with L/R/SBS outputs |
| [ ] | 8.3 | L/R sub-image extraction from combined atlas staging texture |
| [ ] | 8.4 | Shell: `Ctrl+Shift+3` hotkey, filename policy, IPC call, JSON sidecar |
| [ ] | 8.5 | Capture flash indicator (optional) |
| [ ] | 8.6 | Replace Phase 7 file-trigger screenshot with call through same code path |

## Commits

_(none yet)_

## Design Decisions

_(to be filled in during implementation)_

## Known Issues

_(to be filled in during implementation)_
