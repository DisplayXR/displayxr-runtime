# Implementation prompt — Display Zones (`XR_EXT_display_zones`, ADR-027), runtime phases P1–P4

> Paste this whole file as the opening prompt of a fresh session **in the
> `openxr-3d-display` (displayxr-runtime) repo**. It is self-contained.
> The DESIGN is done, accepted, and committed — **deliverable is CODE**.
> Do not relitigate the design decisions; ADR-027 is binding.

## Mission

Implement the Display Zones model: an app composes, within its window, **N 3D
zones** (each a window-px rect with its own view-rig framing + its own
multiview projection layer), **M 2D zones** (existing
`XrCompositionLayerLocal2DEXT`, reused verbatim), and one **wish mask**
(window-space, any-res, M∈[0,1]) that the display processor realizes in
hardware. Scope = **this repo only**: state tracker (P1), D3D11+VK reference
compositors (P2), GL/Metal/D3D12 parity + advertise (P3), DP-contract appends
+ sim_display (P4). The vendor-plugin caps (P5) and the
`displayxr-demo-avatar` migration (P6) are separate sessions in their repos.

## Read first (authoritative, all in-repo)

1. `docs/adr/ADR-027-display-zones.md` — the model + every resolved decision.
2. `docs/specs/extensions/XR_EXT_display_zones.md` — the API sketch you will
   turn into the real header.
3. `docs/roadmap/display-zones.md` — per-phase scope/validation detail + the
   avatar frame-loop (your API-shape reference consumer). Update its phase
   statuses as you land things.
4. Headers: `src/external/openxr_includes/openxr/XR_EXT_local_3d_zone.h`
   (**spec v4 — type values just relocated to 1000999160-166, PR #536**),
   `XR_EXT_view_rig.h`, and `README.md` in that dir (**type-value allocation
   registry — flip the 1000999150-153 row from "reserved" to assigned when you
   author the header; it auto-syncs to displayxr-extensions**).
5. Plumbing anchors: `oxr_session.c` (rig parse ~L1195-1316; Kooima fill via
   `dxr_xrt_display3d_compute_views` ~L1731-98; raw channel ~L1647),
   `u_canvas.h` (`u_canvas_apply_to_metrics` already does the per-rect metrics
   rewrite — reuse it with the locate-chained rect),
   `oxr_session_frame_end.c` (`submit_local_2d_layer` ~L1848 as the model),
   `xrt_compositor.h` (layer union L430-512), `comp_d3d11_compositor.cpp`
   (`d3d11_effective_canvas` ~L398, implicit-mask rasterize, mask sync),
   `comp_vk_native_compositor.c` (`vk_effective_canvas` ~L3898),
   `xrt_display_processor*.h` (5 per-API vtables; zone-mask slot triple is
   D3D11-only today, slots 12-14).

## Decisions already made (binding — from ADR-027)

- `XrDisplayZoneEXT{zoneId, rect}` chains at TWO points: `XrViewLocateInfo`
  (zone-scoped locate; rig chains on the same locate; Kooima framed to the
  rect — the rect IS the canvas) and `XrCompositionLayerProjection`
  (zone-tagged submit). xrEndFrame values authoritative. No zone handles.
- **Zones mode is per-frame**: ≥1 zone-chained projection layer ⇒ canvas
  output rect ignored, sticky legacy mask ignored (not an error),
  implicit-mask-from-Local2D off. **All-or-none**: every projection layer in a
  zones frame must carry a zone chain else `XR_ERROR_VALIDATION_FAILURE`.
  Zero-zone frames = local_3d_zone v4 semantics **bit-identical** (back-compat
  is structural; no shim).
- Overlapping zones allowed: alpha-over in layer-list order (core-OpenXR
  projection-layer semantics). Local2D composites post-weave only (2D-under
  reserved).
- **Wish**: auto-derive = union of zone rects + feather (default); explicit =
  existing `XrLocal3DZoneMaskEXT` referenced via
  `XrDisplayZonesFrameEndInfoEXT.wishMask` on the frame-end chain. Hardware-
  only in zones mode (composition is geometry+alpha). Advisory: DP may
  quantize/ignore. The same mask drives the visual blend
  (`final = M·weave + (1−M)·flatten(2D-over)`) so fractional M looks right on
  binary hardware. Workspace clients: wish **inert** in v1.
- **DP handoff unchanged for weave**: compositor assembles ONE window-spanning
  super-atlas (zone z's view-v tile scaled-blitted into the super-atlas view-v
  tile at the zone rect) and calls today's `process_atlas` with canvas = full
  client window — the shipped #439 supersede path generalized to N. New DP
  surface = wish only: caps appends (`wish_fractional`, `switch_granularity`,
  `reserved[4]`) + port the zone-slot triple to vk/d3d12/gl/metal (append-only,
  asserts + struct-size bumps, `XRT_DP_HAS_SLOT` guards, **no ABI major
  bump**). Old plugins / unported variants ⇒ global `request_display_mode`
  fallback (tier-1 degenerate, fine).
- View count per zone = session view count (never per-zone). `maxZones3D` = 8.
  Extension-app classes only. `xrSetWorkspaceViewRigEXT` override applies per
  zone (rig tunables only; rects stay app-owned). Type values 1000999150-153.
  New surface is exactly: `XrDisplayZoneCapabilitiesEXT`, `XrDisplayZoneEXT`,
  `XrDisplayZonesFrameEndInfoEXT` (+ `VALIDATE_BIT`),
  `XrEventDataDisplayZoneMetricsChangedEXT`, `xrGetDisplayZoneCapabilitiesEXT`,
  `xrGetDisplayZoneRecommendedViewSizeEXT`. Requires local_3d_zone(≥v4) +
  view_rig(≥v2); reuses their types, never re-declares.

## Phases (one feature branch off main, one commit per phase)

**P1 — state tracker.** Author `XR_EXT_display_zones.h` (sketch → header;
update the registry README). Locate-chained zone rect → per-zone Kooima
(`u_canvas_apply_to_metrics` with the chained rect before the existing fill;
raw channel reports the zone rect for free). New `XRT_LAYER_ZONE_3D` +
`struct xrt_layer_zone_3d_data{proj; rect;}` in the union + one
`xrt_comp_layer_zone_3d` vtable method (`comp_base` boilerplate, 56→57).
All-or-none + zones-frame gating in `oxr_session_frame_end.c`. Extension NOT
advertised; gate everything on `DISPLAYXR_ZONES=1`.
*Validate*: temp branch of `cube_handle_d3d11_win` issuing two rect-chained
locates, asserting returned `XrView.fov` equals the same-rect reference
computed app-side via `displayxr::math` (the equivalence-by-construction
oracle). Existing run scripts + ctest unregressed.

**P2 — D3D11 + VK reference compositors.** Effective-canvas gains the zone
term; super-atlas composite loop (scaled blit per zone per view, alpha-over in
layer order, **stride invariant: slot stride = atlas_width / tile_columns at
all three coupled callsites**); N-rect auto-wish rasterize (generalize the
#491 implicit-mask rasterizer, add feather); D3D11 publishes the wish via the
existing `publish_local_zone_mask` leg; VK uses the global-mode fallback until
P4. New **permanent** test app `cube_zones_d3d11_win` (clone
`cube_handle_d3d11_win`: two 3D zones with different rigs + one Local2D strip
+ an explicit feathered wish; lint with `scripts/check_displayxr_app.py`).
*Validate*: sim_display (`SIM_DISPLAY_OUTPUT=sbs`) + the %TEMP% atlas
file-trigger capture — assert each zone's content lands at its rect in each
view tile; Local2D + output-rect + view_rig regression apps unchanged.

**P3 — GL / Metal / D3D12 parity + advertise.** Mechanical port (#439 Phase-3
playbook; D3D11+VK are reference). Flip the extension on, drop the
`DISPLAYXR_ZONES` gate, and delete the single-rect supersede special case
where the N-rect path subsumes it (keep behavior, kill the branch). Metal is
code-only (no Mac eyeball this session — say so in the PR).

**P4 — DP contract + sim_display.** Caps appends; port the zone-slot triple to
the vk/d3d12/gl/metal vtables (per-API texture-handle variance mirrors each
variant's `process_atlas`); compositors switch from fallback to per-API wish
publish. sim_display: implement the triple on its variants, honor
`wish_fractional=1`, add `SIM_DISPLAY_ZONE_GRID=WxH` /
`SIM_DISPLAY_WISH_QUANTIZE=band` knobs that tint the quantized effective state
(makes advisory quantization CI-testable); `displayxr-cli selftest` gains a
zone-caps probe (this is the CI gate binary — keep it hardware-free).

## House rules (load-bearing — violations have bitten before)

- Build ONLY via `scripts\build_windows.bat` (`build` target for iteration);
  verify success by "ALL DONE" in output, NOT exit code. Non-elevated terminal
  for `XR_RUNTIME_JSON` runs (elevated loader ignores it).
- Branch off main, never edit main; the clone is shared with concurrent
  sessions — check `git branch --show-current` before every commit.
- **Hardware-behavior gate**: P2+ changes what lands on glass. Before merging
  the PR, STOP and ask the user for an on-glass eyeball (screenshots during
  eye-tracking warmup are unreliable). PR auto-merge-on-green only after that.
- Multiview terminology: never "stereo"/"left+right eye"/"SBS" in new code,
  comments, or docs (the existing `SIM_DISPLAY_OUTPUT=sbs` env value is
  grandfathered). No "shell" in compositor identifiers (use workspace /
  container app). No vendor names in ADRs or new docs.
- Per-frame diagnostics: `U_LOG_I` only (and aux INFO is dropped from the hot
  path — absence ≠ didn't run); never per-frame `U_LOG_W`.
- DP vtable appends: end-of-struct only, update the `_Static_assert` offset
  tripwires + struct size per variant, guard calls with `XRT_DP_HAS_SLOT`.
- After rebuild, deploy to `C:\Program Files\DisplayXR\Runtime` only when you
  need installed-runtime testing; prefer `_package\run_*.bat` (dev
  `XR_RUNTIME_JSON`). Never deploy `displayxr-shell.exe` from this repo.
- Header changes auto-sync to `displayxr-extensions` on merge; nothing
  external consumes the new header yet, so no coupled-merge dance is needed.

## Non-goals (do NOT build)

P5/P6 (other repos); effective-mask readback (`get_effective_zone_state` —
reserved v2); 2D-under-3D ordering (#491 tail); workspace wish clip+merge;
IPC wish-mask transport (rides P5; the zone rect over IPC locate + the
`XRT_LAYER_ZONE_3D` serialization stubs are fine to leave TODO'd with the
in-process path asserting); Metal visual validation.

## Done-when

- `cube_zones_d3d11_win` renders two differently-rigged zones + a 2D strip
  correctly on sim_display (capture-asserted) and on the user's 3D display
  (eyeballed), with the wish switching only the zone region.
- All existing cube_* / Local2D / output-rect / view_rig behavior bit-identical
  in zero-zone frames; `displayxr-cli selftest` green; both CI workflows green.
- `docs/roadmap/display-zones.md` phase statuses updated; registry README row
  flipped to assigned; PR merged after the user's eyeball.
