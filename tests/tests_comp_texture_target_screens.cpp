// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The screen half of the texture target coverage rule (ADR-060,
 *         #2721): which listed screens count as "another screen" for the
 *         one-DP Vulkan / GL tiers (comp_texture_target_screens.h).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_texture_target_screens.h"

#include <cstring>

namespace {

struct fixture
{
	struct xrt_dp_registry_entry panel;
	struct xrt_screen_list list;

	fixture()
	{
		std::memset(&panel, 0, sizeof(panel));
		panel.monitor_id = 0x10;
		panel.screen_left = 0;
		panel.screen_top = 0;
		panel.pixel_width = 3840;
		panel.pixel_height = 2160;
		std::memset(&list, 0, sizeof(list));
		add(0x10, 0, 0, 3840, 2160);    // the panel itself
		add(0x20, 3840, 0, 1920, 1080); // a monitor to its right
	}

	void
	add(uint64_t id, int32_t x, int32_t y, uint32_t w, uint32_t h)
	{
		struct xrt_screen &s = list.screens[list.count++];
		s.id = id;
		s.desktop_left = x;
		s.desktop_top = y;
		s.desktop_width = w;
		s.desktop_height = h;
	}
};

struct comp_texture_target_resolved
target(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	struct comp_texture_target_resolved r = {};
	r.valid = true;
	r.screen.x = x;
	r.screen.y = y;
	r.screen.w = w;
	r.screen.h = h;
	return r;
}

} // namespace

TEST_CASE("texture_target_screens: on, spanning and off the panel")
{
	fixture f;
	auto on = target(100, 100, 800, 600);
	auto span = target(3500, 100, 800, 600);
	auto off = target(4000, 100, 800, 600);
	CHECK(comp_texture_target_coverage_for_entry(&on, &f.panel, &f.list, 0) ==
	      COMP_TEXTURE_TARGET_COVERAGE_ON_PANEL);
	CHECK(comp_texture_target_coverage_for_entry(&span, &f.panel, &f.list, 0) ==
	      COMP_TEXTURE_TARGET_COVERAGE_SPANNING);
	CHECK(comp_texture_target_coverage_for_entry(&off, &f.panel, &f.list, 0) ==
	      COMP_TEXTURE_TARGET_COVERAGE_OFF_PANEL);
}

TEST_CASE("texture_target_screens: hanging off the desktop edge stays woven")
{
	fixture f;
	auto edge = target(-300, 1900, 800, 600);
	CHECK(comp_texture_target_coverage_for_entry(&edge, &f.panel, &f.list, 0) ==
	      COMP_TEXTURE_TARGET_COVERAGE_ON_PANEL);
}

TEST_CASE("texture_target_screens: the panel under another id is not another screen")
{
	fixture f;
	f.add(0x30, 0, 0, 3840, 2160); // a synthesized default screen over the panel
	auto on = target(100, 100, 800, 600);
	CHECK(comp_texture_target_coverage_for_entry(&on, &f.panel, &f.list, 0) ==
	      COMP_TEXTURE_TARGET_COVERAGE_ON_PANEL);
}

TEST_CASE("texture_target_screens: pinned, no list or no entry is never judged")
{
	fixture f;
	auto off = target(4000, 100, 800, 600);
	CHECK(comp_texture_target_coverage_for_entry(&off, &f.panel, &f.list, 0x10) ==
	      COMP_TEXTURE_TARGET_COVERAGE_ON_PANEL);
	CHECK(comp_texture_target_coverage_for_entry(&off, &f.panel, nullptr, 0) ==
	      COMP_TEXTURE_TARGET_COVERAGE_ON_PANEL);
	CHECK(comp_texture_target_coverage_for_entry(&off, nullptr, &f.list, 0) ==
	      COMP_TEXTURE_TARGET_COVERAGE_ON_PANEL);
}
