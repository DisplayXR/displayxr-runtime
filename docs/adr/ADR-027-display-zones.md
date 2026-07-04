---
status: Accepted
date: 2026-06-10
issues: [439, 396]
---
# ADR-027: Display Zones — decoupled mixed 2D/3D layout, per-zone rig, wish mask

> **Update (ADR-031):** the legacy 2D-surround / `xrSetSharedTextureOutputRectEXT`
> mechanism this ADR contrasts against (Context, Consequences) has since been **removed** —
> display-zones is now the sole region paradigm for all app classes. The surround references
> below are accurate history; see ADR-031 + `docs/roadmap/surround-zones-deprecation.md`.

> **Taxonomy note (2026-07, #696):** this decision also **narrows the `_texture`
> app class**. Making display-zones the region paradigm for *every* class means
> content-handoff (`handle` / `texture` / `hosted` / `ipc`) and region paradigm
> (zones + mask) are now **orthogonal axes** — texture is no longer "how you weave
> a sub-rect," it is just present-ownership handoff for a producer with no window
> the runtime can weave into (CEF, WebXR bridge, decode target). Two consequences
> to record: (1) in test-app naming `zones` is a **feature modifier, not a class**
> — `cube_zones_*` is a *handle* app exercising zones, `cube_zones_texture_*` a
> *texture* app exercising zones; there is no `zones/` group. (2) For an
> out-of-process texture producer, the DP still needs an **on-screen HWND as a
> position/phase anchor** (drag phase-snap + canvas-scoped Kooima via
> `get_window_metrics`), *not* as a render target — shared-texture mode creates no
> swapchain and never presents (`comp_d3d11_compositor.cpp:2103,2251-2333`); the
> NULL-HWND path exists but degrades to display-scoped Kooima + no phase-snap.
> Decoupling that position channel from the HWND (an on-screen target-rect binding
> for genuinely windowless producers) is designed separately in **#697**. See
> `docs/getting-started/app-classes.md` for the app-facing framing.

## Context

The current composition model couples three concerns into one object chain:

- `XR_EXT_view_rig` frames **one** canvas (the full window for `_handle` apps; a
  texture sub-rect via `xrSetSharedTextureOutputRectEXT` for `_texture` apps).
- `XR_EXT_local_3d_zone` supplies 2D content (`XrCompositionLayerLocal2DEXT`)
  and the 2D/3D mask — but per the #439 Phase 2 **supersede rule**, any active
  mask (explicit or implicit-from-Local2D-rects) snaps the weave region, view
  dimensions, and projection metrics to the **full client window**, with the
  mask as the sole 2D/3D selector.

So sub-canvas rig framing and 2D zones are mutually exclusive. The concrete
wall: `displayxr-demo-avatar` (transparent 3D tiger in the bottom 75% of its
window, flat 2D speech bubble in the top 25%) cannot migrate from its app-side
Kooima to the runtime-owned rig — the moment its bubble Local2D layer appears,
the rig can only frame the full window. It ships today only because its
app-side Kooima computes the sub-canvas projection itself, bypassing the
runtime canvas — exactly the duplication ADR-024 set out to delete.

This ADR decouples the three jobs so an app can place **N 3D content zones**
(each with its own view framing), **M 2D content zones**, and submit a
**semi-transparent "wish" mask** telling the hardware which regions to
physically switch to 3D.

## Decision

### The model — three orthogonal planes

1. **3D zones (N)**: each = `{window-px rect, multiview swapchain, view-rig
   descriptor}`. The zone's rect *is* its canvas: the runtime computes that
   zone's off-axis Kooima framed to the rect (ADR-024 — Kooima stays
   runtime-side; only the rect changes per zone) and returns render-ready
   `XrView{pose, fov}` per zone.
2. **2D zones (M)**: each = `{window-px rect, flat swapchain}`, composited
   flat. This is exactly today's `XrCompositionLayerLocal2DEXT`, reused
   verbatim.
3. **Wish mask (1)**: per-pixel M ∈ [0,1] (1 = panel physically 3D, 0 = flat,
   intermediate = fractional 3D-ness at the DP's discretion), authored at any
   resolution in **window space**, atomic with the frame's content. The
   **plugin** maps window→panel (it has the HWND and tracks position),
   quantizes to whatever the firmware needs, and drives the physical switch.

The rig plane (where 3D is *drawn* — projection geometry, runtime-side,
ADR-024) and the wish plane (where the panel physically *switches* —
plugin-side, ADR-007) are orthogonal; the app keeps them consistent, with an
auto-derived wish making the simple case consistent by construction.

**The wish mask is window-scoped at the app boundary, never panel-sized.** The
surround (panel pixels outside the window, other windows, the desktop) is never
the app's business — this preserves vendor isolation (ADR-019) and the
multi-window story.

### Zones mode is per-frame; the supersede rule becomes the legacy-frame rule

A frame is a **zones frame** iff at least one projection layer carries an
`XrDisplayZoneEXT` chain (see the [spec sketch]). In a zones frame:

- The canvas output rect (`xrSetSharedTextureOutputRectEXT`) is **ignored** —
  there is no single canvas to supersede; each zone's rect is its canvas. The
  supersede rule's job ("mask is the sole selector over a known weave region")
  is subsumed by zone rects being explicit.
- The sticky legacy mask (`xrSubmitLocal3DZoneEXT`) is **ignored for that
  frame** (not an error — this enables frame-by-frame migration).
- The implicit-mask-from-Local2D-rects rule is **off**: Local2D layers in a
  zones frame are pure 2D content; the wish derives from the 3D-zone rects (or
  an explicit mask), not from 2D rects.
- **All-or-none**: if any projection layer in a frame carries a zone chain, all
  projection layers in that frame must, else `XR_ERROR_VALIDATION_FAILURE`. An
  unchained projection layer in a zones frame has no defined canvas; guessing
  "full window" would silently recreate the ambiguity this ADR kills.

A frame with **zero** zone-chained projection layers behaves per
`XR_EXT_local_3d_zone` v3 verbatim — supersede rule, implicit mask, sticky
submit, all unchanged. **Back-compat is structural, not emulated**; nothing is
deprecated.

### Decision 1 — wish mask: auto-derived AND explicit

Default (no explicit mask): the wish auto-derives as the union of the frame's
3D-zone rects with an implementation-defined feather at the edges. The simple
case (the avatar) needs zero mask code, and the rig/wish planes cannot drift.
Explicit: the app references an authored mask per frame for transitions,
feathering, and partial-weave effects.

The explicit-wish carrier is the **existing `XrLocal3DZoneMaskEXT`**,
unmodified — it already has the right physics: window-space, any-resolution,
R8_UNORM (already M ∈ [0,1]), three authoring tiers (whole-window / rect-list /
freeform render target with D3D11/D3D12/Vulkan bindings). Re-inventing it would
duplicate seven entry points and five structs for zero gain. What changes is
*reference and interpretation*: in a zones frame the mask is referenced from
the `xrEndFrame` chain (atomic with the layer set, replacing the sticky
side-channel) and is hardware-only — it no longer gates compositor blending
(see composition rules below).

### Decision 2 — consistency burden: auto-default + validate flag; readback reserved

Decoupling lets an app weave 3D content into a region its wish leaves flat
(garbled on glass). Mitigations, in order: (1) the auto-mask default makes the
inconsistent state impossible unless the app opts into an explicit mask;
(2) `XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_EXT` — the runtime cross-checks
zone rects vs mask coverage, locate-rect vs submit-rect, and zoneId pairing,
emitting one-shot WARNs per violation class (clamp-and-warn, never per-frame
reject, per the ADR-024 validation philosophy); (3) a **resolved/effective-mask
readback** (what the firmware actually switched, post-quantization) is
explicitly **reserved for v2**, not built — the append-only ABI (ADR-020) and
OpenXR next-chains leave room, and building it now would round-trip data with
no consumer.

### Decision 3 — submission shape: composition-layer-chained

A 3D zone is declared by one struct, `XrDisplayZoneEXT{zoneId, rect}`, valid at
**two chain points**:

- `XrViewLocateInfo::next` — a **zone-scoped locate**: Kooima frames to `rect`;
  the rig descriptor chains on the same locate exactly as today (per-locate,
  ADR-024).
- `XrCompositionLayerProjection::next` — binds that layer's views to the zone
  at `xrEndFrame`.

The app chains the same instance at both points each frame; the `xrEndFrame`
values are authoritative. This locate→submit consistency burden is precedented
by core OpenXR itself (apps already copy `XrView{pose, fov}` from
`xrLocateViews` into `XrCompositionLayerProjectionView` verbatim); a divergent
rect mis-frames one frame — the same latency class as pose-prediction error —
and validate mode warns. The alternative (a dedicated "zone layout" submit
call) was rejected: it introduces sticky session state that breaks the "one
coherent xrEndFrame set" atomicity #439 established, and it is alien to
OpenXR's everything-is-per-call frame model.

**No zone handles.** Zones are stateless per-frame data, like layers — they own
no GPU resources (the projection layer's swapchains are the content; the mask,
the one thing that does own GPU resources, already has a handle). `zoneId` is
an app-chosen `uint32_t`, unique among the frame's 3D zones, existing so
validate mode can pair locates with submissions, so logs/captures can name
zones, and so the reserved effective-mask readback has a stable referent.

### Decision 4 — extension structure: new `XR_EXT_display_zones`, unified by composition

A new extension, **requiring** `XR_EXT_local_3d_zone` (≥ v3) and
`XR_EXT_view_rig` (≥ v2), that **reuses their types** rather than superseding
them with fresh declarations:

| Concern | Carrier | New or reused |
|---|---|---|
| 3D zone geometry + identity | `XrDisplayZoneEXT` | **new** (the only genuinely new concept) |
| Per-zone view framing | `XrDisplayRigEXT` / `XrCameraRigEXT`, per-locate | reused verbatim |
| Per-zone raw readback | `XrViewDisplayRawEXT` (`canvasRectPx`/`canvasSizeMeters` report the zone) | reused verbatim |
| 2D zone | `XrCompositionLayerLocal2DEXT` | reused verbatim |
| Explicit wish mask | `XrLocal3DZoneMaskEXT` + its 3 authoring tiers | reused verbatim |
| Per-frame wish reference + flags | `XrDisplayZonesFrameEndInfoEXT` on `XrFrameEndInfo::next` | **new** |

Rationale over a `local_3d_zone` v4 major bump: migration cost (an app's 2D and
mask code is byte-identical to today's), implementation cost (the runtime's
mask tiers, R8 staging, and Local2D compositor path are reused — only the
interpretation changes in zones mode), and it keeps `local_3d_zone` v3 fully
alive for single-canvas apps instead of deprecating a months-old extension.
The cost — apps enable three extensions instead of one — is one line of code.

New surface in full: `XrDisplayZoneCapabilitiesEXT`, `XrDisplayZoneEXT`,
`XrDisplayZonesFrameEndInfoEXT`, `XrEventDataDisplayZoneMetricsChangedEXT`,
`xrGetDisplayZoneCapabilitiesEXT`, `xrGetDisplayZoneRecommendedViewSizeEXT`.
Type values 1000999150–153. Header-level sketch:
`docs/specs/extensions/XR_EXT_display_zones.md`.

Scope rules: extension-app classes only (like `local_3d_zone`); view count per
zone = the session's view count (display modes are session-global — zones vary
in rect/rig/size, never in view count); `maxZones3D` advertised as 8
(compositor-side assembly makes the budget plugin-independent).

### Composition rules — overlap and ordering

- **Overlapping 3D zones are allowed**, composited **alpha-over in layer-list
  order** — the natural generalization of core OpenXR's multiple-projection-
  layer compositing (`XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`).
  It scales to N, and cross-fades fall out of app-animated alpha. Two caveats
  the spec states plainly: exact-equal N-way averaging is *not* a primitive
  (not expressible as over-compositing — apps express blends through content
  alpha), and overlapping zones with different rigs read as coexisting
  translucent holograms (each internally consistent; the combination is the
  app's aesthetic call, not an error).
- **Local2D composites post-weave (over 3D) in v1**, as today. 2D-under-3D
  (a Local2D layer beneath a zone's weave) is reserved — it matches the open
  #491 tail and does not gate this design. The #491 2D-under backdrop channel
  (`set_background_2d`) is unchanged and sufficient when that lands: the DP's
  compose-under path doesn't care how many rects produced the backdrop.
- The same mask that drives the hardware wish also drives the post-weave
  content blend (`final = M·weave + (1−M)·flatten(2D-over)` in the masked
  composite), so fractional M yields a correct *visual* blend even on
  binary-switch hardware.

### Decision 5 — DP/plugin handoff: super-atlas weave + wish generalization

**The weave handoff does not change.** Because zones share the session's
rendering mode, the weave shader's job (sample view *v* at panel pixel *p*,
phase from screen position) is identical whether the atlas content came from
one canvas or N zones. The compositor therefore assembles the N per-zone
atlases into **one window-spanning super-atlas** — zone *z*'s view-*v* tile is
composited into the super-atlas view-*v* tile at the zone's rect — and hands
the DP today's `process_atlas` call with canvas = full client window. This is
precisely the shipped #439 Phase 2 supersede-mode path, generalized from one
implicit full-window projection to N placed ones. It respects ADR-007
(compositing zone atlases is layer accumulation — the compositor's job; a
per-zone DP handoff would force every vendor to build an internal N-source
compositor, and shipping vendor weavers take a single input atlas), keeps the
atlas-stride invariant intact (one tiling layout; `slot stride = atlas_width /
tile_columns`, all three coupled callsites untouched), and keeps lenticular
phase continuous across the window with zero new phase plumbing.

The **new DP surface is the wish generalization only** — all append-only per
ADR-020, **no ABI major bump**:

1. **Caps append** to `xrt_dp_local_zone_caps` (caller-zeroed, struct_size-
   gated; absent ⇒ 0): `wish_fractional` (1 = DP meaningfully consumes
   M ∈ (0,1); 0 = DP quantizes by the existing any-nonzero rule) and
   `switch_granularity` (advisory enum: unknown / global / column-band /
   row-band / cell-grid — informational, never leaked to the app API), plus
   `reserved[4]` for the v2 readback caps.
2. **Wish semantics rebase on `publish_local_zone_mask`** — a doc change, not a
   signature change. The published R8_UNORM window-space mask is redefined
   *upward* as the wish: M ∈ [0,1], fractional at DP discretion. The existing
   downsample-and-arbitrate rule ("any non-zero mask pixel overlapping a
   hardware cell ⇒ cell 3D, OR-union across clients") is restated as the
   **default quantization of the wish** — so every existing plugin (including
   sim_display) is already conformant at `wish_fractional = 0`, and boolean
   Local3DZone masks are valid wishes. Zero plugin migration.
3. **Port the zone-slot triple** (`get_local_zone_caps` /
   `publish_local_zone_mask` / `clear_local_zone_mask`) from the D3D11 vtable
   (slots 12–14, the only variant that has them today) to the vk / d3d12 / gl /
   metal variants — appended at each variant's current end with the standard
   offset asserts, texture-handle types mirroring each variant's
   `process_atlas` conventions.

A reserved v2 shape (`process_zone_frame` taking per-zone descriptor arrays
with struct_size/stride, plus `get_effective_zone_state` readback) is
documented in the spec sketch so a future vendor need (per-zone backlight,
zone-aware weave skip, multi-display split per #69 3b) has a pre-agreed slot —
it is intentionally **not** implemented in v1.

**Three-tier plugin negotiation** (decided once at session start):

1. **No zone slots** (old plugin, or non-D3D11 variants before the port): the
   runtime still renders zones correctly — super-atlas + weave are
   plugin-agnostic — but cannot drive a per-region physical switch; it falls
   back to the global `request_display_mode` path ("any zone active ⇒ request
   3D"). Visually correct on always-3D panels; whole window switches on
   switchable panels. **Zones never hard-require a new plugin.**
2. **Zone slots, `wish_fractional = 0`**: wish published; DP quantizes by
   any-nonzero. Fractional values still produce the visual blend; only the
   physical switch is binary.
3. **`wish_fractional = 1`**: the DP consumes intermediate M (blend, dither,
   partial drive — its call).

### Decision 6 — hardware constraints stay plugin-owned

The wish is **advisory**. Normative contract language: the DP MAY quantize,
dilate, or snap the wish to its switching-cell granularity (including
lenticular column/row bands that extend beyond the window into the surround),
MAY coalesce updates, and MAY ignore components the firmware cannot express.
The runtime and app MUST NOT assume the physical state equals the wish. No
panel-cell geometry leaks into the app API beyond the existing advisory
`hardwareZoneGridWidth/Height` in `XrLocal3DZoneCapabilitiesEXT`; the reserved
effective-state readback is the future pressure valve for apps that need to
know.

### Workspace interaction

In workspace mode the controller owns the display mode (`xrRequestDisplayMode`
is already a no-op for workspace clients). Consistently: **a workspace
client's wish is ignored in v1** — zone *rendering* (per-zone rigs and rects)
still works inside the client's tile; only the hardware wish is inert.
Clipping each client's wish to its tile and merging across clients is reserved
for v2. `xrSetWorkspaceViewRigEXT` overrides apply **per zone** — the override
substitutes rig tunables on every zone-scoped locate; zone rects stay app-owned
(controllers own visual policy; apps own their internal layout).

## Consequences

- The avatar migrates: one 3D zone (bottom-75% rect + display rig), one Local2D
  bubble, no mask object (auto wish), app-side Kooima deleted. Walkthrough +
  phased plan: `docs/roadmap/display-zones.md`.
- `xrSetSharedTextureOutputRectEXT` + surround-2D, Local2D v3, and single-
  canvas view_rig remain the documented path for single-canvas apps; each maps
  onto the new model as a degenerate single-zone case (mapping table in the
  spec sketch). The supersede special-case code is eventually *subsumed* by the
  N-rect path (delete the branch, keep the behavior) rather than living beside
  it.
- Compositor cost: one scaled blit per zone per view into the super-atlas, and
  zero-copy tiling is off in zones mode — the same costs supersede mode already
  pays today whenever Local2D is active. Zone swapchains are fixed-size; an
  animating rect is scaled-blitted (precedent: ADR-010 worst-case sizing),
  trading sharpness — apps wanting 1:1 recreate on resize, prompted by
  `XrEventDataDisplayZoneMetricsChangedEXT` + `xrGetDisplayZoneRecommendedViewSizeEXT`.
- IPC: the zone rect rides the rig-chained locate (already server-routed); the
  zone layer serializes like projection + 16 bytes; the wish mask is the one
  genuinely new cross-process surface and may slip independently.

## Risks / open items

- **Verify the column-band switching claim** with the vendor before pinning
  `switch_granularity` enum values into the contract.
- Fractional wish on real hardware is untested; v1 ships with the visual blend
  carrying the fraction and the physical switch quantized.
- `_texture`-class zone apps: the canvas sub-rect becomes a zone rect
  naturally, but the shared-texture worst-case-sizing interaction needs its own
  check during compositor implementation.
- Pre-existing `XrStructureType` collisions in the extension range
  (local_3d_zone vs mcp_tools at 1000999130–132; atlas_capture vs
  workspace_file_dialog at 1000999120–121; macos_gl_binding vs display_info at
  1000999010) — independent of this design (the 150+ block is clean); resolved
  by the relocation + the allocation registry in
  `src/external/openxr_includes/openxr/README.md`.

## References

- Spec sketch: `docs/specs/extensions/XR_EXT_display_zones.md`. Migration note
  + phased plan: `docs/roadmap/display-zones.md`.
- Supersede rule being generalized:
  `src/external/openxr_includes/openxr/XR_EXT_local_3d_zone.h` (§ around
  `xrSubmitLocal3DZoneEXT`, #439 Phase 2).
- Invariants held: ADR-007 (compositor never weaves), ADR-019 (vendor
  isolation), ADR-020 (append-only plugin ABI), ADR-024 (Kooima runtime-side,
  per-locate rigs).
- Plumbing this generalizes: `u_canvas_apply_to_metrics`
  (`src/xrt/auxiliary/util/u_canvas.h`), the effective-canvas/supersede logic
  (`comp_d3d11_compositor.cpp`, `comp_vk_native_compositor.c`), the D3D11 DP
  zone-mask slots 12–14, `docs/specs/runtime/multiview-tiling.md`.
- History: #439 (Local2D phases), #396 W7 (view rig), #491 (alpha-over /
  2D-under tail).

[spec sketch]: ../specs/extensions/XR_EXT_display_zones.md
