# Phase 2.K — Controller-owned interactive layouts

**Status:** Draft, 2026-04-29.

## Problem

Phase 2.G moved layout-preset semantics (grid, immersive, carousel) out of the runtime into the workspace controller. The static layouts ported cleanly, but the **interactive carousel** (drag-to-rotate, scroll-radius, TAB-snap-to-front, momentum, auto-rotation) did not — its replication requires per-frame cursor motion, which Phase 2.D deliberately excluded from the public input drain (`xrEnumerateWorkspaceInputEventsEXT`):

> Per-frame mouse-move events are NOT emitted; controllers wanting hover feedback poll xrWorkspaceHitTestEXT directly.
> — `XR_EXT_spatial_workspace.h` line 304-305

Phase 2.D was right at the time (no controller had asked for per-frame motion). Phase 2.G's `controllers own all motion logic` direction makes the exclusion the wrong choice: the shell can't replicate the runtime's old carousel without WM_MOUSEMOVE, and Phase 2.C (chrome rendering) will need hover for the same reason.

## Goal

Extend the public surface so a workspace controller can fully implement arbitrary interactive layouts — drag, momentum, animation, hover, edge-resize, **smooth transitions between presets** — without the runtime knowing what behaviour the controller is implementing. Performance must support smooth (60 Hz) interaction with no perceptible lag.

Two distinct deliverables in one phase, sharing one shell-side animation framework:

**(I) Smooth transitions between layout presets** — when the user presses Ctrl+1→Ctrl+2 today, windows snap from grid to immersive. The pre-Phase-2.G runtime called `slot_animate_to` (300 ms ease) so each window glided to its target. Phase 2.K replicates this in the shell: when the controller computes new target poses, it interpolates from current pose to target over a duration of its choice and pushes `xrSetWorkspaceClientWindowPoseEXT` every tick until the animation completes. **No public-surface change needed** — set_pose already exists.

**(II) Interactive carousel** — drag-to-rotate, scroll-radius, TAB-snap, momentum. Needs per-frame `WM_MOUSEMOVE` on the public input drain (this is the surface change). Same animation tick consumes motion events and re-pushes poses.

The first half can ship without the second; both reuse the same controller-side animation framework.

## Design

### New event variant

Add `XR_WORKSPACE_INPUT_EVENT_POINTER_MOTION_EXT` (or extend the existing POINTER variant with an `isMotion` flag — TBD during implementation). The event carries:

```c
struct {
    XrWorkspaceClientId     hitClientId;     // hit-test result at drain time
    XrWorkspaceHitRegionEXT hitRegion;
    XrVector2f              localUV;
    int32_t                 cursorX;
    int32_t                 cursorY;
    uint32_t                buttonMask;      // 0x1 L, 0x2 R, 0x4 M (currently held)
    uint32_t                modifiers;
} pointerMotion;
```

The hit-test fields are filled at drain time (service side) so the controller does not have to call `xrWorkspaceHitTestEXT` per frame. Same enrichment pattern Phase 2.D set up for the down/up variant.

### Delivery gate

Two design candidates; pick during implementation after measuring:

| Option | When motion events are emitted | Pros | Cons |
|---|---|---|---|
| **(a) Always, with capture flag** | Always emitted; client opts in via `xrEnableWorkspacePointerCaptureEXT(button)` to receive them. Idle motion still emits. | Simple semantics. Lets controllers track hover for chrome highlighting without holding a button. | Higher per-frame IPC traffic (~60–120 events/s during cursor movement). |
| **(b) Button-held only** | Emit MOTION only while any mouse button is held. Idle hover bypasses the public ring. | Small per-second event count. Matches the "interactive drag" use case directly. | Hover-driven chrome highlighting still has to poll hit-test. |

Recommend **(a)** with a future opt-out flag if perf shows pressure. The hit-test enrichment is the only hot path; modulating its rate is a separate knob.

### Performance budget

- Drain RPC overhead: ~10–20 µs per call (measured for the existing input drain).
- Per-frame motion at 60 Hz × 4 clients × 1 enrichment hit-test (sub-µs) = sub-millisecond CPU per second.
- Set-pose RPCs from the controller responding to motion: same scale.
- Aggregate worst case (4 clients, full carousel drag): well under 1% of one core.

If real-world numbers surprise us, two escape hatches:
1. Throttle motion events server-side to a target Hz (configurable via the Enable call).
2. Add a shared-memory ring for cursor state (the WndProc writes, controller reads without IPC). Defer unless RPC overhead is genuinely visible.

### Hit-test enrichment threading

The current drain runs on the IPC server thread, which already holds the read lock for `workspace_public_ring`. The hit-test path takes `sys->render_mutex`. To enrich motion events without contending with the per-frame compositor render, either:
- Snapshot the slot poses + window rects under a short read-lock at drain time (cheap, no contention with render).
- Or move enrichment to the WndProc thread itself — push pre-enriched events into the ring. WndProc already has slot data via `workspace_raycast_hit_test`. Risk: WndProc must not block on render-mutex. Current `workspace_raycast_hit_test` does take `render_mutex`; moving the call there needs a non-blocking variant.

Pick during implementation; snapshot-at-drain is the safer first pass.

## Scope

### Runtime side

| File | Change |
|---|---|
| `src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h` | Add the MOTION event variant or extend POINTER with `isMotion`. Bump `spec_version` from 5 → 6. Document the gate semantics. |
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | Stop the `message != WM_MOUSEMOVE` skip in the public-ring push (lines ~684–711). Push every WM_MOUSEMOVE while a button mask is non-zero (option b) or always (option a) when capture is enabled. |
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | When pointer capture is enabled, call `SetCapture(workspace_hwnd)` so cursor motion outside the workspace window keeps reaching the WndProc. `ReleaseCapture` on disable. (Resolves the residual SetCapture limitation called out in `followups.md` item #1's resolution note.) |
| `src/xrt/state_trackers/oxr/oxr_workspace.c` | If using a separate MOTION variant, add the type to the dispatch / IPC enum. |
| `src/xrt/ipc/server/comp_d3d11_service_*` (or similar) | Drain enrichment: fill `hitClientId` / `hitRegion` / `localUV` per motion event. Snapshot slot poses under a short lock. |
| `src/xrt/ipc/shared/proto.json` | Bump the wire format if the event struct grows. |

### Shell (controller) side — animation framework + consumers

| File | Change |
|---|---|
| `src/xrt/targets/shell/main.c` | Animation framework: a per-client struct `{ start_pose, target_pose, start_w, target_w, start_h, target_h, start_ns, duration_ns, active }`. A tick function called every poll iteration interpolates per-active-client and pushes `xrSetWorkspaceClientWindowPoseEXT`. Ease-out cubic by default; controllers can swap the curve. Mirrors `slot_animate_to` / `slot_animate_tick` from the pre-Phase-2.G runtime, but lives controller-side now. |
| `src/xrt/targets/shell/main.c` | **Smooth preset transitions:** `shell_apply_preset` no longer calls `set_pose` directly. Instead it computes target poses, then for each client snapshots the current pose (via `xrGetWorkspaceClientWindowPoseEXT`) and seeds the animation framework with start=current / target=new / duration=300 ms. The tick drives the actual set_pose calls over the next ~18 frames. |
| `src/xrt/targets/shell/main.c` | **Variable poll cadence:** main loop drops `POLL_INTERVAL_MS` from 500 → ~16 ms while any client animation is active OR an interactive preset (carousel) is the active layout. Returns to 500 ms when idle. Keeps idle CPU low; smoothness only when needed. |
| `src/xrt/targets/shell/main.c` | **Carousel state machine:** drag-to-rotate, scroll-radius, TAB-snap-to-front, momentum. Reads MOTION + SCROLL events from the input drain. Same tick re-computes per-window carousel poses each frame and pushes via the animation framework (with duration=0 for direct drives during drag, duration=300 ms for releases that snap to a target angle). |
| `src/xrt/targets/shell/shell_openxr.{h,cpp}` | No new PFN resolution required — `xrEnableWorkspacePointerCaptureEXT` is already in the list. Usage change: enable on entry to interactive presets (carousel) so motion events flow; disable on exit. |

### Test app

`test_apps/workspace_minimal_d3d11_win/main.cpp` should drain MOTION events and assert at least one delivers the new fields populated, in the success path.

### Docs

- `docs/specs/XR_EXT_spatial_workspace.md` — promote MOTION to first-class.
- `docs/roadmap/spatial-workspace-extensions-followups.md` — resolve item #2 once 2.K ships.
- `docs/roadmap/spatial-workspace-extensions-phase2-audit.md` — mark 2.K shipped.

## Acceptance criteria

**Smooth transitions (deliverable I)**
- ✅ Pressing Ctrl+1 → Ctrl+2 (or any preset switch) glides each window from its current pose to the new preset's target over ~300 ms with an ease-out curve. No snap, no popping.
- ✅ Connect-time auto-tile (the 2.G fix that places new windows in a grid) glides the new window to its slot from the runtime's spawn position, instead of snapping.
- ✅ `xrSetWorkspaceClientWindowPoseEXT` called at ~60 Hz during animation; no failed RPCs.

**Interactive carousel (deliverable II)**
- ✅ Carousel preset (Ctrl+3 in the shell) shows windows on a 360° ring with auto-rotation at the original ~10°/s rate.
- ✅ Title-bar LMB drag in carousel mode rotates the ring; release continues with momentum.
- ✅ Mouse wheel during carousel adjusts ring radius (clamped 0.05–0.30 m or display-relative equivalent).
- ✅ TAB during carousel snaps the focused window to the front, pauses auto-rotation 5 s.
- ✅ Drag latency: cursor → window pose update visible in next compositor frame (no perceptible lag at 60 Hz).
- ✅ N=2 special case (the static-snap degenerate case from 2.G) goes away — carousel actually rotates so both windows are visible.

**Performance**
- ✅ Idle (no animation, no carousel, no drag): poll loop back to 500 ms cadence. CPU near 0.
- ✅ During animation / carousel / drag: ~16 ms cadence × 4 clients × ~10 µs / RPC ≈ 2.4 ms / sec total. Well under 1% of one core.
- ✅ `tests_pacing` and friends still green.

## Non-goals

- Adding orbital, helix, or expose layouts. The dynamic_layout_tick had stubs for these; controllers own them now if they want them.
- General-purpose gesture / multi-touch input. Single-cursor only.
- Rebuilding the runtime's `mc->dynamic_layout` state. The controller owns this state entirely; the runtime just delivers events and applies poses.

## Open questions

- (a) vs (b) gate — measure traffic before deciding.
- Whether to keep the `pointerHover` variant (region transitions only) as a cheaper alternative for chrome highlighting, alongside MOTION. Probably yes — chrome doesn't need 60 Hz.
- Whether `xrEnableWorkspacePointerCaptureEXT` should imply MOTION delivery or whether a separate flag is cleaner. Probably implied — capture is the natural "I want to track this drag" signal.
