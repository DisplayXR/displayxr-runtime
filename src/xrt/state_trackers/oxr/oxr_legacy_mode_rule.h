// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1510: which rendering mode a LEGACY session runs in, and the
 *         compromise view scale that follows from it.
 *
 * A legacy app is one that did not enable `XR_DXR_display_info`. It cannot
 * enumerate rendering modes, cannot request one, and is never told the active
 * one changed — so every mode-shaped decision on its behalf is the runtime's.
 * It also submits a FIXED two-view stereo frame (post-#1486 `PRIMARY_STEREO`
 * reports exactly 2 views, and the shipping legacy apps were stereo-fixed
 * before that).
 *
 * ### The defect
 *
 * `oxr_system_fill_in()` read the active mode and split on
 * "2 views and both scales <= 0.5" (Case A, the SBS compromise 0.5x1.0) versus
 * "use the mode's own scale" (Case B). A default mode with MORE than two views
 * — sim-display's Quad, 4 views, 0.5x0.5, a 2x2 grid — failed Case A's
 * `== 2` and fell into Case B, so the legacy app was sized for 0.5x0.5 tiles,
 * painted the first two of four, and left the bottom half of the canvas at the
 * clear colour. Measured on the win box with `SIM_DISPLAY_FORCE_MODE=4`.
 *
 * ### Why the fix is a MODE FLOOR and not a wider Case A
 *
 * The tempting change is "Case A for any `view_count >= 2`", i.e. give the
 * legacy app 0.5x1.0 and lay its two views out 2x1 inside a 2x2 mode. That
 * cannot work, and the reason is the display processor, not the compositor:
 *
 *   - The active mode is a COMPLETE recipe — layout, view count, scales and the
 *     hardware state the DP is in. `compute_effective_layout()` in every
 *     backend states the contract as "the content recipe is the ACTIVE MODE's —
 *     submissions are clamped to it, never the other way round", and the DP is
 *     handed that same grid. A 2x1 atlas delivered to a DP whose pipeline is a
 *     4-view weave does not fill the canvas; it de-tiles the wrong stride and
 *     produces geometric corruption, which is strictly worse than two clean
 *     unpainted quadrants.
 *   - It would also have to be done identically in five backends (d3d11, d3d12,
 *     gl, vk_native, metal) or the same app would look different per graphics
 *     API.
 *
 * So the rule is instead: **a legacy session does not run in a mode it cannot
 * fill.** A mode is fillable by a legacy session when its `view_count` is at
 * most two; otherwise the runtime picks the best fillable mode (preferring a
 * two-view 3D mode, falling back to 2D) and moves the display there BEFORE the
 * app is sized, so the mode, the compromise scale, the compositor grid and the
 * DP all agree. That is the #1499 direction — mirroring the existing rule that
 * a legacy app gets the V toggle but never the 1/2/3 direct mode keys.
 *
 * ### When the floor cannot be applied
 *
 * The device owns its mode and may PIN it
 * (`XRT_DEVICE_PROPERTY_OUTPUT_MODE_PINNED` — sim-display's
 * `SIM_DISPLAY_FORCE_MODE`). A dev pin deliberately outranks the runtime: its
 * whole purpose is to hold a mode against every later request, including this
 * one, so that the N-view under-submit path stays testable. In service mode the
 * panel lease, not the app, owns the mode. In both cases the floor is NOT
 * applied, the legacy app keeps Case B, and the runtime says so once, loudly —
 * the issue's objection was that the capability loss was SILENT, not that it
 * happened.
 *
 * Pure integer/float decisions with no runtime dependency, so they are pinned
 * on the host (tests/tests_oxr_legacy_mode_rule.cpp). Same shape as
 * @ref oxr_view_config_rule.h and @ref oxr_weave_latch.h.
 *
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_device.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The most views a legacy session can ever submit. Post-#1486
 * XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO reports exactly two, and a legacy
 * app has no way to begin any other primary view configuration.
 */
#define OXR_LEGACY_MAX_SUBMITTED_VIEWS 2u

/*!
 * Does a legacy instance get compromise tile scaling at all?
 *
 * The call site's condition, so that "a single-mode device gets no compromise"
 * is pinned rather than implied: with one mode there is nothing to compromise
 * BETWEEN, the compositor's recommended scale is already the only right answer,
 * and `legacy_app_tile_scaling` stays false (which also leaves the V toggle and
 * the 1/2/3 keys behaving normally).
 *
 * When this is true, `legacy_app_tile_scaling` is raised for BOTH compromise
 * cases — Case A and Case B alike. The flag means "the runtime, not the mode,
 * owns this app's view dimensions", which is equally true either way.
 */
static inline bool
oxr_legacy_tile_scaling_applies(uint32_t rendering_mode_count)
{
	return rendering_mode_count > 1;
}

/*!
 * May the runtime move the display out of a mode the legacy app cannot fill?
 *
 * @param mode_pinned   The device answered XRT_DEVICE_PROPERTY_OUTPUT_MODE_PINNED.
 *                      A dev pin (SIM_DISPLAY_FORCE_MODE) exists precisely to
 *                      hold a mode against every later request — including this
 *                      one, which is what keeps the N-view under-submit path
 *                      testable at all.
 * @param service_mode  The panel lease, not this app, owns the display-global
 *                      mode; a legacy client must not yank it from under a
 *                      workspace controller or another client.
 */
static inline bool
oxr_legacy_may_demote(bool mode_pinned, bool service_mode)
{
	return !mode_pinned && !service_mode;
}

/*!
 * Can a legacy session's two-view submission fill this mode's canvas?
 *
 * Exactly the question "does the mode's tile grid hold more tiles than the app
 * can paint?". Expressed on `view_count` rather than `tile_columns *
 * tile_rows`: those are the same number for every well-formed mode, and
 * `view_count` is the field the clamp in `compute_effective_layout()` compares
 * the submission against.
 */
static inline bool
oxr_legacy_mode_is_fillable(const struct xrt_rendering_mode *mode)
{
	return mode != NULL && mode->view_count <= OXR_LEGACY_MAX_SUBMITTED_VIEWS;
}

/*!
 * The mode a legacy session should run in.
 *
 * Returns @p active_index untouched whenever the active mode is fillable —
 * which is every shipping configuration today, because the Leia plug-in's modes
 * are all 1- or 2-view. Otherwise:
 *
 *   1. the first 3D mode with exactly two views (keep stereo), else
 *   2. the first 3D mode that is fillable at all, else
 *   3. mode 0 — 2D, and the app renders mono over the whole canvas.
 *
 * @param modes        The device's rendering-mode table; may be NULL.
 * @param mode_count   Entries in @p modes.
 * @param active_index The device's current active mode index.
 */
static inline uint32_t
oxr_legacy_pick_mode_index(const struct xrt_rendering_mode *modes, uint32_t mode_count, uint32_t active_index)
{
	if (modes == NULL || mode_count == 0 || active_index >= mode_count) {
		return active_index;
	}
	if (oxr_legacy_mode_is_fillable(&modes[active_index])) {
		return active_index;
	}

	for (uint32_t i = 0; i < mode_count; i++) {
		if (modes[i].hardware_display_3d && modes[i].view_count == OXR_LEGACY_MAX_SUBMITTED_VIEWS) {
			return i;
		}
	}
	for (uint32_t i = 0; i < mode_count; i++) {
		if (modes[i].hardware_display_3d && oxr_legacy_mode_is_fillable(&modes[i])) {
			return i;
		}
	}
	return 0;
}

/*!
 * The compromise view scale a legacy app is sized with, for the mode it will
 * actually run in (i.e. the one @ref oxr_legacy_pick_mode_index returned).
 *
 * Case A — two views, both scales at most one half: the classic side-by-side
 * panel. The app is given 0.5x1.0, which fills the canvas on the mode's 2x1
 * grid and keeps full vertical resolution for 2D.
 *
 * Case B — everything else: the mode's own scale, which is already what the
 * grid wants (mono 1.0x1.0 on a 1x1 grid; a squeezed-SBS 0.5x1.0 on 2x1).
 *
 * Case A is deliberately NOT widened to `view_count >= 2`. Widening it would
 * hand a >2-view mode a scale that only makes sense on a 2x1 grid, and the
 * compositor and DP would still be on the mode's own grid — see the file
 * comment. A >2-view mode reaches this function only when the floor could not
 * be applied, and there Case B (the honest under-submit) is the correct answer.
 *
 * @param      mode  The mode the session will run in; may be NULL (no change).
 * @param[out] out_x Compromise horizontal view scale.
 * @param[out] out_y Compromise vertical view scale.
 * @return true when Case A applied.
 */
static inline bool
oxr_legacy_compromise_scale(const struct xrt_rendering_mode *mode, float *out_x, float *out_y)
{
	if (mode == NULL || out_x == NULL || out_y == NULL) {
		return false;
	}
	if (mode->view_count == OXR_LEGACY_MAX_SUBMITTED_VIEWS && mode->view_scale_x <= 0.5f &&
	    mode->view_scale_y <= 0.5f) {
		*out_x = 0.5f;
		*out_y = 1.0f;
		return true;
	}
	*out_x = mode->view_scale_x;
	*out_y = mode->view_scale_y;
	return false;
}

#ifdef __cplusplus
}
#endif
