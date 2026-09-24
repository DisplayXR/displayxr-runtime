// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pin the off-panel band -> woven-output mapping of the service weave
 *         engine (u_weave_span2d.h, #1654 / #1699).
 */

#include "catch_amalgamated.hpp"

#include "util/u_weave_span2d.h"

// The 3D panel's device mode on the measured box (see tests_aux_wayland_geom).
static constexpr int32_t kPanelModeW = 3840;
static constexpr int32_t kPanelModeH = 2160;

TEST_CASE("off-panel bands map 1:1 onto a window-sized output, clipped to a smaller one")
{
	// The service's batch output IS the window; a legacy output is only as big
	// as its element's rect, from the window's top-left.
	struct u_wl_rect_px win[4] = {};
	const uint32_t n = u_wl_offpanel_bands(-300, 100, kPanelModeW, kPanelModeH, 1280, 720, win);
	REQUIRE(n == 1);
	struct u_wl_rect_px o[4] = {};
	bool whole = true;
	REQUIRE(u_wl_offpanel_bands_to_output(win, n, 1280, 720, 1280, 720, false, o, &whole) == 1);
	REQUIRE(o[0].x == 0);
	REQUIRE(o[0].w == 300);
	REQUIRE(o[0].h == 720);
	REQUIRE_FALSE(whole);
	// A 200x100 legacy output: the band is clipped to it and then covers it all.
	REQUIRE(u_wl_offpanel_bands_to_output(win, n, 1280, 720, 200, 100, false, o, &whole) == 1);
	REQUIRE(o[0].w == 200);
	REQUIRE(o[0].h == 100);
	REQUIRE(whole);
}

TEST_CASE("off-panel bands scale onto a v6 content-view output")
{
	// A 1280x720 window, 640 px of it left of the panel, woven at viewScale
	// 0.5: the output is 640x360 and its left 320 columns are off the panel.
	struct u_wl_rect_px win[4] = {};
	const uint32_t n = u_wl_offpanel_bands(-640, 48, kPanelModeW, kPanelModeH, 1280, 720, win);
	REQUIRE(n == 1);
	struct u_wl_rect_px o[4] = {};
	bool whole = true;
	REQUIRE(u_wl_offpanel_bands_to_output(win, n, 1280, 720, 640, 360, true, o, &whole) == 1);
	REQUIRE(o[0].x == 0);
	REQUIRE(o[0].y == 0);
	REQUIRE(o[0].w == 320);
	REQUIRE(o[0].h == 360);
	REQUIRE_FALSE(whole);
}

TEST_CASE("scaled corner bands stay adjacent and cover the off-panel area exactly once")
{
	// Odd sizes so the scale rounds: 1001x777 window at 2/3 scale.
	struct u_wl_rect_px win[4] = {};
	const uint32_t n = u_wl_offpanel_bands(-101, -203, kPanelModeW, kPanelModeH, 1001, 777, win);
	REQUIRE(n == 2);
	struct u_wl_rect_px o[4] = {};
	REQUIRE(u_wl_offpanel_bands_to_output(win, n, 1001, 777, 667, 518, true, o, nullptr) == 2);
	// Top strip ends exactly where the left strip starts.
	REQUIRE(o[0].y == 0);
	REQUIRE(o[0].w == 667);
	REQUIRE(o[1].y == o[0].y + o[0].h);
	REQUIRE(o[1].x == 0);
	REQUIRE(o[1].y + o[1].h == 518);
}

TEST_CASE("a window entirely off the panel maps to one whole-output band")
{
	struct u_wl_rect_px win[4] = {};
	const uint32_t n = u_wl_offpanel_bands(-2000, 0, kPanelModeW, kPanelModeH, 1280, 720, win);
	struct u_wl_rect_px o[4] = {};
	bool whole = false;
	REQUIRE(u_wl_offpanel_bands_to_output(win, n, 1280, 720, 640, 360, true, o, &whole) == 1);
	REQUIRE(whole);
	REQUIRE(o[0].w == 640);
	REQUIRE(o[0].h == 360);
	// On the panel: nothing to map, and never "whole".
	REQUIRE(u_wl_offpanel_bands_to_output(win, 0, 1280, 720, 640, 360, true, o, &whole) == 0);
	REQUIRE_FALSE(whole);
}
