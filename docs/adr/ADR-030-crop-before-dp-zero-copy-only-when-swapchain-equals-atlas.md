# ADR-030: Compositor Crops to Content; Zero-Copy Only When the Swapchain Equals the Mode Atlas

**Status:** Accepted
**Date:** 2026-06-14

## Context

The app swapchain is allocated **once** at session creation, sized to the **per-dimension
worst case across all of the DP's rendering modes, assuming a full-screen window** (ADR-010,
[swapchain-model](../specs/runtime/swapchain-model.md)):

```
swapchain_width  = max over modes i of ( tile_columns[i] × view_width[i] )
swapchain_height = max over modes i of ( tile_rows[i]    × view_height[i] )
view_width[i]    = display_width  × mode[i].view_scale_x      (full-screen assumption)
view_height[i]   = display_height × mode[i].view_scale_y
```

The width worst case and the height worst case may come from **different** modes, so the
swapchain is a bounding box that need not match any single mode's content shape.

Each frame the app renders a **content region** for the *active* mode
(`tile_columns × view_width` by `tile_rows × view_height`) into the **top-left** of that
swapchain. The vendor display processor (DP) takes a texture whose dimensions match the
content exactly (it derives tile stride as `texture_dim / tile_columns` and, for some
vendors, feeds the texture size straight into the weaver). So the compositor must hand the
DP a content-sized surface — **never** the oversized swapchain — or the DP samples the
unused padding and the image shifts / weaves garbage.

This left two operations whose contract was understood by individual authors but never
written down as a single load-bearing rule, so each compositor call site grew its own
ad-hoc eligibility test (e.g. `view_count > 1`, coupling to the hardware 2D/3D flag). That
drift is the root of the forced-IPC 2D left-shift (issue #575).

## Decision

**1. Cropping before the DP is mandatory and is the default path.** Every compositor (native
in-process and the D3D11 service / multi-compositor) computes
`content = (tile_columns × view_width, tile_rows × view_height)` for the active mode and
copies that top-left region into a content-sized intermediate before calling
`process_atlas()`. Helpers: `d3d11_crop_atlas_for_dp` (in-process),
`service_crop_atlas_for_dp` (service).

**2. Zero-copy (handing the app's swapchain image straight to the DP, skipping the crop) is a
pure optimization, valid in exactly one case: the app's submitted layout *equals* the active
mode's atlas and fills the swapchain.** This is decided **solely** by the shared predicate
`u_tiling_can_zero_copy()` (`auxiliary/util/u_tiling.h`), which requires **all** of:

- `submitted view_count == mode->view_count` (the #542 divergence guard — a hardware/content
  mismatch frame must take the crop path; zero-copy cannot re-tile a mismatched submission),
- `swapchain_w == mode->atlas_width_pixels && swapchain_h == mode->atlas_height_pixels`, and
- every view's sub-rect matches its expected tile origin and size.

No call site may add its own eligibility proxy (view-count thresholds, hardware-mode
coupling, mode-index checks). If `u_tiling_can_zero_copy()` is false, **crop.** Cropping is
always correct; zero-copy is the special case, not the other way around.

## Rationale

Because the swapchain is the worst-case envelope, the per-frame content equals it **only when
both** hold:

1. the window is **full-screen** (a windowed app renders `view = window × scale`, strictly
   smaller than the `display × scale` envelope in at least one dim → content < swapchain →
   must crop), and
2. the active mode is the one that hits the per-dim worst case in **both** width and height.

So zero-copy is a rare coincidence, and which mode (if any) triggers it is **DP- and
platform-specific**:

- **Windows Leia DP.** Modes: 2D = `1×1 @ 1.0×1.0`; LeiaSR/3D = `2×1 @ 0.5×0.5` (X may be
  refined by the SR SDK). Envelope = `max(W, 2·0.5W) × max(H, 0.5H) = W × H` = full display.
  The only mode that fills it in **both** dims is **2D** (3D is half-height → its content is
  `W × 0.5H`, always smaller → always crops). ⇒ **zero-copy fires only for full-screen 2D.**
- **Android Leia DP.** Its modes (e.g. 3D at `0.75×0.75`) never reach the envelope in either
  dim, so the swapchain never equals the atlas. ⇒ **zero-copy never fires; it always crops.**

These are consequences of the single rule, not separate special cases — which is exactly why
the rule, its rationale, and these consequences must be documented rather than rediscovered.

## Consequences

- `u_tiling_can_zero_copy()` is the **one** zero-copy gate. Reviewers reject any compositor
  branch that decides zero-copy by another means, or that hands the DP a surface larger than
  `tile_columns × view_width × tile_rows × view_height`.
- The DP keeps its dimension guarantee (texture dims == content dims), so vendor weavers may
  assume `1/tile_columns`, `1/tile_rows` UV scaling without querying texture size
  (see [multiview-tiling](../specs/runtime/multiview-tiling.md#dp-side-dimension-guarantee)).
- A windowed app, or any non-envelope-filling mode, always takes the crop path — including
  full-screen 3D on Windows Leia (half-height) and everything on Android.
- The forced-IPC 2D regression (#575) is fixed by aligning the service path's gate to this
  rule (drop the `view_count > 1` / hardware-flag coupling, gate on `u_tiling_can_zero_copy`).

## Related

- ADR-007 (compositor never weaves), ADR-010 (worst-case swapchain sizing),
  ADR-005 (multiview atlas layout), ADR-028 (display-mode recipe vs hardware state),
  ADR-026 (orientation-aware view scaling).
- [multiview-tiling](../specs/runtime/multiview-tiling.md) — crop-blit algorithm + the
  coupled atlas-stride invariant.
