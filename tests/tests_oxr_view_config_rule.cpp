// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1486: the two xrEndFrame projection-layer viewCount rules.
 *
 * Driving the real entry point needs a session, a compositor and a SUBMITTED
 * FRAME — the headless suite (tests_oxr_view_space.cpp) has none of those, so
 * the rules themselves are pure functions in oxr_view_config_rule.h and that is
 * what is pinned here. Same "make the decision host-testable" shape as
 * tests_oxr_weave_latch.cpp and tests_cli_dims_check.cpp.
 *
 * #1528 added a third input to the tight rule — the view count of the mode
 * latched at xrBeginFrame — so that the one in-flight frame a 2D->3D switch
 * catches mid-render is granted rather than dropped. The decision stays a pure
 * function, so it stays pinned here.
 *
 * NOT pinned here (and there is no host-side way to pin it): the WIRING — that
 * verify_projection_view_count picks the permissive rule for
 * PRIMARY_MULTIVIEW_DXR and under DXR_VIEW_CONFIG_LEGACY=1, and the tight one
 * for PRIMARY_STEREO otherwise, and that oxr_session_frame_begin latches the
 * active mode's view count into sess->frame_begin_mode_view_count. That is on
 * the Windows hardware leg.
 */

#include "catch_amalgamated.hpp"

#include "oxr_view_config_rule.h"

namespace {

//! sim_display's modes: 2D(1) Anaglyph(2) Cropped SBS(2) Squeezed SBS(2) Quad(4).
//! Max 4, and no mode has 3 — which is what makes 3 a usable negative.
const uint32_t kSimDisplayModes[] = {1, 2, 2, 2, 4};
constexpr uint32_t kSimDisplayModeCount = 5;

//! A stereo-only panel (the Leia shape): nothing wider than 2 exists.
const uint32_t kStereoOnlyModes[] = {1, 2};
constexpr uint32_t kStereoOnlyModeCount = 2;

} // namespace

TEST_CASE("PRIMARY_STEREO is exactly 2, with ONE extension-scoped exception (#1486)", "[oxr][view_config_rule]")
{
	// The whole rule as a table, so the single hole is visible rather than
	// inferred: {submitted} x {active mode view count} x {mode latched at
	// xrBeginFrame} x {extension enabled}.
	//
	// Accepted iff submitted == 2, OR (submitted == 1 && ext && (active == 1 ||
	// begun == 1)) — #1528 widened the middle term from "active == 1" to "a
	// 1-view mode is in play", and nothing else moved.
	//
	// Every row where `begun == active` is the STEADY STATE (no mode switch in
	// flight) and is exactly the pre-#1528 rule, so the whole of the old matrix
	// is still asserted here; begun == 0 is "never latched", which must also
	// reproduce the old answer.
	SECTION("the full matrix")
	{
		const uint32_t submitted[] = {1, 2, 3, 4};
		const uint32_t active[] = {1, 2, 4};
		const uint32_t begun[] = {0, 1, 2, 4};

		for (uint32_t s : submitted) {
			for (uint32_t a : active) {
				for (uint32_t b : begun) {
					for (bool ext : {false, true}) {
						const bool expect = (s == 2) || (s == 1 && ext && (a == 1 || b == 1));
						INFO("submitted = "
						     << s << ", active mode = " << a << ", begun mode = " << b
						     << ", XR_DXR_display_info = " << (ext ? "on" : "off"));
						CHECK(oxr_view_count_ok_for_stereo(s, a, b, ext) == expect);
					}
				}
			}
		}
	}

	SECTION("the mode edge: the one in-flight frame a 2D->3D switch catches (#1528)")
	{
		// Measured on the win box: every 2D->2-view flip rejected exactly one
		// frame (4/4 crossings, Unity, and identically with the previous plugin
		// build as a control). The app began that frame while the mode was
		// 1-view and submitted it after the runtime had flipped the panel.

		// Begun 1-view, active now 2-view: the in-flight frame. GRANTED.
		CHECK(oxr_view_count_ok_for_stereo(1, 2, 1, true));
		CHECK(oxr_view_count_ok_for_stereo(1, 4, 1, true));

		// The very next frame was begun in the 2-view mode, so the grace is
		// spent: an app that keeps submitting 1 in 3D is still refused. This is
		// what makes the allowance exactly one frame wide.
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2, 2, true));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 4, 4, true));

		// The 3D->2D direction, which never failed and must not start: the mode
		// is already 1-view at end-frame even though the frame was begun in a
		// 2-view one.
		CHECK(oxr_view_count_ok_for_stereo(1, 1, 2, true));
		CHECK(oxr_view_count_ok_for_stereo(1, 1, 4, true));

		// The extension gate is untouched by all of it — a core-only app
		// (every CTS session) gets exact-2 at the mode edge too.
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2, 1, false));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 1, 2, false));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 1, 1, false));

		// Two is accepted across the edge in every combination, as always.
		for (uint32_t a : {1u, 2u, 4u}) {
			for (uint32_t b : {0u, 1u, 2u, 4u}) {
				for (bool ext : {false, true}) {
					INFO("active = " << a << ", begun = " << b << ", ext = " << ext);
					CHECK(oxr_view_count_ok_for_stereo(2, a, b, ext));
				}
			}
		}

		// ...and N-view is refused across the edge in every combination: the
		// grace is about the ONE-view submission only, never a back door into
		// the counts that need PRIMARY_MULTIVIEW_DXR.
		for (uint32_t s : {0u, 3u, 4u, 8u}) {
			for (uint32_t a : {1u, 2u, 4u}) {
				for (uint32_t b : {0u, 1u, 2u, 4u}) {
					for (bool ext : {false, true}) {
						INFO("submitted = " << s << ", active = " << a << ", begun = " << b
						                    << ", ext = " << ext);
						CHECK_FALSE(oxr_view_count_ok_for_stereo(s, a, b, ext));
					}
				}
			}
		}
	}

	SECTION("an unlatched begin-frame count changes nothing (#1528)")
	{
		// 0 = oxr_session_frame_begin could not read the mode (no head device,
		// no rendering modes, index out of range) or no frame has been begun.
		// It must contribute NOTHING: this is an additional allowance, never a
		// tightening, so the answer has to be the pre-#1528 one.
		for (uint32_t s : {0u, 1u, 2u, 3u, 4u}) {
			for (uint32_t a : {1u, 2u, 4u}) {
				for (bool ext : {false, true}) {
					INFO("submitted = " << s << ", active = " << a << ", ext = " << ext);
					const bool old_rule = (s == 2) || (s == 1 && a == 1 && ext);
					CHECK(oxr_view_count_ok_for_stereo(s, a, 0, ext) == old_rule);
				}
			}
		}
	}

	SECTION("a core-only app gets exact-2, whatever mode the panel is in")
	{
		// THE CTS PATH, and the reason the mode gate alone was not enough. A CTS
		// session never enables XR_DXR_display_info, and the win box's full-suite
		// run on 57b550cf7 showed sim-display sitting in a 1-view
		// (Passthrough/2D) mode for essentially the whole pass — so a mode-only
		// exception was open for the entire conformance run.
		//
		// test_XrCompositionLayerProjection.cpp:225-230 locates the views, does
		// `Layer.viewCount--` and CHECKs for XR_ERROR_VALIDATION_FAILURE.
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 1, 1, false));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2, 2, false));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 4, 4, false));

		// ...while the conformant count is always accepted.
		CHECK(oxr_view_count_ok_for_stereo(2, 1, 1, false));
		CHECK(oxr_view_count_ok_for_stereo(2, 2, 2, false));
		CHECK(oxr_view_count_ok_for_stereo(2, 4, 4, false));
	}

	SECTION("an extension app may submit 1 while the active mode is 1-view")
	{
		// Who actually submits one: the cube_* apps compute
		// `eyeCount = display3D ? modeViewCount : 1`, and displayxr-common
		// forwards the caller's count. They all enable XR_DXR_display_info.
		CHECK(oxr_view_count_ok_for_stereo(1, 1, 1, true));

		// But only while the mode really is 1-view — the extension is not a
		// blanket pass.
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2, 2, true));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 4, 4, true));

		// An app that keeps submitting the stereo pair in 2D is still fine —
		// the compositor has always accepted that, and the extension's
		// backward-compatibility clause promises it.
		CHECK(oxr_view_count_ok_for_stereo(2, 1, 1, true));
	}

	SECTION("N-view is refused under this type even with the extension on")
	{
		// The point of the whole change: a Quad-mode app on PRIMARY_STEREO does
		// not get to submit 4. It begins PRIMARY_MULTIVIEW_DXR instead — which
		// is also the recommended path for ANY mode-driven count, including the
		// 1-view case above.
		for (bool ext : {false, true}) {
			INFO("XR_DXR_display_info = " << (ext ? "on" : "off"));
			CHECK_FALSE(oxr_view_count_ok_for_stereo(4, 4, 4, ext));
			CHECK_FALSE(oxr_view_count_ok_for_stereo(4, 2, 2, ext));
			CHECK_FALSE(oxr_view_count_ok_for_stereo(3, 2, 2, ext));
			CHECK_FALSE(oxr_view_count_ok_for_stereo(8, 2, 2, ext));
			CHECK_FALSE(oxr_view_count_ok_for_stereo(0, 2, 2, ext));
		}
	}

	SECTION("unknown active mode falls back to the conformant answer")
	{
		// verify_projection_view_count passes 2 when it cannot read the active
		// mode, which makes the rule "exactly two" — never a loosening, even for
		// an extension app.
		CHECK(oxr_view_count_ok_for_stereo(2, 2, 2, true));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2, 2, true));
	}
}

TEST_CASE("the permissive rule takes any rendering mode's count (#1486)", "[oxr][view_config_rule]")
{
	// This is the body PRIMARY_MULTIVIEW_DXR gets, and the one
	// DXR_VIEW_CONFIG_LEGACY=1 restores for PRIMARY_STEREO.
	CHECK(oxr_view_count_ok_for_multiview(1, kSimDisplayModes, kSimDisplayModeCount));
	CHECK(oxr_view_count_ok_for_multiview(2, kSimDisplayModes, kSimDisplayModeCount));
	CHECK(oxr_view_count_ok_for_multiview(4, kSimDisplayModes, kSimDisplayModeCount));

	// No sim_display mode has 3 views, so 3 is still refused — "permissive" is
	// not "anything goes".
	CHECK_FALSE(oxr_view_count_ok_for_multiview(3, kSimDisplayModes, kSimDisplayModeCount));
	CHECK_FALSE(oxr_view_count_ok_for_multiview(8, kSimDisplayModes, kSimDisplayModeCount));
	CHECK_FALSE(oxr_view_count_ok_for_multiview(0, kSimDisplayModes, kSimDisplayModeCount));

	// 1 and 2 are accepted with no mode list at all: an app may be one frame
	// behind a mode transition, and a device that reports no modes must not
	// lose the ordinary stereo submission.
	CHECK(oxr_view_count_ok_for_multiview(1, nullptr, 0));
	CHECK(oxr_view_count_ok_for_multiview(2, nullptr, 0));
	CHECK_FALSE(oxr_view_count_ok_for_multiview(4, nullptr, 0));
	CHECK_FALSE(oxr_view_count_ok_for_multiview(4, kSimDisplayModes, 0));
}

TEST_CASE("neither rule can stand in for PRIMARY_MONO (#1486)", "[oxr][view_config_rule]")
{
	// PRIMARY_MONO's rule is "exactly 1" and it is NOT one of these two — it
	// stays inline in verify_projection_view_count's switch. What is pinnable
	// here is WHY that matters: both rules wave 2 through unconditionally, so
	// routing a mono session to either one would turn a
	// XR_ERROR_VALIDATION_FAILURE into a silent accept.
	//
	// That is exactly the hole the kill switch had before it grew its
	// `view_config_type != PRIMARY_MONO` guard: DXR_VIEW_CONFIG_LEGACY=1 only
	// ever restored the PRIMARY_STEREO mapping, never mono's.
	//
	// Note the stereo rule waves 2 through even when the ACTIVE mode is 1-view
	// and the extension is on, so neither "the active mode is mono" nor
	// "XR_DXR_display_info is enabled" is a back door into mono's rule.
	CHECK(oxr_view_count_ok_for_stereo(2, 2, 2, false));
	CHECK(oxr_view_count_ok_for_stereo(2, 1, 1, true));
	CHECK(oxr_view_count_ok_for_multiview(2, kSimDisplayModes, kSimDisplayModeCount));
	CHECK(oxr_view_count_ok_for_multiview(2, nullptr, 0));
}

TEST_CASE("where the two rules differ, stated honestly (#1486)", "[oxr][view_config_rule]")
{
	// This property used to read "the two rules are identical for every count
	// <= 3". That has been narrowed twice and saying it unqualified would hide
	// both fixes: the tight rule refuses ONE view in a >=2-view mode, and now
	// also refuses it for a core-only app in ANY mode.
	SECTION("identical for every count <= 3 only for an EXTENSION app in a 1-view mode")
	{
		// The 2D/mono case on a stereo-only panel: the kill switch and the new
		// rule agree, so no shipped Leia app in 2D is affected by either.
		for (uint32_t n = 0; n <= 3; n++) {
			INFO("viewCount = " << n);
			CHECK(oxr_view_count_ok_for_stereo(n, 1, 1, true) ==
			      oxr_view_count_ok_for_multiview(n, kStereoOnlyModes, kStereoOnlyModeCount));
		}
	}

	SECTION("a core-only app differs at ONE view in EVERY mode")
	{
		// The CTS fix (c'), stated as the difference it introduces: the
		// permissive rule still waves a short submission through, the tight one
		// does not — and for a core-only app that holds even in a 1-view mode,
		// which is where the conformance run actually sits.
		for (uint32_t a : {1u, 2u, 4u}) {
			INFO("active mode = " << a);
			CHECK_FALSE(oxr_view_count_ok_for_stereo(1, a, a, false));
		}
		CHECK(oxr_view_count_ok_for_multiview(1, kStereoOnlyModes, kStereoOnlyModeCount));
	}

	SECTION("an extension app differs at ONE view only in a >=2-view mode")
	{
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2, 2, true));
		CHECK(oxr_view_count_ok_for_multiview(1, kStereoOnlyModes, kStereoOnlyModeCount));

		// Two is the agreed answer either way.
		CHECK(oxr_view_count_ok_for_stereo(2, 2, 2, true));
		CHECK(oxr_view_count_ok_for_multiview(2, kStereoOnlyModes, kStereoOnlyModeCount));
	}

	SECTION("and on a device with a wider mode they differ at that count too")
	{
		CHECK_FALSE(oxr_view_count_ok_for_stereo(4, 4, 4, true));
		CHECK(oxr_view_count_ok_for_multiview(4, kSimDisplayModes, kSimDisplayModeCount));
	}
}

/*
 * ADR-041 (Model E). The two rules above are now the LEGACY arm of one rule:
 * the view count is fixed for the session's lifetime and the app submits it
 * whole, aliasing the tail the runtime does not read.
 */

TEST_CASE("DXR_UNDER_SUBMIT maps by clamping, never by falling back (ADR-041)", "[oxr][view_config_rule]")
{
	CHECK(oxr_under_submit_from_setting(0) == OXR_UNDER_SUBMIT_STRICT);
	CHECK(oxr_under_submit_from_setting(1) == OXR_UNDER_SUBMIT_COMPAT);
	CHECK(oxr_under_submit_from_setting(2) == OXR_UNDER_SUBMIT_LEGACY);

	// A typo must not land on the default silently — it clamps to an end.
	CHECK(oxr_under_submit_from_setting(-1) == OXR_UNDER_SUBMIT_STRICT);
	CHECK(oxr_under_submit_from_setting(99) == OXR_UNDER_SUBMIT_LEGACY);
}

TEST_CASE("ADR-041: every type submits the located count, at every knob value", "[oxr][view_config_rule]")
{
	// R under PRIMARY_STEREO is ALWAYS 2; under PRIMARY_MULTIVIEW_DXR it is the
	// device max (4 for sim_display, 2 for a stereo-only panel).
	const enum oxr_under_submit_mode knobs[] = {OXR_UNDER_SUBMIT_STRICT, OXR_UNDER_SUBMIT_COMPAT,
	                                            OXR_UNDER_SUBMIT_LEGACY};

	SECTION("submitting exactly R is always OK — the whole point of the model")
	{
		for (auto k : knobs) {
			INFO("knob = " << (int)k);
			// STEREO, R = 2, in a 1-view mode (the aliased tail) and in a
			// 2-view one.
			CHECK(oxr_projection_view_count_verdict(2, 2, false, 1, 0, true, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_OK);
			CHECK(oxr_projection_view_count_verdict(2, 2, false, 2, 0, true, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_OK);
			// ...and for a CORE-ONLY app, which never has the extension.
			CHECK(oxr_projection_view_count_verdict(2, 2, false, 1, 0, false, nullptr, 0, k) ==
			      OXR_VIEW_COUNT_OK);

			// MULTIVIEW_DXR, R = 4, whatever the active mode is.
			CHECK(oxr_projection_view_count_verdict(4, 4, true, 1, 0, true, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_OK);
			CHECK(oxr_projection_view_count_verdict(4, 4, true, 2, 0, true, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_OK);
		}
	}

	SECTION("MULTIVIEW under-submit dies at the default and lives only under the kill switch")
	{
		// The contradiction ADR-041 removes: a 4-view session submitting 2
		// because the panel happens to be in a stereo mode. Core OpenXR says
		// all located views must be supplied.
		CHECK(oxr_projection_view_count_verdict(2, 4, true, 2, 0, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_STRICT) == OXR_VIEW_COUNT_REJECT);
		CHECK(oxr_projection_view_count_verdict(2, 4, true, 2, 0, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_REJECT);
		CHECK(oxr_projection_view_count_verdict(2, 4, true, 2, 0, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_LEGACY) == OXR_VIEW_COUNT_OK);

		// One view is the same story.
		CHECK(oxr_projection_view_count_verdict(1, 4, true, 1, 0, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_REJECT);
		CHECK(oxr_projection_view_count_verdict(1, 4, true, 1, 0, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_LEGACY) == OXR_VIEW_COUNT_OK);
	}

	SECTION("the ONE compat arm: PRIMARY_STEREO + extension + 1-view mode, and it is marked")
	{
		// Released demos submit one view in 2D mode; the arm is what keeps
		// them running, and OK_DEPRECATED is what gets that fact into the log
		// exactly once instead of blessing it silently.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, 1, 0, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_OK_DEPRECATED);

		// Strict drops it, BY DEFINITION — that is what "0" means.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, 1, 0, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_STRICT) == OXR_VIEW_COUNT_REJECT);

		// Both halves of the #1486 gate survive into the compat arm.
		// No extension (every CTS session) -> reject whatever the mode.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, 1, 0, false, nullptr, 0, OXR_UNDER_SUBMIT_COMPAT) ==
		      OXR_VIEW_COUNT_REJECT);
		// Extension, but the active mode is not 1-view -> reject.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, 2, 0, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_REJECT);
	}

	SECTION("THE CTS PATH: viewCount-- under PRIMARY_STEREO is refused at every knob value")
	{
		// test_XrCompositionLayerProjection.cpp:225-230 locates the views, does
		// `Layer.viewCount--` and CHECKs for XR_ERROR_VALIDATION_FAILURE. A CTS
		// session never enables XR_DXR_display_info, so the compat arm — which
		// requires it — is unreachable there and the knob cannot open a hole.
		for (auto k : knobs) {
			INFO("knob = " << (int)k);
			CHECK(oxr_projection_view_count_verdict(1, 2, false, 1, 0, false, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_REJECT);
			CHECK(oxr_projection_view_count_verdict(1, 2, false, 2, 0, false, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_REJECT);
		}
	}

	SECTION("#1528 mode-edge grace survives ADR-041 — the in-flight 1-view frame is granted")
	{
		// The regression this pins: a 2D->3D switch lands mid-frame, so the app
		// began the frame under a 1-view mode, rendered the one view it was
		// told about, and by xrEndFrame the ACTIVE mode is already 2-view.
		// Judging only the live mode drops exactly one frame per crossing
		// (measured 4/4 on the win box with Unity, #1528). ADR-041 changed
		// which function decides; it must not change the verdict.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, /*active*/ 2, /*begun*/ 1, true, kSimDisplayModes,
		                                        kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_OK_DEPRECATED);
		// ...and it is ONE frame wide: once the 3D mode is established the
		// latch has been overwritten, so a steadily-1-view app is refused.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, /*active*/ 2, /*begun*/ 2, true, kSimDisplayModes,
		                                        kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_REJECT);
		// The grace is a COMPAT-arm allowance, never a hole in strict mode.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, /*active*/ 2, /*begun*/ 1, true, kSimDisplayModes,
		                                        kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_STRICT) == OXR_VIEW_COUNT_REJECT);
		// And it still requires the extension, so no CTS session can reach it.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, /*active*/ 2, /*begun*/ 1, false, kSimDisplayModes,
		                                        kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_REJECT);
	}

	SECTION("over-submitting past R is refused too — R is FIXED, not a floor")
	{
		CHECK(oxr_projection_view_count_verdict(4, 2, false, 4, 0, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_REJECT);
		CHECK(oxr_projection_view_count_verdict(3, 2, false, 2, 0, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_STRICT) == OXR_VIEW_COUNT_REJECT);
	}
}
