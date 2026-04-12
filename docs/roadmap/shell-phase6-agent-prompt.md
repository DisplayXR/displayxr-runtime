# Shell Phase 6: Agent Prompt — Warmup + IPC Fixes

Use this prompt to start a new Claude Code session for implementing Phase 6 on branch `feature/shell-phase6`.

---

## Prompt

```
I'm working on the DisplayXR shell — a spatial window manager for 3D lenticular displays. Phase 6 fixes two bugs discovered during Phase 5 development.

## Context

Read these docs first (in order):
1. `CLAUDE.md` — project overview, build commands, architecture
2. `docs/roadmap/shell-phase6-plan.md` — **the plan you're implementing**
3. `docs/roadmap/shell-phase6-status.md` — task checklist (update as you complete tasks)
4. `docs/roadmap/shell-phase5-status.md` — what Phase 5 delivered (the foundation)
5. GitHub issue #140 — eye-tracking warmup bug
6. GitHub issue #144 — IPC rapid-poll bug

## Branch

You are on `feature/shell-phase6`, branched from `main` after Phase 5 was merged. All work goes here. Commits should reference #43 (Spatial OS tracking issue).

## Task 6.1 — Eye-tracking warmup (#140) — START HERE

### The bug

For 3-10 seconds after shell activation, the shell compositor renders the LEFT EYE STRETCHED TO FULLSCREEN instead of the correct stereo interlaced output. Once eye tracking warms up, the output snaps to correct 3D.

### Root cause

The display processor (DP) is created and asked to switch to 3D mode immediately on shell activation. But the DP's eye tracking takes 3-10 seconds to produce valid positions. During that window:

1. `sync_tile_layout` sets `tile_columns=2` (stereo SBS) because the device's rendering mode says "stereo"
2. `xrt_display_processor_d3d11_get_predicted_eye_positions` returns `eye_pos.valid == false`
3. Fallback eye positions are used: `(-0.032, 0, 0.6)` and `(0.032, 0, 0.6)`
4. The atlas is rendered as SBS with two views
5. The DP's `process_atlas` receives the SBS atlas + invalid eye data
6. The DP can't interlace properly → outputs left eye stretched to fullscreen

### Fix: mono fallback during warmup

When `eye_pos.valid == false`, temporarily override `sys->tile_columns=1` so the render produces a MONO atlas (single full-width view). The DP in its warmup/passthrough state shows this correctly — one view fills the display without stretching.

When `eye_pos.valid` transitions to `true`, restore the stereo tile layout and let the DP interlace normally. The visual transition is a one-frame "pop" from mono to stereo — far better than 3-10s of stretched left eye.

### Key code location

`src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp`, function `multi_compositor_render`:

- Line ~5785: `eye_pos` obtained from DP
- Line ~5789: fallback when `eye_pos.valid == false`
- Line ~5782: `sync_tile_layout(sys)` — sets tile_columns from device rendering mode

Insert the mono fallback AFTER sync_tile_layout and the eye_pos query, BEFORE the per-slot render loop.

### Edge cases to handle

- The launcher panel render uses `sys->tile_columns` — mono mode means one panel copy (correct)
- Per-slot parallax uses tile_columns — mono means one rect per app window (correct)
- DP crop logic uses tile_columns for view dimensions — mono means full-width crop (correct)
- `get_hardware_3d_state` DP vtable method can serve as a SECONDARY warmup check — if hardware isn't physically in 3D mode yet, also force mono

### Verification

1. Launch shell + cube. First 3-10s should show MONO (single-view, clean) instead of stretched left eye.
2. After warmup: transitions to correct stereo 3D.
3. Ctrl+Space deactivate → re-activate → same mono → stereo transition.
4. Regular (non-shell) standalone apps should be UNAFFECTED by this change.

## Task 6.2 — IPC rapid-poll pipe closure (#144)

Lower priority. The workaround (gate `ipc_call_shell_poll_launcher_click` on `g_launcher_visible`) is already in place. Investigate root cause:

1. Add tracing in `ipc_send` / `ipc_receive` to see if reads/writes are properly paired.
2. Stress-test other out-only calls at 500ms cadence.
3. Check mutex scope in `ipc_client_connection.c`.
4. If root cause found, fix it. If not, document the workaround as permanent.

## Commit style

- Commit per task
- Reference #43 in every commit
- Use `Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>` in the commit footer

## Testing

Build locally: `scripts\build_windows.bat build`. Launch the shell with a cube app:
```
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

For automated smoke testing: `bash scripts/test/shell_launcher_smoke.sh`

## When to ask the user

- Before making UI layout decisions that could be "bikeshed"
- When you hit any deadlock, cross-process issue, or scope question
- After completing each task — wait for user to verify on live display before committing
```
