# Shell Phase 6 Status: Warmup + IPC Fixes

**Branch:** `feature/shell-phase6`
**Status:** In progress
**Date:** 2026-04-11

## Scope

Two bugs discovered during Phase 5 development:

1. **#140** — Eye-tracking warmup shows left-eye-stretched for 3-10s after shell activation.
2. **#144** — Rapid out-only IPC poll causes pipe-closed errors (workaround in place).

**Full plan:** [shell-phase6-plan.md](shell-phase6-plan.md)

## Tasks

| Status | Task | Description |
|--------|------|-------------|
| [x] | 6.1 | Fix stretched-left-eye artifact on shell startup (#140) |
| [ ] | 6.2 | IPC rapid-poll pipe closure investigation + fix (#144) |

## Commits

- `dfd23c8c6` Shell 6: fix stretched-left-eye artifact on shell startup (#140)
- `91e9c3c2d` Docs: add Phase 6 plan, status, and agent prompt

## Design Decisions

- **6.1 Root cause**: NOT eye-tracking warmup — the app's HWND appearing on the 3D display disrupts the SR SDK's weaver initialization. Confirmed by isolating: no stretch with empty shell, no stretch when launching from launcher (DP already stable), stretch only when app launches at startup.
- **6.1 Fix**: `STARTF_USESHOWWINDOW + SW_HIDE` on app launch in shell mode. App window is never needed (content via shared handles into multi-comp atlas). Hot-switch restores via `ShowWindow(SW_SHOW)`.
- **6.1 Bonus**: skip per-client DP in shell mode (multi-comp owns shared DP), remove `request_display_mode(true)` from init paths (avoids SR SDK recalibration), add empty-shell hint text.

## Known Issues

_(none)_
