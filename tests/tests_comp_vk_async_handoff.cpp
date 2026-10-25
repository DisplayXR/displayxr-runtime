// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pin which frames may take the non-blocking xrEndFrame hand-off (#1905, #2356).
 *
 * The served weave may run after xrEndFrame returns only when it reads nothing
 * the app owns. #2356: window-space layers qualify once layer_commit composited
 * them into the runtime atlas itself; Local2D, zones, a zone mask and zero-copy
 * still read app images at weave time and must stay blocking.
 */

#include "catch_amalgamated.hpp"

#include "vk_native/comp_vk_native_async_handoff.h"

static comp_vk_native_async_frame
plain_frame()
{
	comp_vk_native_async_frame f = {};
	f.enabled = true;
	f.runtime_queue = true;
	return f;
}

TEST_CASE("async hand-off: a plain projection frame qualifies", "[vk_async_handoff]")
{
	const comp_vk_native_async_frame f = plain_frame();
	CHECK(comp_vk_native_async_frame_eligible(&f));
	CHECK_FALSE(comp_vk_native_async_composite_ws_at_commit(&f)); // nothing to composite
}

TEST_CASE("async hand-off: window-space frames qualify only once composited at commit", "[vk_async_handoff]")
{
	comp_vk_native_async_frame f = plain_frame();
	f.has_window_space = true;

	// The #2356 regression: before the commit-time composite this frame blocked.
	f.window_space_at_commit = false;
	CHECK(comp_vk_native_async_composite_ws_at_commit(&f));
	CHECK_FALSE(comp_vk_native_async_frame_eligible(&f));

	f.window_space_at_commit = true;
	CHECK(comp_vk_native_async_frame_eligible(&f));
}

TEST_CASE("async hand-off: frames whose weave reads app images stay blocking", "[vk_async_handoff]")
{
	for (int which = 0; which < 4; which++) {
		for (int ws = 0; ws < 2; ws++) {
			comp_vk_native_async_frame f = plain_frame();
			f.has_window_space = ws != 0;
			f.window_space_at_commit = ws != 0;
			switch (which) {
			case 0: f.zero_copy = true; break;
			case 1: f.local_2d = true; break;
			case 2: f.zones = true; break;
			default: f.zone_mask = true; break;
			}
			CAPTURE(which, ws);
			CHECK_FALSE(comp_vk_native_async_frame_eligible(&f));
			// ...and the commit-time composite is not attempted: the weave
			// keeps compositing window-space layers for a blocking frame.
			CHECK_FALSE(comp_vk_native_async_composite_ws_at_commit(&f));
		}
	}
}

TEST_CASE("async hand-off: off, or no runtime queue, never qualifies", "[vk_async_handoff]")
{
	comp_vk_native_async_frame f = plain_frame();
	f.enabled = false;
	CHECK_FALSE(comp_vk_native_async_frame_eligible(&f));
	f.has_window_space = true;
	CHECK_FALSE(comp_vk_native_async_composite_ws_at_commit(&f));

	f = plain_frame();
	f.runtime_queue = false;
	f.has_window_space = true;
	f.window_space_at_commit = true;
	CHECK_FALSE(comp_vk_native_async_frame_eligible(&f));
	CHECK_FALSE(comp_vk_native_async_composite_ws_at_commit(&f));
}
