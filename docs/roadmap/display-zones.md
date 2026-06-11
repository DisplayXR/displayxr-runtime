# Display Zones — migration note + phased implementation plan

> Design: [ADR-027](../adr/ADR-027-display-zones.md) +
> [`XR_EXT_display_zones` spec sketch](../specs/extensions/XR_EXT_display_zones.md).
> This doc carries the two living parts: the reference-consumer migration and
> the implementation phasing. Status: **runtime phases P1–P4 SHIPPED**
> (extension advertised; D3D11 capture- + Leia-validated, VK/D3D12/GL
> code-validated, Metal sim- + eyeball-validated 2026-06-11 via the
> permanent `cube_zones_metal_macos` test app — incl. zone overlap
> alpha-over, which Metal now does natively). Remaining:
> Phase 5 (Leia plugin caps + IPC, `displayxr-leia-plugin` repo) and
> Phase 6 (avatar migration, `displayxr-demo-avatar` repo).

## Reference consumer: `displayxr-demo-avatar` migration

The avatar (native-Vulkan `_handle` app: transparent click-through 3D tiger in
the bottom 75% of its window, flat 2D speech bubble in the top 25%) is the
acceptance test for the whole feature. It ships today (v0.1.0) on **app-side
Kooima** because the #439 supersede rule makes runtime sub-canvas framing +
a 2D zone mutually exclusive; its paused `view-rig-migration` branch (renderer
Y-convention + xr_session wiring done, main.cpp not) is **scratch** — the real
consumer change is this one, after the runtime phases land.

### Setup (once, after session create)

- Enable `XR_EXT_display_zones` + `XR_EXT_local_3d_zone` + `XR_EXT_view_rig`;
  `xrGetDisplayZoneCapabilitiesEXT` → `supported`.
- Tiger zone rect = bottom 75% of the W×H client window:
  `{{0, H/4}, {W, 3H/4}}` (client px, y-down).
- `xrGetDisplayZoneRecommendedViewSizeEXT(session, &tigerRect, &viewSize)` →
  create the multiview projection swapchains at `viewSize`.
- Flat swapchain for the bubble (top-25% rect).
- **No mask object** — the auto wish (feathered bottom-75%) is exactly the
  desired hardware behavior.

### Frame loop

```c
while (running) {
    poll_events();  // XrEventDataDisplayZoneMetricsChangedEXT -> re-query + recreate swapchains

    xrWaitFrame(); xrBeginFrame();

    XrDisplayZoneEXT tigerZone = {XR_TYPE_DISPLAY_ZONE_EXT, NULL, /*zoneId*/ 1,
                                  {{0, H/4}, {W, 3*H/4}}};
    XrDisplayRigEXT  rig = {XR_TYPE_DISPLAY_RIG_EXT, NULL,
                            displayPlanePose, virtualDisplayHeight,   // ex-app-side tunables
                            ipdFactor, parallaxFactor, perspectiveFactor};

    // Zone-scoped locate: runtime Kooima framed to the bottom-75% rect.
    tigerZone.next = &rig;
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO, &tigerZone, viewConfig,
                           predictedDisplayTime, space};
    xrLocateViews(session, &li, &viewState, cap, &viewCount, views);
    // views[i].pose/fov are render-ready for THIS rect — the app-side Kooima
    // file is deleted.

    render_tiger_views(views, viewCount);  // alpha-0 background, desktop shows through
    render_bubble_2d();

    tigerZone.next = NULL;
    XrCompositionLayerProjection tiger3D = {XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        &tigerZone, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        space, viewCount, projViews /* XrView pose/fov copied in, as core requires */};
    XrCompositionLayerLocal2DEXT bubble2D = {XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT,
        NULL, layerFlags, bubbleSubImage, /*rect*/ {{0, 0}, {W, H/4}}};
    const XrCompositionLayerBaseHeader* layers[] = {(void*)&tiger3D, (void*)&bubble2D};

    XrFrameEndInfo fe = {XR_TYPE_FRAME_END_INFO, NULL /* no chain: auto wish */,
                         predictedDisplayTime, blendMode, 2, layers};
    xrEndFrame(session, &fe);
    // Runtime: weaves tiger views into the bottom-75% rect; composites bubble
    // flat top-25%; hands the DP {super-atlas, full-window canvas, auto wish =
    // feathered bottom-75% window-space}; DP maps window->panel and switches
    // only the tiger band.
}
```

What changes vs v0.1.0: the app-side Kooima is deleted; the bubble code is
unchanged (a Local2D layer either way); the supersede trap never engages
because there is no single canvas to snap. The shipped v0.1.0 app-side-Kooima
build is the **A/B oracle** — per-rect projection parity against it is the
acceptance criterion.

## Phased implementation plan

Ordering principle: each phase is independently shippable; zones mode stays
env-gated until Phase 3; **no phase touches the behavior of frames that don't
submit a zone layer** (back-compat is structural, there is no shim code).

### Phase 0 — design docs (done)

ADR-027 + spec sketch + this doc. Docs-only, direct to main.

### Phase 1 — state tracker (this repo) — **DONE**

Shipped as planned. Validation note: the temp `cube_handle_d3d11_win` branch
asserted the zone-scoped `XrView.fov` **bit-exact** (max delta 0.000000 rad)
against the app-side `displayxr-common` oracle — equivalence by construction
held literally (same math core).


- Locate-chained `XrDisplayZoneEXT` → per-zone Kooima: apply
  `u_canvas_apply_to_metrics` with the *chained* rect (it already does exactly
  the right metrics rewrite) before the existing
  `dxr_xrt_display3d_compute_views` / `camera3d` fill in `oxr_session.c`
  (~L1640–1900). The raw channel (`XrViewDisplayRawEXT.canvasRectPx /
  canvasSizeMeters`) reports the zone rect for free (it reads the rewritten
  metrics).
- New `XRT_LAYER_ZONE_3D` in `xrt_layer_type` +
  `struct xrt_layer_zone_3d_data { proj; rect; }` in the `xrt_layer_data`
  union + one `xrt_comp_layer_zone_3d` vtable method (`comp_base`
  boilerplate). 2D zones reuse `XRT_LAYER_LOCAL_2D` verbatim.
- Extension NOT advertised; everything behind `DISPLAYXR_ZONES=1`.
- **Validation**: a branch of `cube_handle_d3d11_win` issuing two rect-chained
  locates and asserting returned `XrView.fov` equals the app-side-Kooima
  reference per rect (the avatar's math is the oracle). CI selftest + existing
  run scripts prove zero regression.

### Phase 2 — reference compositors (D3D11 + VK) — **DONE**

Shipped. Implementation notes vs plan: no separate super-atlas object — the
renderer atlas IS the super-atlas (zones frames make the effective canvas the
full window, so tiles are window-scaled and zone layers render at sub-tile
viewports; the stride invariant holds by construction). The auto-wish feather
is a stepped ClearView/ClearAttachments ring raster (8 × 2 px, max semantics).
Known VK limitation: the VK renderer is blit-based, so overlapping zones
OVERWRITE in layer order there (one-shot WARN) — alpha-over needs a VK draw
path (follow-up). Validated on the Leia DP via atlas captures (zone placement
per view tile + parallax + overlap alpha-over on D3D11) and wish generations.


- `d3d11_effective_canvas` / `vk_effective_canvas` gain the zone term (zone
  frames ⇒ full-client-window effective canvas, like supersede mode today).
- Super-atlas assembly loop: zone z's view-v tile scaled-blitted into the
  super-atlas view-v tile at the zone's rect, alpha-over in layer order,
  honoring the stride-invariant triple (`atlas_width / tile_columns` at write,
  clamp, read).
- N-rect auto-wish rasterize (generalizes the #491 implicit-mask rasterizer
  from one rect to N, plus feather); published via the existing D3D11
  `publish_local_zone_mask` leg. VK uses the global `request_display_mode`
  fallback until Phase 4 (tier-1 degenerate behavior, acceptable).
- **Validation**: new permanent test app `cube_zones_d3d11_win` (two 3D zones
  with different rigs + one Local2D strip + an explicit feathered wish) on
  sim_display (`SIM_DISPLAY_OUTPUT=sbs` + atlas file-trigger capture: assert
  each zone's content lands at its rect in each view tile) **and a Leia
  eyeball** (hardware-behavior change ⇒ PR blocks on it). Regression: all
  existing cube_* + a Local2D app unchanged.

### Phase 3 — GL / Metal / D3D12 parity + advertise — **DONE** (Metal code-only)

Shipped; extension always advertised, `DISPLAYXR_ZONES` gate removed. The
"delete the single-rect supersede special case" item resolved conservatively:
legacy zero-zone behavior is a compatibility contract and stays; the raster
unification is folded into the per-API wish rasterizers reusing the
implicit-mask resources. Metal validated on a Mac 2026-06-11 (sim_display
sbs + anaglyph atlas captures + live eyeball) via the permanent
`cube_zones_metal_macos` test app — caps, per-zone view sizes, zone
placement per view tile, AUTO wish raster, Tier-2 explicit wish, validate
bit, M/O interactive toggles. Metal Tier-3 wish is structurally absent
(no `XrLocal3DZoneRenderTarget*EXT` Metal binding). The Metal overlap
OVERWRITE limitation found in that pass was fixed same-day: zone draws go
through dedicated alpha-over pipeline variants (premultiplied / straight
per layer flags, depth disabled — D3D11 parity). GL caveat: the masked
composite (and so the wish lerp) exists only on the GL window-present
path — same pre-existing scope as Local2D on GL.


- Mechanical port per the #439 Phase-3 playbook (D3D11 + VK are reference).
- Flip the extension on; delete the single-rect supersede special case where
  the N-rect path subsumes it (keep behavior, kill the branch).
- **Validation**: `cube_zones_*` per API; Metal code-only until a Mac eyeball.

### Phase 4 — DP contract + sim_display — **DONE**

Shipped as planned (zone triple on all 5 vtables, caps appends +
`XRT_DP_LOCAL_ZONE_CAPS_SIZE_V1` floor, per-API compositor publish with the
tier-1 fallback kept for legacy plugins, sim_display parity +
`SIM_DISPLAY_WISH_QUANTIZE` tint on the D3D11 variant, selftest zone-caps
probe — absence never fails). Verified against the installed Leia plugin:
appended fields read 0 (caller-zeroed append-only contract observed).


- Append `wish_fractional` / `switch_granularity` / `reserved[4]` to
  `xrt_dp_local_zone_caps`; port the zone-slot triple to vk / d3d12 / gl /
  metal vtable variants (offset asserts + size bumps, append-only — **no ABI
  major bump**); compositors switch from the global-mode fallback to per-API
  wish publish.
- sim_display: implement the triple on all its API variants, honor
  `wish_fractional=1`, and add `SIM_DISPLAY_ZONE_GRID=WxH` /
  `SIM_DISPLAY_WISH_QUANTIZE=band` env knobs that **visualize the quantized
  effective state** in its output — making the advisory-quantization contract
  CI-testable without hardware. `displayxr-cli selftest` gains a zone-caps
  probe.

### Phase 5 — Leia plugin + IPC

- `displayxr-leia-plugin`: report appended caps (`wish_fractional=0`,
  `switch_granularity` per vendor confirmation — **verify the column-band
  claim with Leia before pinning values**); zero firmware work required
  (conformant via the default any-nonzero quantization).
- IPC: zone rect on the rig-chained locate call (already server-routed since
  PR #479) + `XRT_LAYER_ZONE_3D` serialization (projection + 16 bytes). The
  wish-mask cross-process transport is the one genuinely new IPC surface —
  scoped here, may slip independently (sentinel-paint probe for validation).
- Validate via `XRT_FORCE_MODE=ipc` with `cube_zones_d3d11_win`, then the
  workspace path (wish ignored in v1 per ADR-027 — verify it is *inert*, not
  broken).

### Phase 6 — avatar migration (acceptance)

`displayxr-demo-avatar`: the change at the top of this doc. A/B against
v0.1.0 (projection parity per rect), then ship and delete the app-side Kooima
+ retire the `view-rig-migration` branch.

## Deferred / reserved (v2)

- Effective-mask readback (`get_effective_zone_state` + app-visible channel).
- Workspace wish clip+merge across clients (v1: workspace client wish inert).
- 2D-under-3D ordering (tracks the #491 tail).
- `process_zone_frame` per-zone DP handoff (only if a vendor need materializes:
  per-zone backlight, zone-aware weave skip, multi-display split #69 3b).
