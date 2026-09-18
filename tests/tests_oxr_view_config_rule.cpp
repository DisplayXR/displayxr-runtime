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

TEST_CASE("PRIMARY_STEREO accepts 2, and 1 only in a 1-view mode (#1486)", "[oxr][view_config_rule]")
{
	SECTION("active mode is 2-view (SBS stereo, and what the CTS runs in)")
	{
		CHECK(oxr_view_count_ok_for_stereo(2, 2));

		// THE CTS ASSERTION. test_XrCompositionLayerProjection.cpp:225-230
		// locates the views, does `Layer.viewCount--` (2 -> 1 here) and CHECKs
		// for XR_ERROR_VALIDATION_FAILURE. A short submission in a 2-view mode
		// is an error, not a mono submission.
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2));

		// The #1486 tightening: a Quad submission under PRIMARY_STEREO is
		// refused even though the device HAS a 4-view rendering mode.
		CHECK_FALSE(oxr_view_count_ok_for_stereo(4, 2));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(3, 2));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(0, 2));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(8, 2));
	}

	SECTION("active mode is 1-view (2D/mono) — the single-view submission path")
	{
		// Who actually submits one: the cube_* apps compute
		// `eyeCount = display3D ? modeViewCount : 1`, and displayxr-common
		// forwards the caller's count. Both stay legal.
		CHECK(oxr_view_count_ok_for_stereo(1, 1));

		// An app that keeps submitting the stereo pair in 2D is still fine —
		// the compositor has always accepted that, and the extension's
		// backward-compatibility clause promises it.
		CHECK(oxr_view_count_ok_for_stereo(2, 1));

		CHECK_FALSE(oxr_view_count_ok_for_stereo(4, 1));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(0, 1));
	}

	SECTION("active mode is 4-view (Quad) — still exactly 2 under this type")
	{
		CHECK(oxr_view_count_ok_for_stereo(2, 4));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 4));
		// The point of the whole change: a Quad-mode app on PRIMARY_STEREO does
		// not get to submit 4. It begins PRIMARY_MULTIVIEW_DXR instead.
		CHECK_FALSE(oxr_view_count_ok_for_stereo(4, 4));
	}

	SECTION("unknown active mode falls back to the conformant answer")
	{
		// verify_projection_view_count passes 2 when it cannot read the active
		// mode, which makes the rule "exactly two" — never a loosening.
		CHECK(oxr_view_count_ok_for_stereo(2, 2));
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2));
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
	// Note the stereo rule waves 2 through even when the ACTIVE mode is 1-view,
	// so "the active mode is mono" is not a back door into mono's rule either.
	CHECK(oxr_view_count_ok_for_stereo(2, 2));
	CHECK(oxr_view_count_ok_for_stereo(2, 1));
	CHECK(oxr_view_count_ok_for_multiview(2, kSimDisplayModes, kSimDisplayModeCount));
	CHECK(oxr_view_count_ok_for_multiview(2, nullptr, 0));
}

TEST_CASE("where the two rules differ, stated honestly (#1486)", "[oxr][view_config_rule]")
{
	// This property used to read "the two rules are identical for every count
	// <= 3". That is no longer true unqualified, and saying so would hide the
	// CTS fix: the tight rule now also refuses ONE view in a >=2-view mode.
	SECTION("identical for every count <= 3 WHEN the active mode is 1-view")
	{
		// The 2D/mono case on a stereo-only panel: the kill switch and the new
		// rule agree, so no shipped Leia app in 2D is affected by either.
		for (uint32_t n = 0; n <= 3; n++) {
			INFO("viewCount = " << n);
			CHECK(oxr_view_count_ok_for_stereo(n, 1) ==
			      oxr_view_count_ok_for_multiview(n, kStereoOnlyModes, kStereoOnlyModeCount));
		}
	}

	SECTION("in a 2-view mode they differ at ONE view")
	{
		// The CTS fix, stated as the difference it introduces: the permissive
		// rule still waves a short submission through (an app one frame behind a
		// mode transition), the tight one does not.
		CHECK_FALSE(oxr_view_count_ok_for_stereo(1, 2));
		CHECK(oxr_view_count_ok_for_multiview(1, kStereoOnlyModes, kStereoOnlyModeCount));

		// Two is the agreed answer either way.
		CHECK(oxr_view_count_ok_for_stereo(2, 2));
		CHECK(oxr_view_count_ok_for_multiview(2, kStereoOnlyModes, kStereoOnlyModeCount));
	}

	SECTION("and on a device with a wider mode they differ at that count too")
	{
		CHECK_FALSE(oxr_view_count_ok_for_stereo(4, 4));
		CHECK(oxr_view_count_ok_for_multiview(4, kSimDisplayModes, kSimDisplayModeCount));
	}
}
