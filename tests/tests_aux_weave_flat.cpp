// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pin the flat-region -> woven-output mapping of the service weave
 *         engine (u_weave_flat.h, XR_DXR_weave spec v8 on desktop Linux, #1699).
 */

#include "catch_amalgamated.hpp"

#include "util/u_weave_flat.h"
#include "util/u_weave_flat.h"

TEST_CASE("flat regions: per-submit rects are window-relative and map 1:1 onto a batch output")
{
	const struct u_wl_rect_px win[2] = {{100, 100, 200, 50}, {1200, 700, 200, 50}};
	struct u_wl_rect_px o[8] = {};
	const uint32_t n = u_wl_flat_rects_to_output(win, 2, nullptr, 0, 5000, 5000, 1280, 720, 1280, 720, false, o, 8);
	REQUIRE(n == 2);
	REQUIRE(o[0].x == 100);
	REQUIRE(o[0].y == 100);
	REQUIRE(o[0].w == 200);
	REQUIRE(o[0].h == 50);
	// The second hangs off the window's bottom-right corner: clipped to it.
	REQUIRE(o[1].x == 1200);
	REQUIRE(o[1].w == 80);
	REQUIRE(o[1].h == 20);
}

TEST_CASE("flat regions: sticky rects are screen-absolute, clipped to the window, and compose with per-submit")
{
	// Window at (1000, 500) 1280x720.
	const struct u_wl_rect_px win[1] = {{10, 10, 20, 20}};
	const struct u_wl_rect_px scr[3] = {
	    {900, 480, 400, 60},   // straddles the window's top-left corner
	    {0, 0, 100, 100},      // wholly outside the window: clipped away
	    {1100, 600, 0, 30},    // empty: dropped
	};
	struct u_wl_rect_px o[8] = {};
	const uint32_t n = u_wl_flat_rects_to_output(win, 1, scr, 3, 1000, 500, 1280, 720, 1280, 720, false, o, 8);
	REQUIRE(n == 2);
	REQUIRE(o[0].x == 10);
	REQUIRE(o[1].x == 0);
	REQUIRE(o[1].y == 0);
	REQUIRE(o[1].w == 300);
	REQUIRE(o[1].h == 40);
}

TEST_CASE("flat regions scale onto a v6 content-view output and respect the output cap")
{
	const struct u_wl_rect_px win[2] = {{640, 360, 640, 360}, {0, 0, 2, 2}};
	struct u_wl_rect_px o[1] = {};
	REQUIRE(u_wl_flat_rects_to_output(win, 2, nullptr, 0, 0, 0, 1280, 720, 640, 360, true, o, 1) == 1);
	REQUIRE(o[0].x == 320);
	REQUIRE(o[0].y == 180);
	REQUIRE(o[0].w == 320);
	REQUIRE(o[0].h == 180);
	// Nothing without a window.
	REQUIRE(u_wl_flat_rects_to_output(win, 2, nullptr, 0, 0, 0, 0, 0, 640, 360, true, o, 1) == 0);
}
