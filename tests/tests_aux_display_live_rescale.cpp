// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2251: a LIVE display-scale / arrangement change re-resolved by the
 *         service — the panel is found again from the plug-in's own hint, the
 *         new panel frame differs from the old one exactly when the placement
 *         moved, and the present origin computed from the new frame is the
 *         correct device-px phase origin (the old frame's is not).
 *
 * The chain under test is the one the service runs on Mutter's
 * MonitorsChanged: fill_display_desktop_info (os_display_desktop_select_by_size
 * over the re-enumerated monitors, hinted with the PLUG-IN's origin) -> the
 * system info's panel frame -> the weave engine's u_weave_panel_frame_equal
 * change test -> u_weave_panel_relative_origin fed to the DP phase slot.
 * Monitor lists are the measured ds1-linux layouts (laptop eDP-1 3456x2160
 * device, ZR 3840x2160 on HDMI-1, GNOME, X root at the ceiling scale).
 */

#include "catch_amalgamated.hpp"

#include "os/os_display_desktop.h"
#include "util/u_weave_present_origin.h"

#include <cstring>

namespace {

constexpr uint32_t PANEL_W = 3840, PANEL_H = 2160;
constexpr uint32_t LAPTOP_W = 3456, LAPTOP_H = 2160;

os_display_desktop_info
mon(const char *name,
    int32_t left,
    uint32_t x11_w,
    uint32_t x11_h,
    uint32_t nw,
    uint32_t nh,
    bool primary,
    double scale)
{
	os_display_desktop_info m = {};
	std::strncpy(m.device_name, name, sizeof(m.device_name) - 1);
	m.left = left;
	m.width = x11_w;
	m.height = x11_h;
	m.width_in_caller_dpi = x11_w;
	m.height_in_caller_dpi = x11_h;
	m.native_width = nw;
	m.native_height = nh;
	m.native_source = OS_DISPLAY_NATIVE_SOURCE_COMPOSITOR;
	m.scale = scale;
	m.is_primary = primary;
	return m;
}

//! A GNOME LOGICAL-mode desktop, laptop left of the panel, X root scaled by
//! the ceiling of the largest monitor scale (xwayland-native-scaling).
struct Layout
{
	double laptop_scale, panel_scale;

	double
	root() const
	{
		const double m = laptop_scale > panel_scale ? laptop_scale : panel_scale;
		double r = 1.0;
		while (r < m - 1e-9) {
			r += 1.0;
		}
		return r;
	}
	int32_t
	laptop_lw() const
	{
		return u_weave_round(LAPTOP_W / laptop_scale);
	}
	void
	monitors(os_display_desktop_info out[2]) const
	{
		const double r = root();
		out[0] =
		    mon("eDP-1", 0, (uint32_t)u_weave_round(laptop_lw() * r),
		        (uint32_t)u_weave_round(LAPTOP_H / laptop_scale * r), LAPTOP_W, LAPTOP_H, true, laptop_scale);
		out[1] =
		    mon("HDMI-1", u_weave_round(laptop_lw() * r), (uint32_t)u_weave_round(PANEL_W / panel_scale * r),
		        (uint32_t)u_weave_round(PANEL_H / panel_scale * r), PANEL_W, PANEL_H, false, panel_scale);
	}
};

//! What the plug-in reports (LeiaSR on Linux / a 4K sim-display): no origin.
os_display_panel_hint
plugin_hint()
{
	os_display_panel_hint h = {};
	h.pixel_width = PANEL_W;
	h.pixel_height = PANEL_H;
	return h;
}

//! fill_display_desktop_info's mapping of the resolved monitor onto the info
//! fields the weave engine's panel frame reads.
u_weave_panel_frame
frame_of(const os_display_desktop_info &m)
{
	u_weave_panel_frame f = {};
	f.screen_left = m.left;
	f.screen_top = m.top;
	f.desktop_w = m.width;
	f.desktop_h = m.height;
	f.native_w = m.native_width;
	f.native_h = m.native_height;
	f.scale = m.scale;
	return f;
}

//! Re-resolve: the same rule instance create runs, from the plug-in's hint.
u_weave_panel_frame
resolve(const Layout &l)
{
	os_display_desktop_info mons[2];
	l.monitors(mons);
	const os_display_panel_hint h = plugin_hint();
	os_display_panel_match match = {};
	const int32_t pick = os_display_desktop_select_by_size(mons, 2, &h, &match);
	REQUIRE(pick == 1);
	CHECK(match.rule == OS_DISPLAY_DESKTOP_RULE_CONNECTOR_MODE);
	return frame_of(mons[pick]);
}

//! The phase origin for a window whose content top-left sits @p rel_lx,
//! @p rel_ly logical px into the panel, bound with the v12 logical origin.
void
origin_for(const Layout &l, const u_weave_panel_frame &f, int32_t rel_lx, int32_t rel_ly, int32_t *ox, int32_t *oy)
{
	const int32_t lx = l.laptop_lw() + rel_lx;
	const int32_t wx = u_wl_logical_to_px(lx, l.panel_scale);
	const int32_t wy = u_wl_logical_to_px(rel_ly, l.panel_scale);
	u_weave_panel_relative_origin(&f, wx, wy, true, lx, rel_ly, (float)l.panel_scale, ox, oy);
}

} // namespace

TEST_CASE("live rescale: the ZR 150 % -> 200 % (the #2251 repro numbers)")
{
	const Layout before{2.0, 1.5}, after{2.0, 2.0};
	const u_weave_panel_frame f0 = resolve(before);
	const u_weave_panel_frame f1 = resolve(after);
	// The service log's two descriptions of the panel.
	CHECK(f0.screen_left == 3456);
	CHECK(f0.desktop_w == 5120);
	CHECK(f0.scale == 1.5);
	CHECK(f1.screen_left == 3456);
	CHECK(f1.desktop_w == 3840);
	CHECK(f1.scale == 2.0);
	CHECK_FALSE(u_weave_panel_frame_equal(&f0, &f1));

	// The browser re-bound at logical (2905,195) @2.0 = device (5810,390).
	int32_t ox = 0, oy = 0;
	u_weave_panel_relative_origin(&f1, 5810, 390, true, 2905, 195, 2.0f, &ox, &oy);
	CHECK(ox == 2354);
	CHECK(oy == 390);
	// The bug: the same bind against the bring-up frame (what the service fed).
	u_weave_panel_relative_origin(&f0, 5810, 390, true, 2905, 195, 2.0f, &ox, &oy);
	CHECK(ox == 1766);
	CHECK(oy == 293);
}

TEST_CASE("live rescale: the ZR 200 % -> 150 %")
{
	const Layout before{2.0, 2.0}, after{2.0, 1.5};
	const u_weave_panel_frame f0 = resolve(before);
	const u_weave_panel_frame f1 = resolve(after);
	CHECK_FALSE(u_weave_panel_frame_equal(&f0, &f1));
	int32_t ox = 0, oy = 0;
	origin_for(after, f1, 508, 101, &ox, &oy);
	CHECK(ox == 762); // 508 logical x 1.5
	CHECK(oy == 152);
	// Against the stale 200 % frame the phase would be scaled by 2.
	int32_t sx = 0, sy = 0;
	origin_for(after, f0, 508, 101, &sx, &sy);
	CHECK(sx == 1016);
	CHECK(sy == 202);
}

TEST_CASE("live rescale: the LAPTOP changes scale, the panel does not — the panel's X11 origin moves")
{
	const Layout before{2.0, 1.5}, after{1.5, 1.5};
	const u_weave_panel_frame f0 = resolve(before);
	const u_weave_panel_frame f1 = resolve(after);
	CHECK(f0.screen_left == 3456);
	CHECK(f1.screen_left == 4608); // laptop now 2304 logical x root 2
	CHECK(f1.desktop_w == f0.desktop_w);
	CHECK(f1.scale == f0.scale);
	CHECK_FALSE(u_weave_panel_frame_equal(&f0, &f1));

	int32_t ox = 0, oy = 0;
	origin_for(after, f1, 400, 100, &ox, &oy);
	CHECK(ox == 600);
	CHECK(oy == 150);
	int32_t sx = 0, sy = 0;
	origin_for(after, f0, 400, 100, &sx, &sy);
	CHECK(sx != 600); // the stale frame: 864 device px (576 logical) off the panel's real origin

	// Why the re-resolve must hint with the PLUG-IN's origin and not the one
	// the previous resolve published: the old panel origin (3456, 0) now lies
	// INSIDE the laptop's rect, so as a "reported position" (rule 1, nearest
	// monitor) it would resolve the laptop as the panel.
	os_display_desktop_info mons[2];
	after.monitors(mons);
	CHECK(f0.screen_left >= mons[0].left);
	CHECK(f0.screen_left < mons[0].left + (int32_t)mons[0].width);
}

TEST_CASE("live rescale: PHYSICAL layout mode — a scale change moves no pixel, the origin is unchanged")
{
	// Integer scales, stage == device == X11: the frame's scale changes, the
	// rect does not; the window binds with factor 1.
	const u_weave_panel_frame f0 = {(int32_t)LAPTOP_W, 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, 1.0};
	const u_weave_panel_frame f1 = {(int32_t)LAPTOP_W, 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, 2.0};
	CHECK_FALSE(u_weave_panel_frame_equal(&f0, &f1)); // re-logged once...
	int32_t a = 0, b = 0, c = 0, d = 0;
	u_weave_panel_relative_origin(&f0, LAPTOP_W + 400, 200, true, LAPTOP_W + 400, 200, 1.0f, &a, &b);
	u_weave_panel_relative_origin(&f1, LAPTOP_W + 400, 200, true, LAPTOP_W + 400, 200, 1.0f, &c, &d);
	CHECK(a == 400); // ...with the same value: no phase jump
	CHECK(b == 200);
	CHECK(c == a);
	CHECK(d == b);
}

TEST_CASE("live rescale: a re-probe that changed nothing is not a change (no churn on the 10 s timer)")
{
	for (const Layout l : {Layout{2.0, 1.5}, Layout{2.0, 2.0}, Layout{1.5, 1.5}, Layout{1.6667, 1.25}}) {
		const u_weave_panel_frame f0 = resolve(l);
		const u_weave_panel_frame f1 = resolve(l);
		CHECK(u_weave_panel_frame_equal(&f0, &f1));
	}
	// A float round-trip of the scale (info stores a float) is not a change.
	u_weave_panel_frame f = {3456, 0, 5120, 2880, PANEL_W, PANEL_H, 1.5};
	u_weave_panel_frame g = f;
	g.scale = (double)(float)1.6666666;
	f.scale = 1.6666666;
	CHECK(u_weave_panel_frame_equal(&f, &g));
}
