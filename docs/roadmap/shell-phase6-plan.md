# Shell Phase 6 Plan: Warmup + IPC Fixes

**Branch:** `feature/shell-phase6`
**Tracking issue:** #43 (Spatial OS) / #44 (3D Shell)
**Depends on:** Phase 5 complete (merged to main)

## Overview

Phase 6 addresses two bugs discovered during Phase 5 development:

1. **#140 — Eye-tracking warmup stretch.** For 3-10 seconds after shell activation, the DP hasn't warmed up eye tracking yet. The compositor renders a stereo SBS atlas (tile_columns=2) and feeds it to the DP which, without valid eye positions, outputs the left eye stretched to fullscreen.
2. **#144 — IPC rapid-poll pipe closure.** When `ipc_call_shell_poll_launcher_click` runs every 500ms unconditionally, the IPC pipe errors out. Current workaround: gated on `g_launcher_visible`. Root cause unknown.

## Task 6.1 — Eye-tracking warmup (#140)

### Root cause

In `multi_compositor_render` (`comp_d3d11_service.cpp`):

```
T0: ensure_shell_window → DP created → request_display_mode(true)
T1: render thread starts
T2-T10: frames render with eye_pos.valid == false → fallback IPD used
         → DP receives SBS atlas + invalid eye data → outputs stretched left eye
T10+: eye_pos.valid == true → DP interlaces correctly → visual snap to 3D
```

The 3-10s gap between DP creation and eye-tracking validity is the bug window. During this window, `sync_tile_layout` sets `tile_columns=2` (stereo) unconditionally based on the device's rendering mode — it doesn't check whether the DP can actually interlace yet.

### Fix strategy: mono fallback during warmup

When `eye_pos.valid == false`, override the tile layout to mono (`tile_columns=1, tile_rows=1`) for that frame. The render produces a single full-atlas view (no SBS split). The DP in its warmup/passthrough state shows this correctly — a single view fills the display without stretching.

When `eye_pos.valid` transitions to `true`, the tile layout snaps back to stereo (`tile_columns=2`) and the DP interlaces normally. There's a one-frame "pop" from mono to stereo but that's preferable to 3-10 seconds of left-eye stretch.

### Implementation

In `multi_compositor_render`, after calling `sync_tile_layout` and before the per-slot render loop:

```cpp
// Phase 6.1: during eye-tracking warmup, force mono layout so the DP
// doesn't stretch the left eye. The DP can't interlace without valid
// eye positions, so rendering stereo SBS is pointless. Once eye
// tracking reports valid, the normal stereo layout kicks in.
bool warmup_mono = false;
if (!eye_pos.valid && sys->tile_columns > 1) {
    warmup_mono = true;
    sys->tile_columns = 1;
    sys->tile_rows = 1;
    sys->view_width = sys->display_width;
    sys->view_height = sys->display_height;
}
```

After the render + DP pass completes, restore the original tile layout:

```cpp
if (warmup_mono) {
    sync_tile_layout(sys);
}
```

This change is localized to multi_compositor_render — no API changes, no IPC changes, no DP changes.

### Edge cases

- **Launcher at ZDP**: the launcher panel render uses `sys->tile_columns` for its per-eye loop. In mono mode, it draws one copy (correct).
- **Per-slot parallax**: with tile_columns=1, `slot_pose_to_pixel_rect_for_eye` produces one rect for the mono view. Windows at z≠0 still get parallax but from a single viewpoint (acceptable during warmup).
- **DP crop**: the crop logic uses `sys->tile_columns` to compute view dimensions. In mono, crop covers the full atlas width (correct).
- **Hardware 3D state**: the `get_hardware_3d_state` DP vtable method can be used as a SECONDARY check — if the hardware reports "not yet in 3D mode", also force mono. This handles the case where eye_pos is technically valid but the display lens hasn't turned on yet.

### Verification

1. Launch shell + cube: `_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe`
2. First 3-10 seconds: should see a MONO (single-view) version of the shell content — no stretched left eye.
3. After warmup: transitions to correct stereo 3D without user interaction.
4. Ctrl+Space deactivate → re-activate → same mono → stereo transition, no stretch.

## Task 6.2 — IPC rapid-poll pipe closure (#144)

### Root cause (hypothesized)

Unknown. The workaround (gate `ipc_call_shell_poll_launcher_click` on `g_launcher_visible`) is in place and effective. The issue only manifests when the call runs every 500ms unconditionally.

### Investigation approach

1. Add tracing in `ipc_send` / `ipc_receive` to verify reads and writes are properly paired per call.
2. Check if the ipc_client_connection mutex is held across the full request/reply pair.
3. Stress-test other out-only calls (`shell_get_state`, `system_get_clients`) at the same cadence to see if the problem is poll-call-specific or general.
4. Check for pipe buffer overflow at sustained 500ms cadence with other calls also running.

### Fix

Depends on investigation findings. Possible fixes:
- Fix a missing mutex scope in the client IPC code.
- Increase IPC pipe buffer or switch to message-mode pipes.
- Accept the workaround as permanent (gated poll is actually the right design — no need to poll when launcher isn't open).

## Critical files

| File | Task | Changes |
|---|---|---|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | 6.1 | Mono fallback in multi_compositor_render when eye_pos.valid == false |
| `src/xrt/ipc/shared/ipc_message_channel.c` (if it exists) | 6.2 | Investigation: tracing in send/receive |
| `src/xrt/ipc/client/ipc_client_connection.c` | 6.2 | Investigation: mutex scope |

## Commit plan

- **6.1**: One commit: "Shell 6: mono fallback during eye-tracking warmup (#140)"
- **6.2**: One or more commits depending on investigation: "Shell 6: fix IPC rapid-poll pipe closure (#144)" or "Shell 6: document IPC poll workaround as permanent (#144)"
