// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pin the service weave engine's panel-relative present origin
 *         (u_weave_present_origin.h) across fractional scales: the DP phase
 *         origin must be panel-relative DEVICE px whatever the panel scale,
 *         the X root scale and the side the panel is on.
 */

#include "catch_amalgamated.hpp"

#include "util/u_weave_present_origin.h"

#include <cmath>

namespace {

constexpr uint32_t kPanelW = 3840; // Odyssey device mode
constexpr uint32_t kPanelH = 2160;

/*!
 * A two-monitor GNOME (LOGICAL layout mode) desktop: a laptop and the 3D
 * panel side by side, top-aligned at stage y = 0. Mutter normalises the
 * layout so its top-left is (0,0); XWayland's root is the stage times one
 * global factor (the ceiling of the largest scale with xwayland-native-scaling,
 * else 1).
 */
struct Desk
{
	uint32_t laptop_w_px, laptop_h_px;
	double laptop_scale;
	double panel_scale;
	bool panel_right; // panel right of the laptop, else left of it
	bool native_scaling = true;

	int32_t
	laptop_lw() const
	{
		return u_weave_round(laptop_w_px / laptop_scale);
	}
	int32_t
	panel_lw() const
	{
		return u_weave_round(kPanelW / panel_scale);
	}
	int32_t
	panel_lh() const
	{
		return u_weave_round(kPanelH / panel_scale);
	}
	int32_t
	panel_lx() const
	{
		return panel_right ? laptop_lw() : 0;
	}
	double
	root() const
	{
		return native_scaling ? std::ceil(std::fmax(laptop_scale, panel_scale) - 1e-9) : 1.0;
	}

	//! What fill_display_desktop_info hands the service.
	u_weave_panel_frame
	frame() const
	{
		u_weave_panel_frame f = {};
		f.screen_left = u_weave_round(panel_lx() * root());
		f.screen_top = 0;
		f.desktop_w = (uint32_t)u_weave_round(panel_lw() * root());
		f.desktop_h = (uint32_t)u_weave_round(panel_lh() * root());
		f.native_w = kPanelW;
		f.native_h = kPanelH;
		f.scale = panel_scale;
		return f;
	}

	//! The browser's bound windowOriginOnScreen for a content top-left at stage
	//! (lx, ly) on the panel: StageRectToDevicePx / u_wl_window_rect_px_on_monitor.
	void
	bind_on_panel(int32_t lx, int32_t ly, int32_t *x, int32_t *y) const
	{
		*x = u_wl_logical_to_px(panel_lx(), panel_scale) + u_wl_logical_to_px(lx - panel_lx(), panel_scale);
		*y = u_wl_logical_to_px(0, panel_scale) + u_wl_logical_to_px(ly, panel_scale);
	}
};

void
check_on_panel(const Desk &d, int32_t rel_lx, int32_t rel_ly)
{
	const u_weave_panel_frame f = d.frame();
	const int32_t lx = d.panel_lx() + rel_lx, ly = rel_ly;
	int32_t wx = 0, wy = 0;
	d.bind_on_panel(lx, ly, &wx, &wy);
	// Ground truth: what the in-process path feeds (wl_geom's monitor-relative
	// device origin) — the panel-relative stage offset at the panel's scale.
	const int32_t want_x = u_wl_logical_to_px(rel_lx, d.panel_scale);
	const int32_t want_y = u_wl_logical_to_px(rel_ly, d.panel_scale);

	int32_t ox = 0, oy = 0;
	// Device geometry only (no v12 chain).
	u_weave_panel_relative_origin(&f, wx, wy, false, 0, 0, 0.0f, &ox, &oy);
	INFO("panel " << d.panel_scale << " laptop " << d.laptop_scale << (d.panel_right ? " right" : " left")
	              << " root " << d.root() << " X11 left " << f.screen_left << " window " << wx << "," << wy);
	CHECK(ox == want_x);
	CHECK(oy == want_y);
	// With the v12 logical origin.
	u_weave_panel_relative_origin(&f, wx, wy, true, lx, ly, (float)d.panel_scale, &ox, &oy);
	CHECK(ox == want_x);
	CHECK(oy == want_y);
}

} // namespace

TEST_CASE("present origin is panel-relative device px at any panel scale, panel right of a scaled laptop")
{
	for (double ps : {1.0, 1.25, 1.5, 1.75, 2.0}) {
		for (double ls : {1.0, 1.6666666, 2.0}) {
			Desk d{2880, 1800, ls, ps, true};
			check_on_panel(d, 508, 101);
			check_on_panel(d, 0, 0);
			check_on_panel(d, 1, 3);
			d.native_scaling = false; // X root in logical px
			check_on_panel(d, 508, 101);
		}
	}
}

TEST_CASE("present origin with the panel LEFT of the laptop")
{
	for (double ps : {1.0, 1.25, 1.5, 1.75, 2.0}) {
		Desk d{2880, 1800, 1.6666666, ps, false};
		check_on_panel(d, 508, 101);
		check_on_panel(d, 37, 0);
	}
}

TEST_CASE("the measured box: Odyssey at 150 % right of a 167 % laptop")
{
	// service log: X11 panel rect 5120x2880 at (3456,0), connector 3840x2160 at 150 %.
	const u_weave_panel_frame f = {3456, 0, 5120, 2880, kPanelW, kPanelH, 1.5};
	int32_t px = 0, py = 0;
	REQUIRE(u_weave_panel_device_origin(&f, &px, &py));
	CHECK(px == 2592); // 1728 logical x 1.5
	CHECK(py == 0);
	// A window whose content is at stage (2236,101): device 2592 + 762.
	int32_t ox = 0, oy = 0;
	CHECK(u_weave_panel_relative_origin(&f, 2592 + 762, 152, false, 0, 0, 0.f, &ox, &oy) ==
	      U_WEAVE_ORIGIN_DEVICE_CONVERTED);
	CHECK(ox == 762);
	CHECK(oy == 152);
	CHECK(u_weave_panel_relative_origin(&f, 2592 + 762, 152, true, 2236, 101, 1.5f, &ox, &oy) ==
	      U_WEAVE_ORIGIN_LOGICAL);
	CHECK(ox == 762);
	CHECK(oy == 152);
	// The pre-fix arithmetic would have fed (2592+762) - 3456 = -102.
}

TEST_CASE("a 200 % panel on a 2x root and pure X11 keep the pre-fix value")
{
	// DS1 / Odyssey at 200 %: X11 rect == device mode, ratio 1.
	const u_weave_panel_frame f = {3456, 0, kPanelW, kPanelH, kPanelW, kPanelH, 2.0};
	int32_t ox = 0, oy = 0;
	CHECK(u_weave_panel_relative_origin(&f, 3950, 346, false, 0, 0, 0.f, &ox, &oy) ==
	      U_WEAVE_ORIGIN_DEVICE_UNSCALED);
	CHECK(ox == 494);
	CHECK(oy == 346);
	// Unknown native mode (sim_display without a connector, Windows-style info).
	const u_weave_panel_frame g = {1920, 0, 0, 0, 0, 0, 0.0};
	CHECK(u_weave_panel_relative_origin(&g, 2000, 50, false, 0, 0, 0.f, &ox, &oy) ==
	      U_WEAVE_ORIGIN_DEVICE_UNSCALED);
	CHECK(ox == 80);
	CHECK(oy == 50);
}

TEST_CASE("PHYSICAL layout mode: stage is device px, factor 1")
{
	// GNOME 46 at an integer 200 %: stage == device, X root == device, the
	// browser binds stage origin with factor 1. Laptop 2880 device wide.
	const u_weave_panel_frame f = {2880, 0, kPanelW, kPanelH, kPanelW, kPanelH, 2.0};
	int32_t ox = 0, oy = 0;
	CHECK(u_weave_panel_relative_origin(&f, 2880 + 400, 200, true, 2880 + 400, 200, 1.0f, &ox, &oy) ==
	      U_WEAVE_ORIGIN_LOGICAL);
	CHECK(ox == 400);
	CHECK(oy == 200);
}

TEST_CASE("a window straddling from a laptop at another scale uses the panel's scale")
{
	// Panel 150 % right of a 167 % laptop; the window's own monitor is the
	// laptop, so its device geometry is in the laptop's frame — only the
	// logical origin gives a panel-relative position (the off-panel band).
	const Desk d{2880, 1800, 1.6666666, 1.5, true};
	const u_weave_panel_frame f = d.frame();
	const int32_t lx = d.panel_lx() - 200; // 200 logical px left of the panel
	const int32_t wx = u_wl_logical_to_px(lx, 1.6666666);
	int32_t ox = 0, oy = 0;
	u_weave_panel_relative_origin(&f, wx, 0, true, lx, 0, 1.6666666f, &ox, &oy);
	CHECK(ox == -300); // -200 logical at the panel's 1.5
	CHECK(oy == 0);
}
