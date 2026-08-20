// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-supplied 2D backdrop for compose-under transparency (#1073 T0).
 * @author David Fattal
 * @ingroup comp_multi
 *
 * Tier 0 of `docs/roadmap/android-transparency-compose-under.md`: the
 * *producer* half of compose-under on Android.
 *
 * The weave assigns views per **subpixel** while RGBA carries **one** alpha per
 * pixel, so a mixed-alpha pixel (the avatar silhouette / the parallax
 * de-occlusion band) has no correct alpha at all — the post-weave gate can only
 * relocate the error, never remove it. Windows and Linux resolve this *before*
 * the per-subpixel collapse by compositing a background under every view and
 * weaving an opaque result. Android had the compose pass nowhere and, more
 * fundamentally, no background image: no public API captures the screen while
 * excluding your own layer (see §4 of the design note).
 *
 * T0 sidesteps the capture problem entirely: the **runtime** supplies a static
 * backdrop — a solid colour or a vertical gradient it draws itself. Zero
 * permissions, zero latency, zero power, and correct for exactly the case the
 * band needs (the band is thin, so a low-fidelity backdrop is good enough).
 * T1 (MediaProjection) and T2 (vendor-privileged capture) later fill the *same*
 * seam, `xrt_display_processor::set_background_2d` (base DP vtable slot 16) —
 * no new slot, no ABI bump.
 *
 * Off by default. Selected by the `debug.dxr.bg2d` sysprop (Android) /
 * `DXR_BG2D` env var:
 *
 *   unset | `0` | `off`     backdrop disabled — today's live alpha-gate path
 *   `1` | `on` | `grad`     default gradient (dark slate → near-black, top→bottom)
 *   `RRGGBB` | `solid:RRGGBB`   flat colour
 *   `grad:RRGGBB,RRGGBB`    vertical gradient, top colour first
 *
 * The image is opaque, so premultiplied and straight RGBA agree — the slot's
 * premultiplied contract is satisfied by construction.
 */

#pragma once

#include "vk/vk_helpers.h"
#include "xrt/xrt_defines.h" // struct xrt_rect (the T2 canvas crop, #174)

#ifdef __cplusplus
extern "C" {
#endif

struct multi_compositor;

/*!
 * Is a runtime-supplied backdrop configured at all?
 *
 * Parsed once per process from `debug.dxr.bg2d` / `DXR_BG2D`. Cheap enough to
 * call per frame; returns false (and logs nothing) when unset, which is the
 * shipping default.
 */
bool
comp_multi_bg2d_enabled(void);

/*!
 * Lazily build this session's backdrop image and return its view, or
 * VK_NULL_HANDLE when disabled or on any failure (the caller then clears the
 * DP's background and the DP weaves the raw atlas — today's behaviour).
 *
 * The upload runs on its own one-shot command buffer and is waited on before
 * returning, deliberately: the Android vendor DP is *self-submitting*, so a
 * copy recorded into the compositor's frame command buffer would still be
 * unsubmitted when the DP's compose pass samples the image.
 *
 * ## T2 and the canvas rect (#174)
 *
 * Slot 16's contract is that the backdrop arrives *"in the same client-window
 * pixel space / canvas rect as `process_atlas`"* — the DP therefore maps the
 * whole image onto each atlas tile (`bg_uv` = (0,0)-(1,1)) and must not be
 * asked to guess a sub-rect. T0 satisfies that by construction: the runtime
 * draws the backdrop, so it *is* canvas-space. A **T2 producer does not** — it
 * sends whole-**panel** pixels, because a screen capture is the whole screen.
 *
 * Handing those straight through is what made the compose-under band show
 * launcher content at the wrong place and scale (#174: an avatar whose zone-3D
 * canvas is the bottom ~75 % of the panel got the entire launcher squeezed into
 * that band, shifted down 25 % and scaled to 0.75). The full-width case hides
 * the horizontal half of the error, which is why it read as a pure vertical
 * offset on the avatar and as "nothing obviously wrong" against T0's
 * x-constant gradient.
 *
 * So the **receiver crops**: @p canvas_on_panel is where this session's canvas
 * actually sits on the panel, in panel pixels, and the captured frame is cut
 * down to it before upload. Cropping here rather than in the DP keeps the slot
 * contract intact (one coordinate space, no producer-tier flag on the wire) and
 * matches Windows/Linux, where the capture module hands the DP a window sub-rect
 * rather than the DP reverse-engineering one.
 *
 * @param      mc     Session whose `session_render` owns the resources.
 * @param      vk     The compositor's Vulkan bundle.
 * @param      canvas_on_panel Canvas rect in panel pixels, or NULL / a
 *                    degenerate rect to use the frame whole (T0 always passes
 *                    NULL — a runtime-drawn backdrop is already canvas-space).
 * @param[out] out_w  Backdrop width in pixels (may be NULL).
 * @param[out] out_h  Backdrop height in pixels (may be NULL).
 */
VkImageView
comp_multi_bg2d_ensure(struct multi_compositor *mc,
                       struct vk_bundle *vk,
                       const struct xrt_rect *canvas_on_panel,
                       uint32_t *out_w,
                       uint32_t *out_h);

/*!
 * Release the backdrop image/memory/staging buffer. Idempotent; safe with a
 * NULL @p vk (matches the rest of the session_render teardown).
 */
void
comp_multi_bg2d_teardown(struct multi_compositor *mc, struct vk_bundle *vk);

#ifdef __cplusplus
}
#endif
