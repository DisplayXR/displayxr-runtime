// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The #1486 projection-layer viewCount rules.
 *
 * xrEndFrame accepts or refuses a projection layer's @c viewCount according to
 * the primary view configuration the session was begun with. #1486 split that
 * into two rules where there used to be one:
 *
 *   - PRIMARY_STEREO is TIGHT. The type means exactly two views, so a wider
 *     submission is refused instead of being waved through because some
 *     rendering mode happened to have that count. One view is legal ONLY in a
 *     1-view (2D/mono) rendering mode — which is exactly who submits one today:
 *     the cube_* apps compute `eyeCount = display3D ? modeViewCount : 1`, and
 *     displayxr-common forwards the caller's count. In a 2-view mode the answer
 *     is 2, full stop.
 *
 *     That last clause is what the CTS asserts. `XrCompositionLayerProjection`
 *     (conformance_test/test_XrCompositionLayerProjection.cpp:225-230) locates
 *     the views, does `Layer.viewCount--` and CHECKs for
 *     XR_ERROR_VALIDATION_FAILURE. Under PRIMARY_STEREO that decrement is
 *     2 -> 1, and the CTS runs in a 2-view mode, so accepting 1 unconditionally
 *     turned a required rejection into a success.
 *   - PRIMARY_MULTIVIEW_DXR is PERMISSIVE — 1, 2, or any rendering mode's view
 *     count, with no reference to the ACTIVE mode. It cannot be narrowed to the
 *     active mode, because the app may be one frame behind a mode transition
 *     (the race between the mode change and xrEndFrame), and that race is not
 *     the app's fault.
 *
 * The permissive rule is also what @c DXR_VIEW_CONFIG_LEGACY=1 restores for
 * PRIMARY_STEREO: under the kill switch that type reports the device MAX again,
 * so refusing the layer the app was told to build would make the rollback
 * incomplete.
 *
 * Both are pure integer decisions with no runtime dependency, so they are pinned
 * on the host (tests/tests_oxr_view_config_rule.cpp) — the real entry point
 * needs a session, a compositor and a submitted frame, none of which a headless
 * CI runner has. Same shape as @ref oxr_weave_latch.h, the ADR-027 tier-1
 * dispatch and the mini-window tell.
 *
 * @ingroup oxr_main
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The TIGHT rule: XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO accepts exactly two
 * views — and a single view only when the ACTIVE rendering mode is itself
 * 1-view, which is the 2D/mono submission path.
 *
 * @param submitted               The layer's viewCount.
 * @param active_mode_view_count  Views in the currently active rendering mode.
 *                                Callers that cannot determine it pass 2, the
 *                                stereo default, which makes the rule "exactly
 *                                two".
 */
static inline bool
oxr_view_count_ok_for_stereo(uint32_t submitted, uint32_t active_mode_view_count)
{
	if (submitted == 2) {
		return true;
	}
	return submitted == 1 && active_mode_view_count == 1;
}

/*!
 * The PERMISSIVE rule: 1, 2, or any rendering mode's view count — deliberately
 * NOT a function of the active mode (see the file comment).
 *
 * Used by XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR, and by the
 * DXR_VIEW_CONFIG_LEGACY rollback for PRIMARY_STEREO.
 *
 * @param submitted         The layer's viewCount.
 * @param mode_view_counts  One entry per rendering mode; may be NULL.
 * @param mode_count        Entries in @p mode_view_counts.
 */
static inline bool
oxr_view_count_ok_for_multiview(uint32_t submitted, const uint32_t *mode_view_counts, uint32_t mode_count)
{
	if (submitted == 1 || submitted == 2) {
		return true;
	}
	if (mode_view_counts == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < mode_count; i++) {
		if (submitted == mode_view_counts[i]) {
			return true;
		}
	}
	return false;
}

#ifdef __cplusplus
}
#endif
