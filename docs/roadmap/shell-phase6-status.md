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
| [ ] | 6.1 | Mono fallback during eye-tracking warmup (#140) |
| [ ] | 6.2 | IPC rapid-poll pipe closure investigation + fix (#144) |

## Commits

_(none yet)_

## Design Decisions

_(to be filled in during implementation)_

## Known Issues

_(to be filled in during implementation)_
