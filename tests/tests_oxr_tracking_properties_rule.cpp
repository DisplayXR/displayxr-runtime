// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1631: what XrSystemTrackingProperties advertises on a 3D display.
 *
 * Driving the real xrGetSystemProperties answer needs an instance, a system, a
 * head device, role devices and a registered plug-in — so, exactly like
 * tests_oxr_legacy_mode_rule.cpp and tests_oxr_view_config_rule.cpp, the
 * decision itself lives as pure functions in oxr_tracking_properties_rule.h and
 * that is what is pinned here.
 *
 * NOT pinned here (it is on the Windows hardware leg): the WIRING — that
 * oxr_system_get_properties() feeds the head flags, the compositor info's
 * supported_eye_tracking_modes and the seven role devices into these functions.
 * The hardware oracle is `displayxr-cli info` / the CTS log's system properties
 * on a qwerty-backed sim-display session, plus the three CTS files that used to
 * skip now running.
 */

#include "catch_amalgamated.hpp"

#include "oxr_tracking_properties_rule.h"

TEST_CASE("#1631 the display processor's eye-tracking term")
{
	// sim-display's honest default (#441): no eye tracker, no capability.
	CHECK_FALSE(oxr_dp_tracks_eyes(0u));

	// SIM_DISPLAY_FAKE_TRACKING=1 re-enables MANUAL_BIT (2). Any non-zero
	// capability mask counts — MANAGED (1), MANUAL (2), or both.
	CHECK(oxr_dp_tracks_eyes(1u));
	CHECK(oxr_dp_tracks_eyes(2u));
	CHECK(oxr_dp_tracks_eyes(3u));
}

TEST_CASE("#1631 a bare sim-display with no role devices still reports FALSE")
{
	// The head is the display driver's (both flags false), the DP has no
	// eye tracking, and nothing holds a role. This is the honest
	// counter-argument on the issue and it is preserved deliberately.
	const bool dp = oxr_dp_tracks_eyes(0u);

	CHECK_FALSE(oxr_system_orientation_tracking(false, dp, false));
	CHECK_FALSE(oxr_system_position_tracking(false, dp, false));
}

TEST_CASE("#1631 tracked role devices alone make the system tracked")
{
	// qwerty / sim_input / net_input / the hand-tracking provider all set
	// both flags on their controller devices; the head stays untracked.
	const bool dp = oxr_dp_tracks_eyes(0u);

	CHECK(oxr_system_orientation_tracking(false, dp, true));
	CHECK(oxr_system_position_tracking(false, dp, true));
}

TEST_CASE("#1631 display-processor eye tracking alone makes the system tracked")
{
	// A real eye-tracked rig with no controllers at all: the runtime
	// delivers tracked VIEW poses, so the system tracks.
	const bool dp = oxr_dp_tracks_eyes(2u); // MANUAL_BIT

	CHECK(oxr_system_orientation_tracking(false, dp, false));
	CHECK(oxr_system_position_tracking(false, dp, false));
}

TEST_CASE("#1631 the head's own flags stay in the OR")
{
	// Pre-#1631 behaviour is a strict subset: a head that tracks still
	// reports tracked with nothing else in the system.
	CHECK(oxr_system_orientation_tracking(true, false, false));
	CHECK(oxr_system_position_tracking(true, false, false));
}

TEST_CASE("#1631 orientation and position are computed independently")
{
	// A role device that tracks orientation but not position must not
	// make positionTracking true — the two terms never cross over.
	CHECK(oxr_system_orientation_tracking(false, false, /* any_role_orientation */ true));
	CHECK_FALSE(oxr_system_position_tracking(false, false, /* any_role_position */ false));

	// ...and the reverse pairing, so neither direction leaks.
	CHECK_FALSE(oxr_system_orientation_tracking(false, false, false));
	CHECK(oxr_system_position_tracking(false, false, true));
}
