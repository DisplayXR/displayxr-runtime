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
 *   - PRIMARY_STEREO is TIGHT: for a CORE-ONLY app it is exactly 2, always.
 *
 *     One view is legal only for an app that enabled XR_DXR_display_info AND
 *     only while a 1-view rendering mode is in play — either the mode active
 *     NOW, or the one the frame was BEGUN under (#1528, below). Both halves are
 *     load-bearing:
 *
 *       * The MODE half is who submits one — the cube_* apps compute
 *         `eyeCount = display3D ? modeViewCount : 1`, and displayxr-common
 *         forwards the caller's count.
 *       * The EXTENSION half is what keeps the CTS honest. A core-only app has
 *         no notion of a "rendering mode" at all: it cannot enumerate one,
 *         cannot request one, and is never told the active one changed. Scoping
 *         a relaxation to a fact such an app cannot observe would make
 *         PRIMARY_STEREO's meaning depend on hidden runtime state.
 *
 *     #1528, the MODE-EDGE GRACE: the mode half reads the mode latched at
 *     xrBeginFrame as well as the live one, because a 2D->3D switch lands
 *     mid-frame. The app begins a frame while the mode is 1-view, renders the
 *     one view it was told about, and by the time it calls xrEndFrame the
 *     runtime has flipped the panel to 2-view — so judging only the live mode
 *     rejected exactly one frame per crossing (measured 4/4 on the win box with
 *     Unity, and identically with the previous plugin build). The RUNTIME owns
 *     the switch; making every provider re-render the in-flight frame would push
 *     a runtime race onto every consumer, so the runtime owns the grace. It is
 *     one frame wide by construction: the latch is overwritten at the next
 *     xrBeginFrame, so a steadily-3D app that keeps submitting 1 is still
 *     refused from its second frame on.
 *
 *     `XrCompositionLayerProjection`
 *     (conformance_test/test_XrCompositionLayerProjection.cpp:225-230) locates
 *     the views, does `Layer.viewCount--` and CHECKs for
 *     XR_ERROR_VALIDATION_FAILURE. Under PRIMARY_STEREO that decrement is
 *     2 -> 1. Gating on the mode alone was NOT enough and the win box proved it
 *     on the full suite: a CTS session never enables XR_DXR_display_info, so the
 *     runtime treats it as a legacy session and sim-display sits in a 1-view
 *     (Passthrough/2D) mode for essentially the whole run — the mode-only
 *     exception was open for the entire conformance pass. With the extension
 *     gate the CTS is always on the exact-2 path.
 *
 *     An extension app that wants mode-driven counts should begin
 *     PRIMARY_MULTIVIEW_DXR; the 1-view allowance here is a narrow
 *     back-compatibility relaxation, not the recommended path.
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
 * ADR-041 (Model E) puts a THIRD decision in front of both of those. The view
 * count a session sees is FIXED for its lifetime (what the begun view
 * configuration reports, R); what varies per frame is how many of those views
 * are ACTIVE (A, the active rendering mode's count clamped to R, published to
 * the app as XrViewActivityStateDXR::activeViewCount). The app therefore
 * submits exactly R views always, aliasing the inactive tail [A, R) onto any
 * subimage it rendered this frame — which is what core OpenXR already demands
 * ("XrCompositionLayerProjection::viewCount must be equal to the number of view
 * poses returned by xrLocateViews", and "All views associated with projection
 * layers must be supplied, or XR_ERROR_VALIDATION_FAILURE must be returned by
 * xrEndFrame"). Under-submitting to PRIMARY_MULTIVIEW_DXR contradicted both
 * sentences, so it goes.
 *
 * @ref oxr_projection_view_count_verdict is that rule, parameterised on the
 * DXR_UNDER_SUBMIT switch so the removal is staged rather than abrupt:
 *
 *   | DXR_UNDER_SUBMIT | PRIMARY_STEREO          | PRIMARY_MULTIVIEW_DXR   |
 *   |------------------|-------------------------|-------------------------|
 *   | 0 strict         | == R (2)                | == R (device max)       |
 *   | 1 DEFAULT        | == R, or 1 (DEPRECATED, | == R, or 1 (DEPRECATED, |
 *   |                  | ext + a 1-view mode in   | ext + a 1-view mode in   |
 *   |                  | play: active OR begun)   | play: active OR begun)   |
 *   | 2 kill switch    | pre-ADR-041 tight rule  | pre-ADR-041 permissive  |
 *
 * The 1-view arm survives at the default ONLY because RELEASED demos submit one
 * view in 2D mode — under either view configuration, since those demos moved to
 * PRIMARY_MULTIVIEW_DXR before ADR-041 landed (#1612). It is the SAME arm for
 * both types: exactly one view, the extension enabled, and a 1-view mode in
 * play. Any other under-submission (2 of 4 in a stereo mode, 1 in a 3D mode)
 * is refused at the default. It answers OXR_VIEW_COUNT_OK_DEPRECATED so the
 * caller can log it once per session. It is unreachable from a CTS run either
 * way — a conformance session never enables XR_DXR_display_info, which the arm
 * requires. The window closes once every demo aliases its inactive views.
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
#include <stddef.h> // NULL — this header must stand alone, not lean on its includers.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The TIGHT rule: XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO accepts exactly two
 * views — and a single view only for an XR_DXR_display_info app for which a
 * 1-view rendering mode is in play, which is the 2D/mono submission path.
 *
 * "In play" is EITHER the mode active now OR the mode latched when the app
 * called xrBeginFrame for this frame (#1528): the app renders the frame it was
 * told about at begin, and a mode switch that lands in between is the runtime's
 * doing, not the app's.
 *
 * @param submitted               The layer's viewCount.
 * @param active_mode_view_count  Views in the currently active rendering mode.
 *                                Callers that cannot determine it pass 2, the
 *                                stereo default, which makes the rule "exactly
 *                                two".
 * @param begun_mode_view_count   Views in the mode that was active at this
 *                                frame's xrBeginFrame. 0 = not latched (no
 *                                xrBeginFrame seen, or the mode was unreadable
 *                                then), which contributes nothing — the rule is
 *                                then exactly what it was before #1528.
 * @param display_info_enabled    Did the INSTANCE enable XR_DXR_display_info?
 *                                False for every core-only app, including every
 *                                CTS session — for those the answer is exactly
 *                                2 whatever mode the panel is in.
 */
static inline bool
oxr_view_count_ok_for_stereo(uint32_t submitted,
                             uint32_t active_mode_view_count,
                             uint32_t begun_mode_view_count,
                             bool display_info_enabled)
{
	if (submitted == 2) {
		return true;
	}
	if (submitted != 1 || !display_info_enabled) {
		return false;
	}
	return active_mode_view_count == 1 || begun_mode_view_count == 1;
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

/*!
 * ADR-041 staging switch: how much less than the located view count xrEndFrame
 * still accepts. Latched from DXR_UNDER_SUBMIT.
 */
enum oxr_under_submit_mode
{
	//! Every type submits exactly the located count. No relaxation at all.
	OXR_UNDER_SUBMIT_STRICT = 0,
	//! DEFAULT: as STRICT, plus the deprecated 1-view-in-a-1-view-mode arm
	//! (both PRIMARY_STEREO and PRIMARY_MULTIVIEW_DXR, #1612).
	OXR_UNDER_SUBMIT_COMPAT = 1,
	//! Kill switch: the pre-ADR-041 rules, including MULTIVIEW under-submit.
	OXR_UNDER_SUBMIT_LEGACY = 2,
};

/*!
 * The answer, with "accepted but on the way out" kept distinct from "accepted"
 * so the caller can warn exactly once instead of silently blessing it.
 */
enum oxr_view_count_verdict
{
	OXR_VIEW_COUNT_OK = 0,
	OXR_VIEW_COUNT_OK_DEPRECATED = 1,
	OXR_VIEW_COUNT_REJECT = 2,
};

/*!
 * Map the raw DXR_UNDER_SUBMIT value onto the switch. Out-of-range clamps
 * rather than falls back to the default, so DXR_UNDER_SUBMIT=99 is the kill
 * switch and DXR_UNDER_SUBMIT=-1 is strict — a typo can never silently land on
 * "whatever the default was".
 *
 * Pure, so the env plumbing (DEBUG_GET_ONCE_NUM_OPTION, which caches per
 * process) is not what the host tests have to drive.
 */
static inline enum oxr_under_submit_mode
oxr_under_submit_from_setting(long value)
{
	if (value <= 0) {
		return OXR_UNDER_SUBMIT_STRICT;
	}
	if (value >= 2) {
		return OXR_UNDER_SUBMIT_LEGACY;
	}
	return OXR_UNDER_SUBMIT_COMPAT;
}

/*!
 * ADR-041: the projection-layer viewCount rule for the two view configuration
 * types whose count is not a constant. PRIMARY_MONO / QUAD_VARJO / the MSFT
 * secondary observer keep their fixed core counts and never reach here, and
 * neither does the DXR_VIEW_CONFIG_LEGACY (#1486) rollback, which is a separate
 * switch that restores the permissive rule wholesale.
 *
 * @param submitted               The layer's viewCount.
 * @param reported                R — what xrLocateViews returned this session:
 *                                2 under PRIMARY_STEREO, the device max under
 *                                PRIMARY_MULTIVIEW_DXR.
 * @param is_multiview            Was the session begun with
 *                                PRIMARY_MULTIVIEW_DXR?
 * @param begun_mode_view_count   Views in the mode latched at this frame's
 *                                xrBeginFrame; 0 = unknown (#1528).
 * @param active_mode_view_count  A — views in the currently active rendering
 *                                mode. Callers that cannot determine it pass 2,
 *                                which never loosens anything.
 * @param display_info_enabled    Did the INSTANCE enable XR_DXR_display_info?
 *                                False for every core-only app, including every
 *                                CTS session.
 * @param mode_view_counts        One entry per rendering mode; may be NULL.
 *                                Only read under OXR_UNDER_SUBMIT_LEGACY.
 * @param mode_count              Entries in @p mode_view_counts.
 * @param under_submit            The staging switch.
 */
static inline enum oxr_view_count_verdict
oxr_projection_view_count_verdict(uint32_t submitted,
                                  uint32_t reported,
                                  bool is_multiview,
                                  uint32_t active_mode_view_count,
                                  uint32_t begun_mode_view_count,
                                  bool display_info_enabled,
                                  const uint32_t *mode_view_counts,
                                  uint32_t mode_count,
                                  enum oxr_under_submit_mode under_submit)
{
	if (under_submit == OXR_UNDER_SUBMIT_LEGACY) {
		// Exactly what shipped before ADR-041.
		const bool ok = is_multiview ? oxr_view_count_ok_for_multiview(submitted, mode_view_counts, mode_count)
		                             : oxr_view_count_ok_for_stereo(submitted, active_mode_view_count,
		                                                            begun_mode_view_count,
		                                                            display_info_enabled);
		return ok ? OXR_VIEW_COUNT_OK : OXR_VIEW_COUNT_REJECT;
	}

	// The core rule, and it is the same sentence for both types: submit what
	// xrLocateViews handed you.
	if (reported > 0 && submitted == reported) {
		return OXR_VIEW_COUNT_OK;
	}

	// The one compat arm, identical for both types. Released demos submit a
	// single view in 2D mode, and they were moved to PRIMARY_MULTIVIEW_DXR
	// before ADR-041 landed, so a STEREO-only arm froze their 2D frames (every
	// one rejected, the last 3D weave left on the panel — #1612). Scoped to
	// exactly that case: ONE view, the extension on, a 1-view mode in play.
	// A MULTIVIEW session submitting 2 of 4 in a stereo mode, or 1 in a 3D
	// mode, is still refused here — nothing released does that.
	//
	// #1528's mode-edge grace applies HERE too, not just on the legacy path: a
	// released 1-view app is exactly the app a 2D->3D switch can catch in
	// flight, and ADR-041 changed which function decides, not whose fault the
	// race is. Judging only the LIVE mode would drop one frame per crossing
	// again — the regression #1528 measured 4/4 on the win box with Unity.
	(void)is_multiview; // Same arm for both types; only the LEGACY path splits.
	if (under_submit == OXR_UNDER_SUBMIT_COMPAT && submitted == 1 && display_info_enabled &&
	    (active_mode_view_count == 1 || begun_mode_view_count == 1)) {
		return OXR_VIEW_COUNT_OK_DEPRECATED;
	}

	return OXR_VIEW_COUNT_REJECT;
}

#ifdef __cplusplus
}
#endif
