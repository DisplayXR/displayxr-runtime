// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the hosted-window adoption policy (#2637): which of the
 *         process's own top-level windows a hosted session adopts under
 *         DXR_WIN32_ADOPT_OWN_WINDOW, and when it keeps its own window.
 */

#include "catch_amalgamated.hpp"

#include "util/comp_adopt_window_policy.h"

static comp_adopt_candidate
win(int32_t w, int32_t h, bool on_panel, bool fg = false, bool excl = false)
{
	comp_adopt_candidate c = {};
	c.x = 100;
	c.y = 100;
	c.w = w;
	c.h = h;
	c.on_panel = on_panel;
	c.foreground = fg;
	c.exclusive_fullscreen = excl;
	return c;
}

TEST_CASE("knob parse", "[adopt]")
{
	CHECK(comp_adopt_mode_parse(nullptr) == COMP_ADOPT_MODE_AUTO);
	CHECK(comp_adopt_mode_parse("") == COMP_ADOPT_MODE_AUTO);
	CHECK(comp_adopt_mode_parse("auto") == COMP_ADOPT_MODE_AUTO);
	CHECK(comp_adopt_mode_parse("0") == COMP_ADOPT_MODE_OFF);
	CHECK(comp_adopt_mode_parse("off") == COMP_ADOPT_MODE_OFF);
	CHECK(comp_adopt_mode_parse("false") == COMP_ADOPT_MODE_OFF);
	CHECK(comp_adopt_mode_parse("1") == COMP_ADOPT_MODE_FORCE);
	CHECK(comp_adopt_mode_parse("on") == COMP_ADOPT_MODE_FORCE);
}

TEST_CASE("auto: an app that owns no window keeps today's full-screen window", "[adopt]")
{
	const char *why = nullptr;
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_AUTO, nullptr, 0, &why) == -1);
	REQUIRE(why != nullptr);
}

TEST_CASE("auto: the one window on the panel is adopted", "[adopt]")
{
	const comp_adopt_candidate c[] = {win(1280, 720, true)};
	const char *why = nullptr;
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_AUTO, c, 1, &why) == 0);
}

TEST_CASE("auto: a window on another monitor is not adopted, a second off-panel one does not count", "[adopt]")
{
	const comp_adopt_candidate off[] = {win(1280, 720, false)};
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_AUTO, off, 1, nullptr) == -1);

	const comp_adopt_candidate mixed[] = {win(640, 480, false), win(1280, 720, true)};
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_AUTO, mixed, 2, nullptr) == 1);
}

TEST_CASE("auto: two windows on the panel are ambiguous; force picks foreground, else largest", "[adopt]")
{
	const comp_adopt_candidate two[] = {win(640, 480, true), win(1280, 720, true, /*fg*/ true)};
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_AUTO, two, 2, nullptr) == -1);
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_FORCE, two, 2, nullptr) == 1);

	const comp_adopt_candidate nofg[] = {win(1280, 720, false), win(640, 480, true)};
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_FORCE, nofg, 2, nullptr) == 0);
}

TEST_CASE("exclusive full-screen and empty client areas are never adopted", "[adopt]")
{
	const comp_adopt_candidate excl[] = {win(3840, 2160, true, true, /*excl*/ true)};
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_AUTO, excl, 1, nullptr) == -1);
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_FORCE, excl, 1, nullptr) == -1);

	const comp_adopt_candidate empty[] = {win(0, 0, true)};
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_AUTO, empty, 1, nullptr) == -1);
}

TEST_CASE("off: never adopts", "[adopt]")
{
	const comp_adopt_candidate c[] = {win(1280, 720, true, true)};
	const char *why = nullptr;
	CHECK(comp_adopt_choose(COMP_ADOPT_MODE_OFF, c, 1, &why) == -1);
	CHECK(why != nullptr);
}
