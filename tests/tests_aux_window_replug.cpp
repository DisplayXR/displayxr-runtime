// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Restore-on-replug geometry (util/u_window_replug.h): a window goes
 *         back to its offset on a screen that was unplugged and plugged in
 *         again, clamped only as far as needed to stay reachable.
 *
 * The layout is the measured Odyssey one: a 1512x982 pt laptop as the main
 * display, the 1920x1080 pt panel arranged right of it, top-aligned. Top-down
 * points throughout (the header's space).
 */

#include "catch_amalgamated.hpp"

#include "util/u_window_replug.h"

namespace {

const u_window_replug_rect kPanel = {1512, 0, 1920, 1080};
// Its visible area: below a 25 pt menu bar (macOS shows one on every display
// while "Displays have separate Spaces" is on).
const u_window_replug_rect kPanelVisible = {1512, 25, 1920, 1055};

} // namespace

TEST_CASE("replug: offset is relative to the screen's top-left", "[window_replug]")
{
	const u_window_replug_rect frame = {1912, 125, 1512, 855};
	double dx = 0, dy = 0;
	u_window_replug_offset(&frame, &kPanel, &dx, &dy);
	CHECK(dx == 400);
	CHECK(dy == 125);
}

TEST_CASE("replug: same panel, same place -> the window comes back exactly where it was", "[window_replug]")
{
	// Was 400 pt in from the panel's left edge. AppKit parked it at the
	// laptop's right edge on the unplug (the bug: on replug it then sat at
	// the panel's LEFT edge). The restore puts it back at +400.
	const u_window_replug_rect r = u_window_replug_restore(400, 125, 1512, 855, &kPanel, &kPanelVisible, 64);
	CHECK(r.x == 1912);
	CHECK(r.y == 125);
	CHECK(r.w == 1512);
	CHECK(r.h == 855);
}

TEST_CASE("replug: the panel came back at another place in the arrangement", "[window_replug]")
{
	// Re-arranged LEFT of the laptop: the offset follows the screen.
	const u_window_replug_rect panel = {-1920, 0, 1920, 1080};
	const u_window_replug_rect vis = {-1920, 0, 1920, 1080};
	const u_window_replug_rect r = u_window_replug_restore(400, 125, 1512, 855, &panel, &vis, 64);
	CHECK(r.x == -1520);
	CHECK(r.y == 125);
}

TEST_CASE("replug: a window that hung off the panel is restored as it was, within reach", "[window_replug]")
{
	// 1000 pt in, a 1512 pt wide window: 592 pt hung past the panel's right
	// edge before the unplug. Still >= 64 pt visible, so kept as-is.
	const u_window_replug_rect r = u_window_replug_restore(1000, 125, 1512, 855, &kPanel, &kPanelVisible, 64);
	CHECK(r.x == 2512);
	CHECK(r.y == 125);
}

TEST_CASE("replug: the panel came back smaller -> clamped to stay reachable", "[window_replug]")
{
	// Came back at 1280x720 pt: the old offset would leave the window entirely
	// off it. Keep 64 pt on screen horizontally and the title bar above the
	// bottom.
	const u_window_replug_rect panel = {1512, 0, 1280, 720};
	const u_window_replug_rect vis = {1512, 0, 1280, 720};
	const u_window_replug_rect r = u_window_replug_restore(1700, 900, 800, 400, &panel, &vis, 64);
	CHECK(r.x == 1512 + 1280 - 64);
	CHECK(r.y == 720 - 64);
	CHECK(r.w == 800);
	CHECK(r.h == 400);
}

TEST_CASE("replug: title bar never above the visible area (menu bar)", "[window_replug]")
{
	const u_window_replug_rect r = u_window_replug_restore(100, -50, 800, 400, &kPanel, &kPanelVisible, 64);
	CHECK(r.y == 25);
	// Off the left edge: at least 64 pt stay on the panel.
	const u_window_replug_rect l = u_window_replug_restore(-1000, 100, 800, 400, &kPanel, &kPanelVisible, 64);
	CHECK(l.x == 1512 + 64 - 800);
}
