# WebXR-in-Shell — Stage 2 Detailed Plan

**Parent:** [`webxr-in-shell-plan.md`](./webxr-in-shell-plan.md)
**Depends on:** Stages 0, 0a, 0b (all merged on `feature/webxr-in-shell`)
**Status:** Draft — approach locked, awaiting execution.

## Problem (restated against verified code state)

For Chrome WebXR in the shell, everything the plan assumed had to be
built for Stage 0 actually already exists: `xrt_window_metrics`, the
server-side per-client accessor, the server-side eye transform to
window-local space, and a Kooima-from-window-dims code path in the
state tracker. The missing link is a plumbing gap:

- The state tracker calls `oxr_session_get_window_metrics`
  (`oxr_session.c:249`).
- For D3D11 *in-process* native compositor clients it returns correct
  window metrics via `comp_d3d11_compositor_get_window_metrics`.
- For **IPC clients** (Chrome WebXR, any IPC app that doesn't own its
  own native compositor in-process) the function falls through to the
  `multi_compositor(&sess->xcn->base)` branch at `:288-291`, which is a
  wrong cast in the *client* process — an IPC client has an
  `ipc_client_compositor`, not a `multi_compositor`. The call returns
  false, and Kooima falls back to full-display dimensions.

Result: Chrome's WebXR FOV is computed for the full display, not the
shell window. Even if the blit were scaled, the perspective would still
be a headset-scale Kooima.

The second issue is that Chrome allocates a headset-scale framebuffer
(~2160 px/eye) because `recommendedImageRect` today reports full-display
per-view dims. The compositor's shell-mode blit path then crops
instead of scaling, so the visible content is the top-left of Chrome's
framebuffer inside the window tile.

## Scope

Make Chrome's legacy WebXR (and any other IPC app that runs without its
own native compositor) render correctly inside a shell window.
Explicitly out of scope:

- Bridge-aware pages (handled at Stage 3).
- Live resize while a session is active (Stage 4 — per-frame pull
  should make it Just Work, but verification is its own stage).
- Android / macOS.

## Design

### 2a. Per-client window metrics over IPC (Option 1)

Add a dedicated IPC call that returns the server's per-client
`xrt_window_metrics`. Rationale for a dedicated call over piggybacking
on an existing reply: separation of concerns, natural per-frame pull
model for Stage 4's live resize, trivial payload for a named-pipe round
trip at ~60 Hz. The IPC method pattern already exists — mirror an
existing `IPC_METHOD_*_get_*` call.

Changes:

1. **Proto / generator** — add a new IPC method that takes no args and
   fills an `xrt_window_metrics` on the reply. Follow the IPC code-gen
   layout used by existing compositor methods (the code generator is
   `scripts/generate_ipc_*.py` or similar — to be confirmed at impl
   time).
2. **Server handler** (`ipc/server/`) — call
   `comp_d3d11_service_get_client_window_metrics(xsysc, xc, &wm)` with
   the server-side `xc` for the calling IPC client. Return `wm`.
3. **Client shim** (`compositor/client/` —
   `comp_ipc_client_compositor.c` or similar) — thin wrapper that
   invokes the new IPC call and copies the reply into the caller's
   `xrt_window_metrics *`.
4. **State tracker** (`oxr_session.c:249`) — add a new branch before
   the broken `multi_compositor` fallback that routes IPC-client
   sessions through the client shim. Keep the D3D11/D3D12/Metal/GL/VK
   native-compositor branches (they already work). Remove or guard the
   multi-compositor branch so it only triggers in genuine in-process
   multi-compositor cases.

Behavior: when the server knows the client's slot rect, the client sees
window-scoped metrics; when the server doesn't (e.g. shell mode off,
before the first render), the call returns `valid=false` and the state
tracker's existing fallback to `get_display_dimensions` kicks in.

Bridge-relay sessions (`sess->is_bridge_relay`) must continue to skip
window-scoped Kooima — they forward raw DP-tracked eyes and let the
browser do its own math. Gate the new branch on
`!sess->is_bridge_relay` to preserve that contract.

### 2b. `recommendedImageRect` via scale-blit, not pre-assignment

Don't try to report a per-session `recommendedImageRect` at
`xrEnumerateViewConfigurationViews` time. The shell slot isn't assigned
until first render, so any rect we report is either a guess that's
wrong after user layout changes, or it forces a policy decision we
shouldn't bake in at session startup.

Instead, make the server's shell-mode blit path scale oversized content
to the tile instead of cropping it. Today
`comp_d3d11_service.cpp:8881` has:

```cpp
bool needs_scale = !sys->shell_mode && (src_w > tile_w || src_h > tile_h);
```

Change the gate to:

```cpp
bool needs_scale = (src_w > tile_w || src_h > tile_h);
```

with the SRGB gotcha addressed (see risks below). Chrome allocates its
preferred framebuffer, the compositor shrinks to tile on the way into
the atlas. Handle apps that already render at tile size stay on the
raw 1:1 copy path and are unaffected.

### 2c. Bridge-aware vs legacy detection

Already solved. `sess->is_bridge_relay` is set at session creation time
and drives the existing `has_external_window` / `is_bridge_relay`
branches in `oxr_session.c`. Stage 2's new IPC-metrics branch should
simply exclude bridge-relay sessions — they stay on their existing
raw-pose relay path. Stage 3 handles the bridge's own display-info
scoping separately.

## Files touched

| File | Why |
|---|---|
| `src/xrt/ipc/shared/ipc_protocol.h` (or generator input) | Declare new IPC method |
| `src/xrt/ipc/server/ipc_server_handler.c` | Handle the new method, call `comp_d3d11_service_get_client_window_metrics` |
| `src/xrt/compositor/client/comp_ipc_client_compositor.*` | Client-side shim invoking the new IPC |
| `src/xrt/state_trackers/oxr/oxr_session.c` | New branch in `oxr_session_get_window_metrics`; remove bad multi-compositor fallback |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Drop `!sys->shell_mode` from the `needs_scale` gate; verify SRGB path |

No new headers; `xrt_window_metrics` already defined in
`xrt_display_metrics.h`.

## Staging

Ship one sub-commit per sub-stage so regressions are bisectable.

1. **2a.0 — IPC method scaffolding.** Declare + generate + wire the
   no-op call returning `valid=false`. Build green, no behavior change.
2. **2a.1 — Server handler.** Return real per-client metrics from
   `comp_d3d11_service_get_client_window_metrics`. Log on first call
   per client. Still no behavior change from the client's point of
   view (state tracker doesn't call it yet).
3. **2a.2 — Client shim + state-tracker branch.** Route
   `oxr_session_get_window_metrics` through the IPC call for IPC
   clients. Gate on `!sess->is_bridge_relay`. At this point Chrome
   WebXR sees correct window dims for Kooima FOV — but the frame still
   crops instead of scaling.
4. **2b — Enable scale in shell mode.** Drop the `!sys->shell_mode`
   part of the `needs_scale` guard. Verify handle apps still hit the
   raw 1:1 path (content dims match tile). Verify WebXR now fills the
   window at the window's FOV.
5. **2c — Regression sweep + commit polish.** Shell off with WebXR,
   shell on with handle apps, shell on with two WebXR tabs, shell on
   with bridge-aware page. Everything behaves as before for (1) and
   (4), correctly for (2) and (3).

## Verification

Per sub-stage:

| Sub-stage | Test |
|---|---|
| 2a.0 | `scripts\build_windows.bat build` green. `displayxr-service.log` shows new IPC method registered. |
| 2a.1 | Chrome WebXR page → server log shows `get_window_metrics` call with real rect on each xrLocateViews. |
| 2a.2 | Chrome log / IPC client log shows received metrics. Kooima FOV in `oxr_session.c` diagnostic lines uses window dims not display dims. *Visual perspective is still wrong until 2b ships* — content is cropped. |
| 2b | Content fills the window. Perspective matches the window's size and distance. Drag window + start new VR session → new FOV. Handle app colors unchanged (SRGB check). |
| 2c | Regression matrix passes. |

Hardware: Leia SR + dev machine + Chrome with WebXR.
Test URLs:
- Legacy: `https://immersive-web.github.io/webxr-samples/immersive-vr-session.html`
- Bridge-aware: local bridge sample (check `webxr-bridge/DEVELOPER.md` for current serve command).

## Risks / gotchas

1. **SRGB blit paths** (`feedback_srgb_blit_paths` memory). Non-shell
   scale uses a shader that linearizes; 1:1 shell copies don't. When
   we let scale run in shell mode, we need the shader's gamma handling
   to match the raw-copy path — otherwise shell-mode WebXR colors
   shift when content exceeds tile size. Re-read the memory and the
   shader (`d3d11_service_shaders.h` / `.cpp` or the crop shader)
   before changing the gate.
2. **Atlas stride invariant** (`feedback_atlas_stride_invariant`).
   Tiles are always at `sys->view_width` stride in shell mode. The
   scale blit must write into the tile, not shift the stride. The
   existing non-shell scale path already respects this; shell mode
   just needs to enable it, not redesign it.
3. **IPC round trip per frame.** ~60 Hz call with a ~60-byte struct.
   Trivial on named pipes, but worth measuring under two
   simultaneously-active WebXR clients to confirm no IPC-thread
   backup. If it matters, cache on client for N frames.
4. **Chrome DPR / foveation.** Chrome may allocate a buffer at its
   own native eye size independent of `recommendedImageRect`. That's
   fine for Stage 2 — the scale blit handles any source size. First
   run: log the framebuffer size Chrome actually presents and note it
   in the service log to verify scale is running.
5. **Mid-session live resize.** Stage 4 territory. Stage 2 should
   leave the door open: the per-frame IPC pull in 2a means resize
   picks up automatically as long as the server slot rect updates
   synchronously with the drag. Verify by dragging during a test
   session in 2c — if it stutters or snaps late, it's a Stage 4
   refinement, not a Stage 2 blocker.
6. **Fallback on metrics unavailable.** If the new IPC call returns
   `valid=false` (e.g. race before first render, shell mode off), the
   state tracker must cleanly fall through to its existing
   display-dimension fallback. Keep the chain intact.

## Non-goals

- Implementing live resize events (Stage 4).
- Per-window eye tracking.
- Bridge-aware session scoping (Stage 3).
- Android / macOS.

## Follow-ups (post-Stage 2)

### Qwerty bridge-relay freeze leaks across sessions

Surfaced during 2a.2 visual test. Pre-existing bug, not introduced by Stage 2,
but Stage 2 made it visible because before 2a.2 the WebXR perspective was so
wrong you couldn't tell the head pose was also frozen.

**Symptom.** With a bridge-aware page open, *any concurrent legacy WebXR
session* (and any in-shell handle app) sees a frozen head pose — qwerty WASD/
mouse-look stops moving the viewpoint for everyone.

**Root cause.** `qwerty_device.c:34,350-370` has a global
`g_qwerty_bridge_relay_active` flag that zeroes out pose deltas when the
bridge is alive. Flipped to true from `comp_d3d11_service.cpp:9464` and
`comp_multi_compositor.c:1325`. Affects all callers of
`xrt_device_get_tracked_pose` on the qwerty xdev — the whole service.

**Why the freeze exists.** Prevents qwerty pose drift while the bridge has
the session. But the bridge itself takes the `headless_client` early-return
path in `ipc_try_get_sr_view_poses:303-316` and never queries qwerty —
so the freeze is currently "protecting" callers that don't exist while
breaking callers that do.

**Known prior art.** Exact concern flagged in
`docs/roadmap/webxr-bridge-v2-plan.md:181`:
> "Calling `qwerty_set_process_keys(false)` from the bridge's session affects
> the whole service, including any other concurrent client. That is fine for
> the single-user / single-WebXR-app scenario but would need rethinking for
> multi-app shell mode."

**Proposed fix (1/2 day, not "dedicated qwerty per app").** Two options:

1. **Per-call bypass (preferred).** Add `qwerty_get_tracked_pose_unfrozen()`.
   `ipc_try_get_sr_view_poses` calls it for non-headless callers. Bridge-
   relay session unaffected (already takes the headless early-return).
   Most surgical — one new symbol, one call-site swap.

2. **Remove the freeze entirely.** Only safe after auditing every caller of
   `qwerty_get_tracked_pose` to confirm none of them rely on the freeze
   semantics. Bigger blast radius; option 1 first.

**Bisect-safe.** One commit. Verify with the same regression matrix as 2c
plus: open a bridge-aware page **and** Chrome legacy WebXR simultaneously
in shell — both should respond to qwerty input independently.

**Not in Stage 2 scope.** Doesn't block 2b/2c. File as its own dev issue on
`DisplayXR/displayxr-runtime-pvt` after 2c lands.

### Bridge-aware page color shift in shell mode (pre-existing)

Surfaced during 2c regression sweep but **NOT introduced by Stage 2**.
Reverted d3d11_service.cpp to pre-2b state (`4ab4f98ff`) and the shift
persists — confirms the issue lives elsewhere in the multi-comp pipeline
or downstream.

**Symptom.** A bridge-aware sample's deep-navy background color
(`scene.background = THREE.Color(0x0d0d40)`, ≈ linear 0.05/0.05/0.25)
displays as a brightened indigo (~0.31/0.30/0.69) when the page is
hosted in a shell window. Same page in non-shell mode shows the correct
deep navy. The shift is consistent with **one extra gamma transform**
somewhere in the shell-mode path: linear data being re-interpreted as
gamma at one stage, then re-encoded again at display.

**Path difference between modes.**
- Non-shell (correct): `app SC → per-client atlas (raw copy) → DP weave`
- Shell (purple):     `app SC → per-client atlas (raw copy) → multi_compositor_render shader-blit → combined atlas → service_crop_atlas_for_dp → DP weave`

The extra **multi_compositor_render shader-blit step** is the most
likely culprit. Per the diagnostic in `compositor_layer_commit`,
bridge-aware in shell goes through raw copy at Stage 1
(`needs_scale=0, srgb=1`) — so the per-client atlas is gamma-encoded
correctly. The multi-comp shader (`d3d11_service_shaders.h` PSMain,
called with `convert_srgb=0.0` at `comp_d3d11_service.cpp:6961`) samples
via `cc->render.atlas_srv` (default UNORM SRV → no auto-linearize)
and writes via the combined atlas RTV (default UNORM → no auto-encode).
By inspection it should be a passthrough, but the visible result says
otherwise.

**Plausible causes to investigate:**
1. The combined-atlas RTV / SRV is actually SRGB-typed somewhere
   (auto-encode on write or auto-linearize on sample, undocumented).
2. The blit shader's `OMSetBlendState(blend_alpha)` interacts with
   pre-multiplied alpha in a way that shifts mid-tones.
3. The DP's interpretation of combined-atlas bytes differs from
   per-client-atlas bytes (e.g. SR weaver expects different gamma
   when fed via the multi-comp path).

**Not in Stage 2 scope.** File as its own dev issue. The bug has been
present whenever shell mode + bridge-aware were tested simultaneously
(no regression introduced by Stage 2). Stage 2 verification matrix
should note "bridge-aware color in shell is pre-existing" rather than
flagging it as a new bug.

**Resolved.** Addressed in commit `48dbdc6b2` (with diag-1 in
`7c0b7fc1a`). Fix takes a reinterpretation-on-read approach: shell-
mode per-client atlas storage is now `R8G8B8A8_TYPELESS` with two
parallel SRVs (UNORM and UNORM_SRGB); `multi_compositor_render`
selects the SRV based on the source swapchain's color space (tracked
in a new `atlas_holds_srgb_bytes` per-client flag). The DP now receives
linear bytes regardless of the source. The same bug was also affecting
legacy WebXR with SRGB swapchains and is fixed in the same change. See
`docs/roadmap/webxr-bridge-color-shift-plan.md` §Outcome.

### Window pose resets on session re-create

User observation during 2c regression sweep: dragging a shell window to
a new pose, exiting VR, and re-entering creates a new immersive session
that lands the window back at its **default slot pose** (forgetting
the drag). The new session's FOV correctly matches the (reset) pose —
Stage 2's per-frame metric refresh works as designed.

**Root cause.** Multi-comp slots are tied to session lifetime, not
application identity. End-session destroys the per-client compositor
and frees the slot; re-create allocates a fresh slot at the default
pose. The drag-customized pose lives on the freed slot, which is gone.

**Not in Stage 2 scope.** Belongs in the same UX bucket as Stage 4
(live resize during a session). Possible fix paths: persist last-known
pose per `application_name` (or PID, or sidecar `.displayxr.json`)
and rehydrate on slot allocation. Or hold the slot open across
intra-app session re-creates within a short grace window.

### Launcher empty-state when shell auto-spawns from IPC client

User observation: when the shell is launched **automatically by the
service** in response to an incoming IPC client (e.g. Chrome WebXR
without prior shell), Ctrl+L doesn't bring up the launcher and the
"press Ctrl+L" empty-state hint isn't shown. Same shell binary launched
via Ctrl+Space (orchestrator-managed) or CLI works fine.

**Hypothesis.** `displayxr-shell main.c` has a code path that's
conditional on `--service-managed` flag or initial app count. The
auto-spawn-from-IPC path may not pass `--service-managed`, or may
short-circuit the hotkey RegisterHotKey + empty-state UI gate.

**Not in Stage 2 scope.** ~30 min - 2 hr depending on root cause.
File as a separate dev issue.
