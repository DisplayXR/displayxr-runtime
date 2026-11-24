// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2801: what an in-process session is told when the compositor moved
 *         the rendering mode (the runtime's V / '0' / '1' keys on a
 *         runtime-owned window).
 *
 * The wiring (xrPollEvent reads the head device and the session, and pushes
 * XrEventDataRenderingModeChangedDXR / XrEventDataHardwareDisplayStateChangedDXR)
 * needs an instance, a device and a compositor, so the decision is a pure
 * function in oxr_mode_observed_rule.h and that is what is pinned here.
 */

#include "catch_amalgamated.hpp"

#include "oxr_mode_observed_rule.h"

namespace {

xrt_rendering_mode
mk(uint32_t views, bool hw3d)
{
	xrt_rendering_mode m{};
	m.view_count = views;
	m.hardware_display_3d = hw3d;
	return m;
}

//! ADR-065: [0] = 2D, [1] = the driver's 3D kind.
const xrt_rendering_mode kModes[2] = {mk(1, false), mk(2, true)};

oxr_mode_observed_decision
decide(uint32_t last,
       uint32_t active,
       bool sess_hw3d,
       bool in_process = true,
       bool running = true,
       bool events = true,
       bool held_2d = false)
{
	return oxr_mode_observed_decide(kModes, 2, last, active, sess_hw3d, in_process, running, events, held_2d);
}

} // namespace

TEST_CASE("a mode the compositor moved is reported once, with both events (#2801)", "[oxr][mode_observed]")
{
	SECTION("V from 3D to 2D")
	{
		const oxr_mode_observed_decision d = decide(1, 0, true);
		CHECK(d.sync);
		CHECK(d.push_mode_event);
		CHECK(d.previous == 1);
		CHECK(d.current == 0);
		CHECK(d.sync_hardware);
		CHECK(d.push_hardware_event);
		CHECK_FALSE(d.hardware_3d);
	}

	SECTION("'1' from 2D to 3D")
	{
		const oxr_mode_observed_decision d = decide(0, 1, false);
		CHECK(d.push_mode_event);
		CHECK(d.previous == 0);
		CHECK(d.current == 1);
		CHECK(d.push_hardware_event);
		CHECK(d.hardware_3d);
	}

	SECTION("once: after the session recorded it, the next poll decides nothing")
	{
		const oxr_mode_observed_decision first = decide(1, 0, true);
		REQUIRE(first.sync);
		const oxr_mode_observed_decision second = decide(first.current, 0, first.hardware_3d);
		CHECK_FALSE(second.sync);
		CHECK_FALSE(second.push_mode_event);
		CHECK_FALSE(second.push_hardware_event);
	}
}

TEST_CASE("never an event with previous == current (#2801)", "[oxr][mode_observed]")
{
	for (uint32_t idx = 0; idx < 2; idx++) {
		for (int hw = 0; hw < 2; hw++) {
			const oxr_mode_observed_decision d = decide(idx, idx, hw != 0);
			CHECK_FALSE(d.sync);
			CHECK_FALSE(d.push_mode_event);
			// The hardware state alone is not this rule's to report: the
			// request path, auto-2D and the visibility poll own it.
			CHECK_FALSE(d.sync_hardware);
			CHECK_FALSE(d.push_hardware_event);
		}
	}
}

TEST_CASE("the hardware event follows the 3D state, not the index (#2801)", "[oxr][mode_observed]")
{
	// The index moved but the session already holds that hardware state.
	const oxr_mode_observed_decision d = decide(1, 0, false);
	CHECK(d.push_mode_event);
	CHECK_FALSE(d.sync_hardware);
	CHECK_FALSE(d.push_hardware_event);
}

TEST_CASE("who is not told (#2801)", "[oxr][mode_observed]")
{
	SECTION("a service-mode session: the service's events own it")
	{
		const oxr_mode_observed_decision d = decide(1, 0, true, /*in_process=*/false);
		CHECK_FALSE(d.sync);
		CHECK_FALSE(d.push_mode_event);
		CHECK_FALSE(d.push_hardware_event);
	}

	SECTION("a session that is not running: recorded, not reported")
	{
		// xrEndSession drops the device to mode 0; xrBeginSession may restore it.
		const oxr_mode_observed_decision d = decide(1, 0, true, true, /*running=*/false);
		CHECK(d.sync);
		CHECK(d.current == 0);
		CHECK_FALSE(d.push_mode_event);
		CHECK_FALSE(d.sync_hardware);
		CHECK_FALSE(d.push_hardware_event);
	}

	SECTION("an app that did not enable XR_DXR_display_info: state recorded, no event")
	{
		// A legacy app's V toggle.
		const oxr_mode_observed_decision d = decide(1, 0, true, true, true, /*events=*/false);
		CHECK(d.sync);
		CHECK(d.sync_hardware);
		CHECK_FALSE(d.hardware_3d);
		CHECK_FALSE(d.push_mode_event);
		CHECK_FALSE(d.push_hardware_event);
	}
}

TEST_CASE("a panel the runtime holds 2D reports the mode, not the hardware (#2801)", "[oxr][mode_observed]")
{
	const oxr_mode_observed_decision d = decide(0, 1, false, true, true, true, /*held_2d=*/true);
	CHECK(d.push_mode_event);
	CHECK(d.sync_hardware);
	CHECK(d.hardware_3d);
	CHECK_FALSE(d.push_hardware_event);
}

TEST_CASE("an index outside the table reports the mode and leaves the hardware state (#2801)", "[oxr][mode_observed]")
{
	SECTION("no table")
	{
		const oxr_mode_observed_decision d =
		    oxr_mode_observed_decide(nullptr, 0, 0, 1, false, true, true, true, false);
		CHECK(d.push_mode_event);
		CHECK_FALSE(d.sync_hardware);
		CHECK_FALSE(d.push_hardware_event);
	}

	SECTION("index past the end")
	{
		const oxr_mode_observed_decision d = decide(0, 7, false);
		CHECK(d.push_mode_event);
		CHECK(d.current == 7);
		CHECK_FALSE(d.sync_hardware);
	}
}
