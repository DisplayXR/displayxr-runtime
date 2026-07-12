# XR_DXR_display_zones

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_display_zones` |
| Spec Version | 1 |
| Type Values | `1004999150–1004999153` |
| Author | Leia Inc. |
| Platform | All (extension-app classes only) |
| Requires | `XR_DXR_local_3d_zone` (≥ v3), `XR_DXR_view_rig` (≥ v2) |

> **Status:** implemented (runtime phases P1–P4 of
> `docs/roadmap/display-zones.md`); design per
> [ADR-027](../../adr/ADR-027-display-zones.md). Canonical header:
> `src/external/openxr_includes/openxr/XR_DXR_display_zones.h`.

---

## 1. Overview

`XR_DXR_display_zones` lets an app compose, within its window, **N 3D zones**
(each a window-pixel rect with its own view-rig framing and its own multiview
projection layer), **M 2D zones**, and one **wish mask** (window-space,
any-resolution, per-pixel M ∈ [0,1]) telling the hardware which regions to
physically switch to 3D.

It unifies three existing mechanisms **by composition** — it requires and
reuses, rather than replaces:

- **3D framing** per zone = the `XR_DXR_view_rig` descriptors
  (`XrDisplayRigDXR` / `XrCameraRigDXR`), chained per-locate exactly as today;
  the new `XrDisplayZoneDXR` on the same locate scopes the framing to the
  zone's rect (the rect *is* the canvas).
- **2D zones** = `XrCompositionLayerLocal2DDXR` from `XR_DXR_local_3d_zone`,
  verbatim.
- **Explicit wish mask** = `XrLocal3DZoneMaskDXR` and its three authoring
  tiers, verbatim — referenced per-frame from the `xrEndFrame` chain instead
  of the sticky `xrSubmitLocal3DZoneDXR` channel.

The single-canvas behaviors (the former `xrSetSharedTextureOutputRectDXR` /
surround path — now **removed** in favour of zones, ADR-031 — plus the Local2D +
supersede rule and single-canvas view_rig) are the degenerate single-zone case
(§6). The Local2D/view_rig legacy frames keep working; the output-rect/surround
entry points are gone and map onto one 3D zone + Local2D zones.

## 2. Motivation

`XR_DXR_view_rig` frames exactly one canvas, and the #439 Phase 2 supersede
rule snaps that canvas to the full client window whenever a Local2D layer or
mask is active — so sub-canvas 3D framing and 2D zones are mutually exclusive.
An app like `displayxr-demo-avatar` (3D in the bottom 75%, 2D bubble in the
top 25%) cannot use the runtime-owned rig at all. Display zones decouple the
three jobs the single Local2D mask currently does at once: where 3D content
*is* (zone rects), how it is *framed* (per-zone rigs), and where the panel
physically *switches* (the wish). Full rationale: ADR-027.

## 3. API Reference

### 3.1 Defines

```c
#define XR_DXR_display_zones 1
#define XR_DXR_display_zones_SPEC_VERSION 1
#define XR_DXR_DISPLAY_ZONES_EXTENSION_NAME "XR_DXR_display_zones"

#define XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR               ((XrStructureType)1004999150)
#define XR_TYPE_DISPLAY_ZONE_DXR                            ((XrStructureType)1004999151)
#define XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR            ((XrStructureType)1004999152)
#define XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_DXR ((XrStructureType)1004999153)

typedef XrFlags64 XrDisplayZonesFrameEndFlagsDXR;
//! Cross-check zone/locate/mask consistency this frame: zone rects vs wish
//! coverage, locate-rect vs submit-rect, zoneId pairing. One-shot WARN per
//! violation class per session — never a per-frame error.
#define XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_DXR ((XrDisplayZonesFrameEndFlagsDXR)0x00000001)
```

### 3.2 XrDisplayZoneCapabilitiesDXR

```c
typedef struct XrDisplayZoneCapabilitiesDXR {
    XrStructureType    type;          // XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR
    void* XR_MAY_ALIAS next;
    XrBool32           supported;     // XR_FALSE => only the legacy single-canvas path
    uint32_t           maxZones3D;    // max zone-chained projection layers per frame (8)
} XrDisplayZoneCapabilitiesDXR;

XRAPI_ATTR XrResult XRAPI_CALL xrGetDisplayZoneCapabilitiesDXR(
    XrSession session, XrDisplayZoneCapabilitiesDXR* capabilities);
```

`maxZones3D` is plugin-independent (zone assembly is compositor-side; see
ADR-027 §Decision 5). Hardware switching granularity is deliberately **not**
exposed here — the wish is advisory (§5); the only advisory hardware hint
remains `hardwareZoneGridWidth/Height` in `XrLocal3DZoneCapabilitiesDXR`.

### 3.3 XrDisplayZoneDXR — the dual-chain-point zone declaration

```c
/*!
 * A 3D display zone: identity + placement. Valid at TWO chain points:
 *
 *  - XrViewLocateInfo::next — a ZONE-SCOPED LOCATE: the runtime computes the
 *    Kooima projection framed to `rect` (the rect IS the canvas) and returns
 *    render-ready XrView{pose, fov} for this zone. Chain a rig descriptor
 *    (XrDisplayRigDXR / XrCameraRigDXR) on the same locate as usual; an
 *    XrViewDisplayRawDXR chained on the result reports canvasRectPx /
 *    canvasSizeMeters for THE ZONE.
 *
 *  - XrCompositionLayerProjection::next — binds that layer's views to the
 *    zone at xrEndFrame.
 *
 * Chain the SAME instance at both points within a frame. The xrEndFrame
 * values are authoritative; a locate/submit rect divergence mis-frames one
 * frame (same latency class as pose-prediction error — and the same
 * consistency burden core OpenXR already places on apps when copying XrView
 * pose/fov into XrCompositionLayerProjectionView). VALIDATE_BIT warns.
 *
 * `rect` is in client-window pixels — the same space as
 * XrCompositionLayerLocal2DDXR::rect and the mask authoring tiers.
 */
typedef struct XrDisplayZoneDXR {
    XrStructureType          type;    // XR_TYPE_DISPLAY_ZONE_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 zoneId;  // app-chosen; unique among this frame's 3D zones
    XrRect2Di                rect;    // client-window pixels
} XrDisplayZoneDXR;
```

`zoneId` exists for locate↔submit pairing (validate mode), debug
logs/captures, and as the stable referent for the reserved effective-mask
readback. There is **no zone handle** — zones are stateless per-frame data,
like layers; only the mask owns GPU resources and already has a handle.

### 3.4 XrDisplayZonesFrameEndInfoDXR — per-frame wish reference

```c
/*!
 * Optional, chained on XrFrameEndInfo::next in a zones frame.
 *
 * Absent, or wishMask == XR_NULL_HANDLE: the wish AUTO-DERIVES as the union
 * of the frame's 3D-zone rects with an implementation-defined feather.
 *
 * Present with a mask: that mask is the frame's wish verbatim — atomic with
 * the layer set. In zones mode the wish is HARDWARE-ONLY: it does not gate
 * compositor blending (composition follows zone geometry + alpha; §4).
 */
typedef struct XrDisplayZonesFrameEndInfoDXR {
    XrStructureType                 type;   // XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR
    const void* XR_MAY_ALIAS        next;
    XrDisplayZonesFrameEndFlagsDXR  flags;
    XrLocal3DZoneMaskDXR            wishMask;  // XR_NULL_HANDLE = auto-derive
} XrDisplayZonesFrameEndInfoDXR;
```

### 3.5 Per-zone view sizing

```c
//! Pure query: recommended per-view image size for a zone of this rect under
//! the current display mode — the zone-rect analog of
//! xrEnumerateViewConfigurationViews. Zones share the session's view COUNT
//! (display modes are session-global); only per-view dimensions vary by rect.
XRAPI_ATTR XrResult XRAPI_CALL xrGetDisplayZoneRecommendedViewSizeDXR(
    XrSession session, const XrRect2Di* zoneRect, XrExtent2Di* recommendedViewSize);

/*!
 * Advisory: per-zone recommended view sizes may have changed (display-mode /
 * tile-count switch, window DPI change). Re-query each zone; stale sizes stay
 * correct, just soft. The N-zone analog of
 * XrEventDataLocal3DZoneViewSizeChangedDXR (which is single-size and cannot
 * describe N zones; it keeps firing for legacy sessions).
 */
typedef struct XrEventDataDisplayZoneMetricsChangedDXR {
    XrStructureType          type;   // XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_DXR
    const void* XR_MAY_ALIAS next;
    XrSession                session;
} XrEventDataDisplayZoneMetricsChangedDXR;
```

Zone swapchains are fixed-size: when a rect animates, the runtime scaled-blits
the view tile to the rect (sharpness trades off). Apps wanting 1:1 recreate on
resize, prompted by the event + re-query.

## 4. Frame rules ("zones mode")

A frame is a **zones frame** iff ≥ 1 projection layer carries an
`XrDisplayZoneDXR` chain. Rules, all per ADR-027:

1. **All-or-none.** Every projection layer in a zones frame must carry a zone
   chain, else `xrEndFrame` returns `XR_ERROR_VALIDATION_FAILURE`. Other layer
   types (quad, Local2D, window-space) are unaffected.
2. **Legacy state is inert, not an error.** In a zones frame each zone rect is
   its own canvas, the sticky `xrSubmitLocal3DZoneDXR` mask is ignored, and the
   implicit-mask-from-Local2D-rects rule is off (Local2D layers are pure 2D
   content). This permits frame-by-frame migration.
3. **Atomicity** (inherited from #439): zone layers + Local2D layers + the
   frame-end wish reference are consumed as one coherent `xrEndFrame` set.
4. **Overlap allowed**: overlapping 3D zones composite alpha-over in
   layer-list order, like core OpenXR projection layers. Exact-equal N-way
   averaging is not a primitive; blends are expressed through content alpha.
5. **Ordering (v1)**: Local2D composites post-weave (over 3D). 2D-under-3D is
   reserved (#491 tail).
6. **Locate/submit asymmetry allowed**: locating a zone you don't submit is
   legal (wasteful); submitting a zone not located this frame is legal (stale
   framing — validate mode warns).
7. **Zero-zone frames** behave per `XR_DXR_local_3d_zone` v3 verbatim —
   supersede rule, implicit mask, sticky submit all unchanged.
8. **Workspace mode**: a workspace client's wish is ignored in v1 (the
   controller owns the display mode, consistent with `xrRequestDisplayMode`
   being a no-op); zone rendering still works inside the client's tile.
   `xrSetWorkspaceViewRigDXR` overrides substitute rig tunables on every
   zone-scoped locate; zone rects stay app-owned.

## 5. Wish semantics

Per-pixel M ∈ [0,1]: 1.0 = panel physically 3D, 0.0 = flat, intermediate =
fractional 3D-ness at the display processor's discretion (blend, dither,
threshold). The carrier is the existing R8_UNORM window-space mask — boolean
masks are valid wishes; fractional authoring is now blessed rather than only
arising at anti-aliased edges.

**The wish is advisory.** The DP MAY quantize, dilate, or snap it to its
switching-cell granularity (including lenticular column/row bands extending
beyond the window into the surround), coalesce updates, or ignore components
the firmware cannot express. The default quantization is the existing rule:
any non-zero wish pixel overlapping a hardware cell ⇒ that cell switches 3D,
OR-union across clients. The app MUST NOT assume the physical state equals the
wish; an effective-state readback is **reserved for spec v2** and intentionally
absent from v1. The wish never describes panel pixels outside the window —
window→panel mapping is the plugin's job (vendor isolation, ADR-019).

## 6. Degenerate single-zone mapping (back-compat)

| Today (legacy frame — no zone chains) | Display-zones equivalent | Old path still works? |
|---|---|---|
| `_handle` full-window app + `XrDisplayRigDXR` per-locate | One zone: rect = full client window, same rig; auto wish = full window | Yes — an unchained locate frames the session canvas, verbatim view_rig v2 |
| Former `xrSetSharedTextureOutputRectDXR(x,y,w,h)` + surround-2D textures (**removed**, ADR-031) | One zone: rect = (x,y,w,h); surround content as Local2D layers covering the remainder | N/A — these entry points are removed; express the region directly as one 3D zone + Local2D zones |
| Local2D frame: explicit mask + 2D layers, supersede → full window | One full-window zone + the same Local2D layers + the same mask object as `wishMask` | Yes — supersede fires only in legacy frames |
| Implicit mask = union of Local2D rects (M = 0 inside) | Zone rects state the 3D region directly; auto wish = union of zone rects | Yes (legacy frames); rule off in zones frames |
| `xrSubmitLocal3DZoneDXR` sticky mask | Per-frame `wishMask` in the frame-end chain | Yes — sticky path live in legacy frames; inert in zones frames |
| `XrEventDataLocal3DZoneViewSizeChangedDXR` | `XrEventDataDisplayZoneMetricsChangedDXR` + per-zone re-query | Yes — old event keeps firing for legacy sessions |
| App-side Kooima (avatar v0.1.0) | Zone-scoped locate + `XrDisplayRigDXR` | N/A — the point of the design |

Nothing is deprecated: `local_3d_zone` v3 and `view_rig` v2 remain the
documented path for single-canvas apps.

## 7. Display-processor contract delta (runtime-internal)

Summarized here for spec completeness; normative detail in ADR-027 §Decision 5.
The weave handoff is unchanged (compositor assembles a window-spanning
super-atlas; `process_atlas` with canvas = full client window). Append-only
additions, no ABI major bump:

- `xrt_dp_local_zone_caps` gains `wish_fractional`, `switch_granularity`
  (advisory, never app-visible), `reserved[4]`.
- `publish_local_zone_mask`'s mask is redefined upward as the wish (M ∈ [0,1];
  existing any-nonzero downsample = default quantization, so existing plugins
  are conformant unmodified).
- The zone-slot triple is ported from the D3D11 vtable (slots 12–14) to the
  vk / d3d12 / gl / metal variants, appended per ADR-020.
- Reserved v2: `process_zone_frame` (per-zone descriptor arrays,
  struct_size/stride evolution) + `get_effective_zone_state`.

## 8. Example — `displayxr-demo-avatar`

See the migration note in `docs/roadmap/display-zones.md` for the full
frame-loop walkthrough (one bottom-75% zone + display rig, one Local2D bubble,
no mask object — auto wish).
