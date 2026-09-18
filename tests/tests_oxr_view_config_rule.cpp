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

TEST_CASE("PRIMARY_STEREO accepts 1 or 2 and nothing else (#1486)", "[oxr][view_config_rule]")
{
	CHECK(oxr_view_count_ok_for_stereo(1));
	CHECK(oxr_view_count_ok_for_stereo(2));

	// The #1486 tightening itself: a Quad submission under PRIMARY_STEREO is
	// refused even though the device HAS a 4-view rendering mode.
	CHECK_FALSE(oxr_view_count_ok_for_stereo(4));
	CHECK_FALSE(oxr_view_count_ok_for_stereo(3));
	CHECK_FALSE(oxr_view_count_ok_for_stereo(0));
	CHECK_FALSE(oxr_view_count_ok_for_stereo(8));
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

TEST_CASE("the two rules differ ONLY above 2 views (#1486)", "[oxr][view_config_rule]")
{
	// The whole blast radius of the tightening, stated as a property: on a
	// stereo-only panel the kill switch and the new rule are the same rule, so
	// no shipped Leia app can be affected by either.
	for (uint32_t n = 0; n <= 3; n++) {
		INFO("viewCount = " << n);
		CHECK(oxr_view_count_ok_for_stereo(n) ==
		      oxr_view_count_ok_for_multiview(n, kStereoOnlyModes, kStereoOnlyModeCount));
	}

	// And on a device that HAS a wider mode, they differ at exactly that count.
	CHECK_FALSE(oxr_view_count_ok_for_stereo(4));
	CHECK(oxr_view_count_ok_for_multiview(4, kSimDisplayModes, kSimDisplayModeCount));
}
