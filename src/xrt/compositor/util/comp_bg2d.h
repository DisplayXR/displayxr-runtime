// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  2D backdrop producer for compose-under transparency (#1073).
 * @author David Fattal
 * @ingroup comp_util
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
 *
 * ## Slot-16 lifetime (#1120)
 *
 * The view handed to `set_background_2d` is **borrowed**: the DP samples it in a
 * compose pass it submits itself and, since L11, does not wait on. So every
 * mutation of the image on this side is ordered against that in-flight read —
 * a write-after-read barrier for a re-upload, a queue drain before a destroy.
 * Two knobs exist around it, both off/inert by default:
 *
 *   `debug.dxr.bg2d.sync`   / `DXR_BG2D_SYNC`   0 → restore the pre-#1120 racy
 *                                               ordering on the same build (A/B).
 *   `debug.dxr.bg2d.jiggle` / `DXR_BG2D_JIGGLE` HZ → perturb the canvas rect at
 *                                               HZ so the re-crop + teardown +
 *                                               re-upload path runs every frame.
 */

#pragma once

#include "vk/vk_helpers.h"
#include "xrt/xrt_defines.h" // struct xrt_rect (the T2 canvas crop, #174)

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Everything one consumer needs to own a backdrop image.
 *
 * Deliberately compositor-agnostic: the backdrop is produced identically for
 * the out-of-process `comp_multi` session path and for the in-process
 * `comp_vk_native` one, and duplicating 600 lines of Vulkan upload to say so
 * twice would be the only difference between them. Zero-initialise it and hand
 * the same pointer to every call; @ref comp_bg2d_teardown returns it to that
 * state.
 */
struct comp_bg2d_state
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkBuffer staging_buffer;
	VkDeviceMemory staging_memory;
	uint32_t w, h;
	bool initialized;
	bool uploaded_once; //!< Has content, so a refresh transitions from SHADER_READ_ONLY.
	bool logged;        //!< One "backdrop uploaded" line per consumer, not one per frame.
	uint32_t seq;       //!< T2: delivery counter of the captured frame currently uploaded.

	//! #174 — a T2 producer sends whole-PANEL pixels but slot 16 promises the
	//! canvas, so the receiver crops. Scratch for the repack, owned here and
	//! freed by @ref comp_bg2d_teardown; unused by T0 (runtime-drawn, already
	//! canvas-space).
	uint8_t *crop_scratch;
	size_t crop_capacity;
	bool logged_crop; //!< One "cropped to the canvas" line per consumer.
	//! One "dropped a capture from the other orientation" line per stale
	//! episode — cleared again when a usable frame is accepted, so each
	//! rotation that outruns the producer is reported exactly once.
	bool logged_stale;

	//! Canvas rect the currently-uploaded backdrop was cropped for, and the
	//! panel extent that rect was expressed against. A T2 producer in `once`
	//! mode sends exactly ONE frame, and it usually lands before the app has
	//! submitted the zone layer that establishes the canvas — so "re-upload
	//! when a newer frame arrives" alone would freeze the very first,
	//! canvas-less mapping in place forever. The panel extent is part of the
	//! key because it is what a **device rotation** changes: the crop maps
	//! canvas→frame through it, so the same canvas rect on a rotated panel is
	//! a different crop (#1073 rotation follow-up).
	struct xrt_rect canvas_used;
	uint32_t panel_used_w, panel_used_h;
	bool have_canvas_used;
	bool failed; //!< Latched after a failed build, so we try once.

	//! T2 only. When the listener was armed, and whether we have already said
	//! that nobody ever called. The producer is a SEPARATE process that has to
	//! be started by hand (`scripts/android_bg_capture.sh`) until the vendor
	//! service auto-starts it, and a producer that is simply not running looks
	//! from in here EXACTLY like one that has not connected yet: the consumer
	//! listens forever, `comp_bg2d_ensure` returns NULL forever, and the only
	//! symptom is that transparent edges fringe again because there is no
	//! backdrop to compose under. That silence cost a debugging session on
	//! 2026-08-21, so it now announces itself once.
	uint64_t capture_wait_since_ns;
	bool logged_no_producer;
};


/*!
 * Where a whole-panel capture must be cut for a session's compose-under
 * backdrop, in panel pixels — the single authority both compositor paths use.
 *
 * The DP maps the backdrop with `bg_uv` (0,0)-(1,1) across **whatever region
 * `process_atlas` writes**, so the crop is a function of that region and
 * nothing else. That region is @p dp_canvas when the caller hands
 * `process_atlas` a non-degenerate canvas sub-rect (the out-of-process
 * `comp_multi` path passes the frame's zone-3D rect), and the **whole client
 * window** when it hands it the degenerate rect (the in-process
 * `comp_vk_native` path always does: a zones frame's rects drive the lens mask,
 * not the weave output rect, so the DP fills the target).
 *
 * Deriving the crop from anything else — the zone rect on a path that does not
 * pass it down — squeezes the whole panel into a band: the backdrop then reads
 * as a `panel_h / canvas_h` vertical stretch, exact at the canvas' far edge and
 * drifting linearly toward its near one (#1101; measured 1.333x on a 1600x2560
 * panel under a bottom-75% zone).
 *
 * @param      window_on_panel Client window rect in panel pixels.
 * @param      dp_canvas       Canvas sub-rect handed to `process_atlas`,
 *                             window-relative. A degenerate (zero-extent) rect
 *                             means "the DP fills the whole target".
 * @param[out] out_rect        Crop rect in panel pixels.
 * @return false when @p window_on_panel is degenerate and no crop can be
 *         derived; the caller then supplies no backdrop rather than a
 *         mis-registered one.
 */
bool
comp_bg2d_backdrop_source_rect(const struct xrt_rect *window_on_panel,
                               const struct xrt_rect *dp_canvas,
                               struct xrt_rect *out_rect);

/*!
 * Is a runtime-supplied backdrop configured at all?
 *
 * Parsed once per process from `debug.dxr.bg2d` / `DXR_BG2D`. Cheap enough to
 * call per frame; returns false (and logs nothing) when unset, which is the
 * shipping default.
 */
bool
comp_bg2d_enabled(void);

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
 * @param      st     Caller-owned state; zero-initialised on first use.
 * @param      vk     The compositor's Vulkan bundle.
 * @param      cmd_pool Pool for the one-shot upload command buffer.
 * @param      canvas_on_panel Canvas rect in panel pixels, or NULL / a
 *                    degenerate rect to use the frame whole (T0 always passes
 *                    NULL — a runtime-drawn backdrop is already canvas-space).
 * @param      panel_w, panel_h Panel extent @p canvas_on_panel is expressed
 *                    against, i.e. the panel as it is NOW. The frame's own
 *                    space is the panel as it was when the shot was taken,
 *                    which the producer states from protocol v2 on; the two
 *                    part company across a device rotation and a frame from the
 *                    other orientation is dropped rather than mapped (#1073).
 * @param[out] out_w  Backdrop width in pixels (may be NULL).
 * @param[out] out_h  Backdrop height in pixels (may be NULL).
 */
VkImageView
comp_bg2d_ensure(struct comp_bg2d_state *st,
                 struct vk_bundle *vk,
                 VkCommandPool cmd_pool,
                 const struct xrt_rect *canvas_on_panel,
                 uint32_t panel_w,
                 uint32_t panel_h,
                 uint32_t *out_w,
                 uint32_t *out_h);

/*!
 * Release the backdrop image/memory/staging buffer. Idempotent; safe with a
 * NULL @p vk (matches the rest of the session_render teardown).
 */
void
comp_bg2d_teardown(struct comp_bg2d_state *st, struct vk_bundle *vk);

#ifdef __cplusplus
}
#endif
