// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pin the logical->device conversion at the Wayland boundary
 *         (#1595 / #1596).
 *
 * Every case in here is the measured 2026-09-20 box unless it says otherwise:
 * Ubuntu 26.04, GNOME 50 Wayland, laptop eDP-1 primary at 2880x1800 device /
 * 1728x1080 logical (fractional scale 1.6667), Acer SpatialLabs DS1 on HDMI-1
 * at 3840x2160 device / 1920x1080 logical (scale 2.0), logical origin (1728, 0),
 * device origin (3456, 0).
 *
 * The box is chosen deliberately: BOTH scales are exercised, and an
 * integer-only implementation passes every 2.0 case and fails every 1.6667 one.
 * That is exactly the shape of the bug — `wl_output.scale` reports an integer 2
 * for the 1.6667 output, so a plausible-looking wrong answer is the default
 * failure mode, not an obvious one.
 */

#include "catch_amalgamated.hpp"

#include "util/u_wayland_geom.h"

// The measured layout, named once.
static constexpr int32_t kLaptopLogicalW = 1728;
static constexpr int32_t kLaptopLogicalH = 1080;
static constexpr int32_t kLaptopModeW = 2880;
static constexpr int32_t kLaptopModeH = 1800;

static constexpr int32_t kPanelLogicalX = 1728;
static constexpr int32_t kPanelLogicalW = 1920;
static constexpr int32_t kPanelLogicalH = 1080;
static constexpr int32_t kPanelModeW = 3840;
static constexpr int32_t kPanelModeH = 2160;
static constexpr int32_t kPanelDeviceX = 3456;

//! eDP-1 as the app sees it: xdg_output logical rect + wl_output mode, no
//! explicit scale (the 1.6667 has to be DERIVED).
static struct u_wl_monitor
laptop_from_xdg_output()
{
	struct u_wl_monitor m = {};
	m.logical_x = 0;
	m.logical_y = 0;
	m.logical_w = kLaptopLogicalW;
	m.logical_h = kLaptopLogicalH;
	m.mode_w = kLaptopModeW;
	m.mode_h = kLaptopModeH;
	return m;
}

//! HDMI-1 (the DS1) as the app sees it.
static struct u_wl_monitor
panel_from_xdg_output()
{
	struct u_wl_monitor m = {};
	m.logical_x = kPanelLogicalX;
	m.logical_y = 0;
	m.logical_w = kPanelLogicalW;
	m.logical_h = kPanelLogicalH;
	m.mode_w = kPanelModeW;
	m.mode_h = kPanelModeH;
	return m;
}

//! eDP-1 as the GNOME Shell geometry extension publishes it: logical rect plus
//! an EXPLICIT fractional scale, and no mode at all.
static struct u_wl_monitor
laptop_from_shell_extension()
{
	struct u_wl_monitor m = {};
	m.logical_x = 0;
	m.logical_y = 0;
	m.logical_w = kLaptopLogicalW;
	m.logical_h = kLaptopLogicalH;
	m.scale = 5.0 / 3.0; // Meta.Display.get_monitor_scale() -> 1.666...
	return m;
}

static struct u_wl_monitor
panel_from_shell_extension()
{
	struct u_wl_monitor m = {};
	m.logical_x = kPanelLogicalX;
	m.logical_y = 0;
	m.logical_w = kPanelLogicalW;
	m.logical_h = kPanelLogicalH;
	m.scale = 2.0;
	return m;
}


/*
 *
 * The scale itself.
 *
 */

TEST_CASE("u_wl_monitor_scale derives a FRACTIONAL scale from the mode")
{
	// The case an integer-only implementation gets wrong. 2880/1728 = 1.6667.
	const struct u_wl_monitor laptop = laptop_from_xdg_output();
	REQUIRE(u_wl_monitor_scale(&laptop) == Catch::Approx(5.0 / 3.0).epsilon(1e-9));

	// ...and the case it gets right, which is why the bug hides.
	const struct u_wl_monitor panel = panel_from_xdg_output();
	REQUIRE(u_wl_monitor_scale(&panel) == Catch::Approx(2.0).epsilon(1e-9));
}

TEST_CASE("u_wl_monitor_scale prefers an explicitly published fractional scale")
{
	struct u_wl_monitor m = laptop_from_shell_extension();
	REQUIRE(u_wl_monitor_scale(&m) == Catch::Approx(5.0 / 3.0).epsilon(1e-9));

	// An explicit scale wins over a mode that disagrees: the publisher that
	// states a fractional scale is the more precise of the two.
	m.mode_w = 9999;
	m.mode_h = 9999;
	REQUIRE(u_wl_monitor_scale(&m) == Catch::Approx(5.0 / 3.0).epsilon(1e-9));
}

TEST_CASE("u_wl_monitor_scale refuses rather than guessing")
{
	struct u_wl_monitor m = {};
	REQUIRE(u_wl_monitor_scale(&m) == 0.0);
	REQUIRE(u_wl_monitor_scale(nullptr) == 0.0);

	// Logical size but no mode and no scale: an integer wl_output.scale is the
	// only thing left, and this header will not take it.
	m.logical_w = kLaptopLogicalW;
	m.logical_h = kLaptopLogicalH;
	REQUIRE(u_wl_monitor_scale(&m) == 0.0);
}

TEST_CASE("u_wl_logical_to_px rounds half away from zero")
{
	// 1080 * 1.6667 = 1800.036 -> 1800, not 1799 (truncation would lose a row).
	REQUIRE(u_wl_logical_to_px(1080, 1.6667) == 1800);
	REQUIRE(u_wl_logical_to_px(1728, 1.6667) == 2880);
	// A negative displacement (window left of its monitor origin mid-drag)
	// must round away from zero too, or the phase jitters by a pixel across 0.
	REQUIRE(u_wl_logical_to_px(-3, 1.5) == -5);
	REQUIRE(u_wl_logical_to_px(0, 1.6667) == 0);
}


/*
 *
 * Monitor rects.
 *
 */

TEST_CASE("the DS1's device rect is 3840x2160+3456+0, from either publisher")
{
	// THE failing comparison of 2026-09-20: wl_output.geometry puts this
	// output at logical x=1728 while the runtime's panel rect has it at
	// device x=3456, so an unconverted match can never succeed.
	struct u_wl_rect_px r = {};

	const struct u_wl_monitor from_xdg = panel_from_xdg_output();
	REQUIRE(u_wl_monitor_rect_px(&from_xdg, &r));
	REQUIRE(r.x == kPanelDeviceX);
	REQUIRE(r.y == 0);
	REQUIRE(r.w == kPanelModeW);
	REQUIRE(r.h == kPanelModeH);

	const struct u_wl_monitor from_shell = panel_from_shell_extension();
	REQUIRE(u_wl_monitor_rect_px(&from_shell, &r));
	REQUIRE(r.x == kPanelDeviceX);
	REQUIRE(r.y == 0);
	REQUIRE(r.w == kPanelModeW);
	REQUIRE(r.h == kPanelModeH);
}

TEST_CASE("the laptop's device size is 2880x1800 at BOTH publishers' 1.6667")
{
	int32_t w = 0, h = 0;

	const struct u_wl_monitor from_xdg = laptop_from_xdg_output();
	REQUIRE(u_wl_monitor_size_px(&from_xdg, &w, &h));
	REQUIRE(w == kLaptopModeW);
	REQUIRE(h == kLaptopModeH);

	// No mode published: the size has to come out of the fractional scale,
	// and 1728 * 1.6667 must land exactly on 2880.
	const struct u_wl_monitor from_shell = laptop_from_shell_extension();
	REQUIRE(u_wl_monitor_size_px(&from_shell, &w, &h));
	REQUIRE(w == kLaptopModeW);
	REQUIRE(h == kLaptopModeH);
}

TEST_CASE("an integer scale of 2 on the 1.6667 output gives the WRONG answer")
{
	// Not a test of our code — a pin on why wl_output.scale is refused. If
	// someone ever wires the integer scale in, these are the numbers that
	// would silently appear instead.
	struct u_wl_monitor wrong = {};
	wrong.logical_w = kLaptopLogicalW;
	wrong.logical_h = kLaptopLogicalH;
	wrong.scale = 2.0; // what wl_output.scale reports for this output

	int32_t w = 0, h = 0;
	REQUIRE(u_wl_monitor_size_px(&wrong, &w, &h));
	REQUIRE(w == 3456); // vs the true 2880
	REQUIRE(h == 2160); // vs the true 1800
	REQUIRE(w != kLaptopModeW);
}


/*
 *
 * Window rects.
 *
 */

TEST_CASE("a fullscreen window on the DS1 is at offset (0,0) and panel-sized")
{
	const struct u_wl_monitor panel = panel_from_shell_extension();
	struct u_wl_rect_px r = {};
	// Mutter reports the fullscreen frame rect in logical pixels.
	REQUIRE(u_wl_window_rect_px_on_monitor(&panel, kPanelLogicalX, 0, kPanelLogicalW, kPanelLogicalH, &r));
	REQUIRE(r.x == 0);
	REQUIRE(r.y == 0);
	REQUIRE(r.w == kPanelModeW);
	REQUIRE(r.h == kPanelModeH);
}

TEST_CASE("a windowed surface on the DS1 converts its offset, not just its size")
{
	const struct u_wl_monitor panel = panel_from_shell_extension();
	struct u_wl_rect_px r = {};
	// A 960x540 logical window 200 logical px in from the panel's left edge.
	REQUIRE(u_wl_window_rect_px_on_monitor(&panel, kPanelLogicalX + 200, 100, 960, 540, &r));
	REQUIRE(r.x == 400);
	REQUIRE(r.y == 200);
	REQUIRE(r.w == 1920);
	REQUIRE(r.h == 1080);
}

TEST_CASE("the window offset is monitor-relative, so the global space cancels")
{
	// The reason the conversion is expressed relative to the monitor: the
	// absolute device origin of an output in a mixed-scale layout is not a
	// well-defined thing, but a displacement WITHIN one output is. Moving the
	// whole layout must not move the phase.
	struct u_wl_monitor shifted = panel_from_shell_extension();
	struct u_wl_rect_px a = {}, b = {};
	REQUIRE(u_wl_window_rect_px_on_monitor(&shifted, kPanelLogicalX + 200, 0, 960, 540, &a));

	shifted.logical_x += 5000;
	REQUIRE(u_wl_window_rect_px_on_monitor(&shifted, kPanelLogicalX + 5000 + 200, 0, 960, 540, &b));

	REQUIRE(a.x == b.x);
	REQUIRE(a.y == b.y);
	REQUIRE(a.w == b.w);
	REQUIRE(a.h == b.h);
}

TEST_CASE("a window on the 1.6667 laptop converts at 1.6667, not 2")
{
	const struct u_wl_monitor laptop = laptop_from_shell_extension();
	struct u_wl_rect_px r = {};
	REQUIRE(u_wl_window_rect_px_on_monitor(&laptop, 300, 150, 864, 540, &r));
	REQUIRE(r.x == 500);  // 300 * 1.6667, not 600
	REQUIRE(r.y == 250);  // 150 * 1.6667, not 300
	REQUIRE(r.w == 1440); // 864 * 1.6667, not 1728
	REQUIRE(r.h == 900);
}

TEST_CASE("u_wl_window_rect_px_on_monitor refuses a monitor with no scale")
{
	struct u_wl_monitor m = {};
	m.logical_w = 1728;
	m.logical_h = 1080;
	struct u_wl_rect_px r = {};
	REQUIRE_FALSE(u_wl_window_rect_px_on_monitor(&m, 0, 0, 100, 100, &r));
	// ...and a degenerate window.
	const struct u_wl_monitor panel = panel_from_shell_extension();
	REQUIRE_FALSE(u_wl_window_rect_px_on_monitor(&panel, 0, 0, 0, 100, &r));
}


/*
 *
 * Output-to-panel matching (#1596, third gap).
 *
 */

TEST_CASE("the DS1 matches the runtime's panel rect once converted")
{
	const struct u_wl_monitor panel = panel_from_xdg_output();
	bool origin_agrees = false;
	REQUIRE(u_wl_monitor_is_panel(&panel, kPanelDeviceX, 0, kPanelModeW, kPanelModeH, &origin_agrees));
	REQUIRE(origin_agrees);
}

TEST_CASE("the laptop does NOT match the panel rect")
{
	const struct u_wl_monitor laptop = laptop_from_xdg_output();
	bool origin_agrees = true;
	REQUIRE_FALSE(u_wl_monitor_is_panel(&laptop, kPanelDeviceX, 0, kPanelModeW, kPanelModeH, &origin_agrees));
	REQUIRE_FALSE(origin_agrees);
}

TEST_CASE("size matches but origin disagrees: still the panel, flagged")
{
	// The case the size-first rule exists for. A compositor whose global
	// device space is not this output's own (XWayland scales the whole root by
	// one integer factor, for instance) reports an origin we cannot reproduce,
	// while the MODE is device pixels by protocol and always comparable.
	const struct u_wl_monitor panel = panel_from_xdg_output();
	bool origin_agrees = true;
	REQUIRE(u_wl_monitor_is_panel(&panel, 9999, 0, kPanelModeW, kPanelModeH, &origin_agrees));
	REQUIRE_FALSE(origin_agrees);
}

TEST_CASE("u_wl_monitor_is_panel answers false on an unknown panel size")
{
	const struct u_wl_monitor panel = panel_from_xdg_output();
	REQUIRE_FALSE(u_wl_monitor_is_panel(&panel, kPanelDeviceX, 0, 0, 0, nullptr));
	REQUIRE_FALSE(u_wl_monitor_is_panel(nullptr, 0, 0, kPanelModeW, kPanelModeH, nullptr));
}

TEST_CASE("the logical rect never matches the panel — the original bug")
{
	// What the app actually compared on 2026-09-20: wl_output.geometry's
	// logical origin against the runtime's device-pixel panel rect. Even with
	// the right output in hand, the origin is off by the scale factor.
	const struct u_wl_monitor panel = panel_from_xdg_output();
	REQUIRE(panel.logical_x != kPanelDeviceX);
	REQUIRE(panel.logical_w != kPanelModeW);
}


/*
 *
 * The 1:1 decision (#1595).
 *
 */

TEST_CASE("a panel-sized buffer on the panel is 1:1")
{
	REQUIRE(u_wl_present_is_1to1(kPanelModeW, kPanelModeH, kPanelModeW, kPanelModeH));
}

TEST_CASE("the measured failure is NOT 1:1")
{
	// The session of 2026-09-20: fullscreened on the laptop, configured at
	// 1728x1080 logical, against a 3840x2160 panel.
	REQUIRE_FALSE(u_wl_present_is_1to1(1728, 1080, kPanelModeW, kPanelModeH));
	// And even had it landed on the DS1, a logical-sized buffer is a resample.
	REQUIRE_FALSE(u_wl_present_is_1to1(kPanelLogicalW, kPanelLogicalH, kPanelModeW, kPanelModeH));
}

TEST_CASE("one pixel off is not 1:1 — the rule has no tolerance")
{
	REQUIRE_FALSE(u_wl_present_is_1to1(kPanelModeW - 1, kPanelModeH, kPanelModeW, kPanelModeH));
	REQUIRE_FALSE(u_wl_present_is_1to1(kPanelModeW, kPanelModeH + 1, kPanelModeW, kPanelModeH));
}

TEST_CASE("an unknown extent answers false, and callers must not read it as a degrade")
{
	REQUIRE_FALSE(u_wl_present_is_1to1(0, 0, kPanelModeW, kPanelModeH));
	REQUIRE_FALSE(u_wl_present_is_1to1(kPanelModeW, kPanelModeH, 0, 0));
}

TEST_CASE("a windowed surface is 1:1 against its own DEVICE extent")
{
	// End to end: a 960x540 logical window on the DS1 occupies 1920x1080
	// device px, so a 1920x1080 buffer reaches glass unresampled and a
	// 960x540 one — the configure size, which is the tempting value — does not.
	const struct u_wl_monitor panel = panel_from_shell_extension();
	struct u_wl_rect_px win = {};
	REQUIRE(u_wl_window_rect_px_on_monitor(&panel, kPanelLogicalX + 200, 100, 960, 540, &win));
	REQUIRE(u_wl_present_is_1to1(1920, 1080, (uint32_t)win.w, (uint32_t)win.h));
	REQUIRE_FALSE(u_wl_present_is_1to1(960, 540, (uint32_t)win.w, (uint32_t)win.h));
}


/*
 *
 * Frame vs buffer: which rect is the content (#1654).
 *
 */

TEST_CASE("an undecorated window: frame and buffer agree, the buffer is used")
{
	const struct u_wl_rect_logical frame = {257, 173, 1280, 720};
	const struct u_wl_rect_logical buffer = frame;
	struct u_wl_rect_logical out = {};
	REQUIRE(u_wl_window_content_rect(&frame, &buffer, &out));
	REQUIRE(out.logical_y == 173);
	REQUIRE(u_wl_surface_within_frame(&frame, &buffer));
}

TEST_CASE("a client-side title bar: the content is the buffer, a bar-height below the frame")
{
	// Measured on GNOME 50 (1.6667 laptop): cube_handle_vk_linux --windowed
	// with its 46 px title bar in a subsurface at y = -46. Mutter's frame is
	// the window geometry (bar + content); its buffer rect is the bound
	// surface alone.
	const struct u_wl_rect_logical frame = {257, 173, 1280, 766};
	const struct u_wl_rect_logical buffer = {257, 219, 1280, 720};
	struct u_wl_rect_logical out = {};
	REQUIRE(u_wl_window_content_rect(&frame, &buffer, &out));
	REQUIRE(out.logical_x == 257);
	REQUIRE(out.logical_y == 219);
	REQUIRE(out.logical_w == 1280);
	REQUIRE(out.logical_h == 720);
	// A bar is not a mapping fault.
	REQUIRE(u_wl_surface_within_frame(&frame, &buffer));
}

TEST_CASE("no published buffer rect: fall back to the frame")
{
	const struct u_wl_rect_logical frame = {10, 20, 800, 600};
	struct u_wl_rect_logical out = {};
	REQUIRE_FALSE(u_wl_window_content_rect(&frame, nullptr, &out));
	REQUIRE(out.logical_y == 20);
	const struct u_wl_rect_logical empty = {0, 0, 0, 0};
	REQUIRE_FALSE(u_wl_window_content_rect(&frame, &empty, &out));
	REQUIRE(out.logical_h == 600);
}

TEST_CASE("an unmapped device-pixel buffer spills past the frame")
{
	// The #1653 case: frame 1920x1080 logical, a 3840x2160 buffer attached
	// with no viewport at 200 % -> a 3840x2160-LOGICAL surface.
	const struct u_wl_rect_logical frame = {1728, 0, 1920, 1080};
	const struct u_wl_rect_logical buffer = {1728, 0, 3840, 2160};
	REQUIRE_FALSE(u_wl_surface_within_frame(&frame, &buffer));
}
