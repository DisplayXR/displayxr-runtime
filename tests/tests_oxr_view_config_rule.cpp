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
 * NOT pinned here (and there is no host-side way to pin it): the WIRING — that
 * verify_projection_view_count picks the permissive rule for
 * PRIMARY_MULTIVIEW_DXR and under DXR_VIEW_CONFIG_LEGACY=1, and the tight one
 * for PRIMARY_STEREO otherwise. That is on the Windows hardware leg.
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
	// inferred: {submitted} x {active mode view count} x {extension enabled}.
	// Accepted iff submitted == 2, OR (submitted == 1 && active == 1 && ext).
	SECTION("the full matrix")
	{
		const uint32_t submitted[] = {1, 2, 3, 4};
		const uint32_t active[] = {1, 2, 4};

		for (uint32_t s : submitted) {
			for (uint32_t a : active) {
				for (bool ext : {false, true}) {
					const bool expect = (s == 2) || (s == 1 && a == 1 && ext);
					INFO("submitted = " << s << ", active mode = " << a
					                    << ", XR_DXR_display_info = " << (ext ? "on" : "off"));
					CHECK(oxr_view_count_ok_for_stereo(s, a, ext) == expect);
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
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 1, false));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2, false));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 4, false));

		// ...while the conformant count is always accepted.
		CHECK(oxr_view_count_ok_for_stereo(2, 1, false));
		CHECK(oxr_view_count_ok_for_stereo(2, 2, false));
		CHECK(oxr_view_count_ok_for_stereo(2, 4, false));
	}

	SECTION("an extension app may submit 1 while the active mode is 1-view")
	{
		// Who actually submits one: the cube_* apps compute
		// `eyeCount = display3D ? modeViewCount : 1`, and displayxr-common
		// forwards the caller's count. They all enable XR_DXR_display_info.
		CHECK(oxr_view_count_ok_for_stereo(1, 1, true));

		// But only while the mode really is 1-view — the extension is not a
		// blanket pass.
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2, true));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 4, true));

		// An app that keeps submitting the stereo pair in 2D is still fine —
		// the compositor has always accepted that, and the extension's
		// backward-compatibility clause promises it.
		CHECK(oxr_view_count_ok_for_stereo(2, 1, true));
	}

	SECTION("N-view is refused under this type even with the extension on")
	{
		// The point of the whole change: a Quad-mode app on PRIMARY_STEREO does
		// not get to submit 4. It begins PRIMARY_MULTIVIEW_DXR instead — which
		// is also the recommended path for ANY mode-driven count, including the
		// 1-view case above.
		for (bool ext : {false, true}) {
			INFO("XR_DXR_display_info = " << (ext ? "on" : "off"));
			CHECK_FALSE(oxr_view_count_ok_for_stereo(4, 4, ext));
			CHECK_FALSE(oxr_view_count_ok_for_stereo(4, 2, ext));
			CHECK_FALSE(oxr_view_count_ok_for_stereo(3, 2, ext));
			CHECK_FALSE(oxr_view_count_ok_for_stereo(8, 2, ext));
			CHECK_FALSE(oxr_view_count_ok_for_stereo(0, 2, ext));
		}
	}

	SECTION("unknown active mode falls back to the conformant answer")
	{
		// verify_projection_view_count passes 2 when it cannot read the active
		// mode, which makes the rule "exactly two" — never a loosening, even for
		// an extension app.
		CHECK(oxr_view_count_ok_for_stereo(2, 2, true));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2, true));
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
	CHECK(oxr_view_count_ok_for_stereo(2, 2, false));
	CHECK(oxr_view_count_ok_for_stereo(2, 1, true));
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
			CHECK(oxr_view_count_ok_for_stereo(n, 1, true) ==
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
			CHECK_FALSE(oxr_view_count_ok_for_stereo(1, a, false));
		}
		CHECK(oxr_view_count_ok_for_multiview(1, kStereoOnlyModes, kStereoOnlyModeCount));
	}

	SECTION("an extension app differs at ONE view only in a >=2-view mode")
	{
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2, true));
		CHECK(oxr_view_count_ok_for_multiview(1, kStereoOnlyModes, kStereoOnlyModeCount));

		// Two is the agreed answer either way.
		CHECK(oxr_view_count_ok_for_stereo(2, 2, true));
		CHECK(oxr_view_count_ok_for_multiview(2, kStereoOnlyModes, kStereoOnlyModeCount));
	}

	SECTION("and on a device with a wider mode they differ at that count too")
	{
		CHECK_FALSE(oxr_view_count_ok_for_stereo(4, 4, true));
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
			CHECK(oxr_projection_view_count_verdict(2, 2, false, 1, true, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_OK);
			CHECK(oxr_projection_view_count_verdict(2, 2, false, 2, true, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_OK);
			// ...and for a CORE-ONLY app, which never has the extension.
			CHECK(oxr_projection_view_count_verdict(2, 2, false, 1, false, nullptr, 0, k) ==
			      OXR_VIEW_COUNT_OK);

			// MULTIVIEW_DXR, R = 4, whatever the active mode is.
			CHECK(oxr_projection_view_count_verdict(4, 4, true, 1, true, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_OK);
			CHECK(oxr_projection_view_count_verdict(4, 4, true, 2, true, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_OK);
		}
	}

	SECTION("MULTIVIEW under-submit dies at the default and lives only under the kill switch")
	{
		// The contradiction ADR-041 removes: a 4-view session submitting 2
		// because the panel happens to be in a stereo mode. Core OpenXR says
		// all located views must be supplied.
		CHECK(oxr_projection_view_count_verdict(2, 4, true, 2, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_STRICT) == OXR_VIEW_COUNT_REJECT);
		CHECK(oxr_projection_view_count_verdict(2, 4, true, 2, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_REJECT);
		CHECK(oxr_projection_view_count_verdict(2, 4, true, 2, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_LEGACY) == OXR_VIEW_COUNT_OK);

		// One view is the same story.
		CHECK(oxr_projection_view_count_verdict(1, 4, true, 1, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_REJECT);
		CHECK(oxr_projection_view_count_verdict(1, 4, true, 1, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_LEGACY) == OXR_VIEW_COUNT_OK);
	}

	SECTION("the ONE compat arm: PRIMARY_STEREO + extension + 1-view mode, and it is marked")
	{
		// Released demos submit one view in 2D mode; the arm is what keeps
		// them running, and OK_DEPRECATED is what gets that fact into the log
		// exactly once instead of blessing it silently.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, 1, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_OK_DEPRECATED);

		// Strict drops it, BY DEFINITION — that is what "0" means.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, 1, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_STRICT) == OXR_VIEW_COUNT_REJECT);

		// Both halves of the #1486 gate survive into the compat arm.
		// No extension (every CTS session) -> reject whatever the mode.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, 1, false, nullptr, 0, OXR_UNDER_SUBMIT_COMPAT) ==
		      OXR_VIEW_COUNT_REJECT);
		// Extension, but the active mode is not 1-view -> reject.
		CHECK(oxr_projection_view_count_verdict(1, 2, false, 2, true, kSimDisplayModes, kSimDisplayModeCount,
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
			CHECK(oxr_projection_view_count_verdict(1, 2, false, 1, false, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_REJECT);
			CHECK(oxr_projection_view_count_verdict(1, 2, false, 2, false, kSimDisplayModes,
			                                        kSimDisplayModeCount, k) == OXR_VIEW_COUNT_REJECT);
		}
	}

	SECTION("over-submitting past R is refused too — R is FIXED, not a floor")
	{
		CHECK(oxr_projection_view_count_verdict(4, 2, false, 4, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_COMPAT) == OXR_VIEW_COUNT_REJECT);
		CHECK(oxr_projection_view_count_verdict(3, 2, false, 2, true, kSimDisplayModes, kSimDisplayModeCount,
		                                        OXR_UNDER_SUBMIT_STRICT) == OXR_VIEW_COUNT_REJECT);
	}
}
