# Display Zones, standalone forced-IPC tail (#551)

> Paste this whole file as the opening prompt of a fresh session in THIS repo
> (`openxr-3d-display`). **Branch off origin/main** — NOT the branch this
> clone has checked out (it may be parked on another session's WIP; this
> prompt file is untracked, so it sits in the working tree regardless).
> Recipe: `git fetch origin`, then work in a worktree created FROM
> origin/main (`git worktree add ../dxr-wt-551 -b feat/zones-standalone-551
> origin/main`; junction `vcpkg` from the main clone into the worktree:
> `cmd /c mklink /J <wt>\vcpkg C:\Users\SPARKS~1\Documents\GitHub\openxr-3d-display\vcpkg`;
> on removal, `rmdir` the junction BEFORE `git worktree remove`). Never
> switch this clone's branch. Untracked working file — delete when the work
> lands.
> Read first: issue #551 (carries the condensed evidence), PR #552 (the
> shipped composite this builds on), `docs/adr/ADR-027-display-zones.md`.

## Context

#549 shipped (PR #552, merged 2026-06-12): the D3D11 service compositor now
composites zone-3D + Local-2D layers into the per-client atlas, and the
**workspace leg is fully validated** (avatar under the shell, Leia eyeball
passed). The composite is correct on BOTH paths — proven by a DP-input dump
on the standalone path showing the Local-2D banner replicated into both view
tiles and zone view-0 content perfectly placed.

What remains (#551) is the **standalone forced-IPC leg** (`XRT_FORCE_MODE=ipc`,
no workspace), two coupled gaps:

### Gap 1 — zone-scoped view-rig locate returns no eyes without workspace metrics

Hard evidence (2026-06-12, `cube_zones_d3d11_win`, panel already in 3D):

- Under the shell, the app log shows:
  `VIEW-RIG IPC client: locate ok, rig_applied=1 raw_valid=1 eyes=2` → zones
  weave correctly in both eyes.
- Direct forced-IPC, same app, same build:
  `VIEW-RIG IPC client: locate ok, rig_applied=0 raw_valid=0 eyes=0`,
  `3D-GATE[n]: got_eyes=0 valid=0 count=0 have_view_state=0`, plus one
  `XR_ERROR_POSE_INVALID ... views[0]->pose.orientation == {0,0,0,0}` at
  startup, and service-side
  `ipc_try_get_sr_view_poses: pose still identity after input` with
  `pose[0]==pose[1]` (FOV H=8.8° V=6.0° — degenerate).
- Effect: the app's zone-scoped `xrLocateViews` yields no usable per-eye
  poses → `cube_zones` submits zone layers the service receives with
  `view_count=1` (its `submitViewCounts[zi]` = min(viewCountOutput,
  tileCount), `test_apps/cube_zones_d3d11_win/main.cpp:986-1001,1116`) →
  zone regions weave with a black view-1 eye. **The 2D Local-2D layer is
  unaffected** (no locate dependency) — user-confirmed on glass: "the 2d
  layer renders fine, both 3D layers have black right eye".

Code anchors (verify line refs on current main):
- `src/xrt/ipc/server/ipc_server_handler.c` ~:343 — the P5 zone-scoped
  locate block: gated on `have_wm && rig != NULL && rig->zone_valid` — the
  **window-metrics resolution (`have_wm`) and/or rig application is the part
  that only works under workspace**. There is also a `@todo` at ~:862
  ("rig->zone_* is not applied on this" path) — likely the actual missing
  leg.
- Client side: `src/xrt/state_trackers/oxr/oxr_session.c` zone-locate chain
  (~:1525) + `oxr_session_locate_views` (3D-GATE / VIEW-RIG log lines).
- The standalone client DOES have a real HWND (`c->app_hwnd` /
  `c->render.hwnd` in `comp_d3d11_service.cpp`) and its own per-client DP
  with live eye positions (`xrt_display_processor_d3d11_get_predicted_eye_positions`)
  — the raw materials exist; the rig/metrics plumbing just isn't wired for
  the non-workspace path. Compare how `ipc_handle_compositor_get_window_metrics`
  resolves under workspace vs not.

### Gap 2 — wish-over-IPC publish (per-zone Tier-2 weave)

The zone wish mask is not serialized/published to the DP on the service
path. #549 shipped a **tier-1 stopgap** in `compositor_layer_commit`
(`comp_d3d11_service.cpp`, search "tier-1 wish fallback"): a zones frame on
the standalone path requests the global 3D mode once (cooldown-stamped so
the vendor 3D-state poll doesn't counter-correct — keep that pattern). For
per-zone 2D/3D weave (Tier 2) the mask must reach the DP:
- In-process reference: `comp_d3d11_compositor.cpp` `d3d11_update_zone_wish_mask`
  (rasterizes the zone-rect union with an 8×2 px feathered ring) →
  `publish_local_zone_mask` on the DP.
- The service's per-client DP handle is the same `xrt_display_processor_d3d11`
  vtable — check whether the zone-mask publish entry is reachable there
  (P4/P5 added the DP contract; if the vtable entry exists, this may be
  service-side-only; if not, a leia-plugin PR is needed → coupled-release
  rules apply).
- Workspace clients stay INERT per ADR-027 v1 — both gaps are
  **standalone-only**; do not touch workspace mode behavior.

Suggested order: Gap 1 first (it makes zones visually correct in both eyes
— user-visible), then Gap 2 (replaces the tier-1 global flip with honest
per-zone weave; if it turns out to need plugin work, scope-split AGAIN and
ship Gap 1 alone).

## Validation

- Consumer: `test_apps/cube_zones_d3d11_win` under `XRT_FORCE_MODE=ipc`
  (service running, NO shell). Wrapper exists in spirit at
  `_package/run_cube_zones_ipc_549.bat` of the old worktree — recreate: set
  `XRT_FORCE_MODE=ipc`, run the exe, NO `XR_RUNTIME_JSON` (registry finds
  the installed runtime), and **explorer-launch the .bat** (elevated harness
  breaks IPC handle duplication AND the loader ignores XR_RUNTIME_JSON when
  elevated).
- Expected after Gap 1: both eyes show both zones (red + blue with cubes) +
  the orange banner; app log shows `rig_applied=1 ... eyes=2`; service-side
  zones breadcrumb shows 6 blits (2 zones × 2 views + Local-2D × 2).
- Expected after Gap 2: per-zone weave — un-zoned canvas regions render 2D
  (Tier 2) instead of the whole window weaving 3D; the tier-1 global-mode
  request can then be demoted/removed for DPs that accept the mask.
- Regression: avatar under the shell must stay perfect (workspace leg
  untouched); plain `cube_handle_d3d11_win` forced-IPC still works
  (zero-copy path).
- **Leia eyeball before merge** (hardware-behavior change). PR + auto-merge
  on green AFTER the eyeball.

## Debug arsenal that cracked #549 (reuse instead of rediscovering)

- Deploy loop: `scripts\build_windows.bat build` in the worktree (verify by
  grepping "ALL DONE" — exit code lies), taskkill `displayxr-service.exe`,
  copy `_package/bin/{displayxr-service.exe,DisplayXRClient.dll}` to
  `C:\Program Files\DisplayXR\Runtime` (NEVER the plugins dir or any shell
  exe), restart non-elevated via
  `explorer.exe "C:\Program Files\DisplayXR\Runtime\displayxr-service.exe"`.
- One-shot texture dumps (staging copy + `stbi_write_png`, available in
  `comp_d3d11_service.cpp`'s TU): dump the per-client atlas (writer side),
  the DP-input crop, or the combined atlas post-draw to bisect writer vs
  reader. Remove before merge.
- Solid-magenta probe: set `cb->convert_srgb = 2.0f` + `src_rect.xyz` on a
  suspect blit to split "draw discarded" from "sampled black".
- Service logs: `%LOCALAPPDATA%\DisplayXR\DisplayXR_displayxr-service.exe.*.log`
  (grep `ZONES SVC`, `ZONES IPC`, `VIEW-RIG`, `3D-GATE`); app logs same dir.
- Workspace atlas capture (`%TEMP%\workspace_screenshot_trigger`) is
  workspace-only; for standalone use desktop `CopyFromScreen` via PowerShell
  + the on-glass eyeball (weave correctness needs the human eye).

## Hardware-behavior gotchas inherited from #549 (don't re-learn)

- Any commit-thread helper that binds a texture as RTV on the shared
  immediate context must UNBIND it before returning (render thread binds the
  same texture as SRV BEFORE its own RTV bind → D3D11 silently drops the
  SRV → first tile of the pass samples black).
- Commit-thread shader work must hold `sys->render_mutex`; acquire
  cross-process sources all-or-nothing BEFORE any clear.
- Stride invariant everywhere: `atlas_width / tile_columns` at write, clamp,
  AND read (`service_crop_atlas_for_dp` was the read-side violator).
- Standalone immediate mode flips must stamp
  `last_flip_landed_ns`/`cached_3d_state` or the 100 ms vendor poll
  counter-corrects every frame.
- SR camera tracking flaps when nobody is in front of the panel
  (`Eye tracking state changed: isTracking=0/1`) — don't chase it as a bug.

## House rules

Multiview terminology (tile/view/atlas — never stereo/SBS); `workspace` not
`shell` in compositor identifiers/logs; one-shot WARNs only; premultiplied
alpha everywhere; wait for the user's local test + Leia eyeball before
merging; evidence before root-cause theories — dump pixels, don't speculate.
