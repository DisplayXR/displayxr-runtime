# Agent Prompt — MCP Phase A Implementation

You are picking up work on the DisplayXR MCP Phase A feature on branch `feature/mcp-phase-a-ci`, worktree `.claude/worktrees/mcp-phase-a`.

## Context you need before touching code

Read in this order:

1. `docs/roadmap/mcp-spec-v0.2.md` — full v0.2 spec. §3.1 (handle-app architecture), §4.A (Phase A tool list), §4.C (three-signals model + bug classes) are authoritative.
2. `docs/roadmap/mcp-phase-a-plan.md` — this plan. Follow the slice sequence exactly; don't merge slices.
3. `CLAUDE.md` — repo conventions. Relevant: branch naming (`-ci` suffix, already done), issue refs in commits, prefer local builds over CI, worktree rule.
4. `docs/architecture/separation-of-concerns.md` — layer boundaries. MCP server goes in `auxiliary/util/` so it sits below state tracker and above platform/OS. It must not depend on compositor-API headers directly — swapchain access goes through existing `xrt_swapchain` vtables.
5. `src/xrt/state_trackers/oxr/oxr_session_frame_end.c` — where the snapshot gets published.
6. `src/xrt/auxiliary/util/u_logging.{h,c}` — you'll be adding an opt-in ring buffer here for `tail_log`.

## Operating mode

- **Primary platform: macOS.** Iterate with `./scripts/build_macos.sh` and `cube_handle_metal_macos` / `cube_handle_gl_macos` under `sim_display`. Do not touch Windows-specific code until slice 7.
- **No CI for iteration.** Local builds only. Only use `/ci-monitor` when a slice is ready for PR.
- **One slice per commit.** Each slice in the plan is its own atomic commit. Don't squash during development; squash at PR time if needed.
- **Commit messages must include `(#XXX)`** — create or find a GitHub issue for Phase A before first commit and reference it.

## Invariants you must preserve

- **Runtime still works with `DISPLAYXR_MCP` unset.** The MCP server is strictly opt-in and has zero runtime cost when disabled. Verify after every slice.
- **No blocking on the render thread.** Tool handlers read from atomically-published snapshots; they never lock anything the compositor holds.
- **No changes to existing tool behavior for non-MCP consumers.** The U_LOG ring buffer, when added, is opt-in and does not change log output for existing readers.
- **Layer rules (`separation-of-concerns.md`) apply.** MCP tool code must not include vendor SDK headers or graphics-API headers directly. Capture goes through swapchain vtable readback.

## Slice entry/exit criteria

Follow `docs/roadmap/mcp-phase-a-plan.md` §"Delivery slices." For each slice:

1. Before starting: read the slice description, list files you will touch, confirm dependencies from previous slices are landed.
2. Implement.
3. Write or update the scripted test in `tests/mcp/`.
4. Build on macOS, run the test, capture output.
5. Verify the invariants above.
6. Commit with an issue ref.

Do not open a PR until all mac-side slices (1–6) are green.

## The "done" demo

Phase A is done when this sequence works on a clean mac build:

```bash
./scripts/build_macos.sh
DISPLAYXR_MCP=1 XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json \
    ./test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos &
APP_PID=$!
./build/src/xrt/targets/mcp_adapter/displayxr-mcp --pid $APP_PID
# then drive via a scripted JSON-RPC session that calls:
#   list_sessions → get_kooima_params → get_submitted_projection →
#   diff_projection → capture_frame → tail_log
```

A short script in `tests/mcp/demo_maya.sh` should run this and print a pass/fail summary.

## When to ask vs when to proceed

- **Proceed without asking** on: file layout inside `auxiliary/util/`, JSON-RPC framing details, which third-party JSON lib (recommend `cJSON`), snapshot struct layout, test scaffolding choices.
- **Ask first** on: anything that modifies `xrt_compositor` or `xrt_device` vtables; any new cross-cutting lock; dependencies on new third-party libraries beyond `cJSON` and `stb_image_write`; changes to public OpenXR extension headers.

## What "done" looks like at PR time

- All 7 slices committed (6 mac + 1 Windows), each with its test.
- `docs/roadmap/mcp-phase-a-status.md` authored — mirror the structure of `docs/roadmap/shell-phase7-status.md`.
- PR body links the spec, the plan, this prompt, and the Phase A issue.
- Demo script `tests/mcp/demo_maya.sh` included and passing locally on mac; the Windows slice validated on a Leia display with a recorded screenshot.
- No changes to `docs/roadmap/mcp-spec-v0.2.md` without explicit user discussion — the spec is the contract this work must satisfy, not a doc you update as implementation details change.
