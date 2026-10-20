// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief comp_weave_screens (#2218 item 6): a desktop-Linux / macOS XR_DXR_weave
 *        present-owner's window placed against the screens — which screen's
 *        display processor weaves which part, with which present origin,
 *        across moves, hotplug and per-screen preference changes, and the
 *        single-screen fallback.
 */

#include "util/comp_weave_screens.h"
#include "util/comp_segments.h"
#include "util/u_weave_present_origin.h"
#include "xrt/xrt_compositor.h"

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const uint64_t EDP = 0x1001; // laptop panel, OS primary
const uint64_t ZR = 0x2002;  // the 3D panel, system default (the active plug-in's)

const float LAPTOP_SCALE = 5.0f / 3.0f; // 167 %

xrt_screen
mk_screen(uint64_t id,
          uint32_t flags,
          int32_t x,
          int32_t y,
          uint32_t w,
          uint32_t h,
          uint32_t nw,
          uint32_t nh,
          float scale,
          const char *plugin)
{
	xrt_screen s{};
	s.id = id;
	s.flags = flags;
	s.confidence = 2;
	s.desktop_left = x;
	s.desktop_top = y;
	s.desktop_width = w;
	s.desktop_height = h;
	s.native_width = nw;
	s.native_height = nh;
	s.desktop_scale = scale;
	std::snprintf(s.plugin_id, sizeof(s.plugin_id), "%s", plugin);
	std::snprintf(s.device_name, sizeof(s.device_name), "%s", id == EDP ? "eDP-1" : "HDMI-A-1");
	return s;
}

/*!
 * ds1-linux under GNOME Wayland with XWayland native scaling: the X root is
 * the stage x 2 (the ceiling of the largest scale), so the laptop (2880x1800
 * at 167 %, logical 1728x1080) is 3456x2160 in X11 px and the ZR panel
 * (3840x2160 at 100 %) sits at X11 x = 3456, 7680x4320.
 */
xrt_screen_list
desk_mixed(const char *edp_plugin = "sim-display", const char *zr_plugin = "leia-sr")
{
	xrt_screen_list l{};
	l.count = 2;
	l.screens[0] = mk_screen(ZR, XRT_SCREEN_FLAG_SYSTEM_DEFAULT, 3456, 0, 7680, 4320, 3840, 2160, 1.0f, zr_plugin);
	l.screens[1] = mk_screen(EDP, XRT_SCREEN_FLAG_PRIMARY, 0, 0, 3456, 2160, 2880, 1800, LAPTOP_SCALE, edp_plugin);
	return l;
}

//! The same two outputs on an unscaled X11 session (X11 px == device px).
xrt_screen_list
desk_x11()
{
	xrt_screen_list l{};
	l.count = 2;
	l.screens[0] = mk_screen(ZR, XRT_SCREEN_FLAG_SYSTEM_DEFAULT, 2880, 0, 3840, 2160, 3840, 2160, 1.0f, "leia-sr");
	l.screens[1] = mk_screen(EDP, XRT_SCREEN_FLAG_PRIMARY, 0, 0, 2880, 1800, 2880, 1800, 1.0f, "sim-display");
	return l;
}

//! The segment manager's projection of a (projected) list: what
//! comp_vk_native_segments_set_screens builds, every screen DP-backed.
uint32_t
seg_screens(const xrt_screen_list &l, comp_segments_screen *out)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < l.count; i++) {
		const xrt_screen &s = l.screens[i];
		if (s.desktop_width == 0 || s.desktop_height == 0) {
			continue;
		}
		comp_segments_screen &c = out[n++];
		c = comp_segments_screen{};
		c.id = s.id;
		c.desktop = {s.desktop_left, s.desktop_top, s.desktop_width, s.desktop_height};
		c.native_w = s.native_width;
		c.native_h = s.native_height;
		c.is_primary = (s.flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0;
		c.has_dp_factory = true;
		std::snprintf(c.plugin_id, sizeof(c.plugin_id), "%s", s.plugin_id);
	}
	return n;
}

struct cut
{
	comp_weave_frame frame;
	xrt_screen_list proj;
	comp_seg_rect win;
	comp_segment_table t;
	bool split;
};

//! Place the window, cut the table — the per-submit path of the weave engine.
cut
place(const xrt_screen_list &l, const comp_weave_window &w)
{
	cut c{};
	c.frame = comp_weave_screens_project(&l, &w, &c.proj, &c.win);
	comp_segments_screen scr[COMP_SEGMENTS_MAX_SCREENS];
	const uint32_t n = seg_screens(c.proj, scr);
	const comp_seg_rect canvas = {0, 0, c.win.w, c.win.h};
	comp_segments_compute(&c.win, &canvas, scr, n, &c.t);
	c.split = comp_segments_table_is_split(&c.t, c.win.w, c.win.h);
	return c;
}

//! A window bound with the v12 logical origin, its device origin derived the
//! way the browser does (the monitor's logical origin x factor + offset x factor).
comp_weave_window
win_logical(int32_t mon_lx, int32_t lx, int32_t ly, float scale, uint32_t w, uint32_t h)
{
	comp_weave_window wnd{};
	wnd.x = u_wl_logical_to_px(mon_lx, scale) + u_wl_logical_to_px(lx - mon_lx, scale);
	wnd.y = u_wl_logical_to_px(ly, scale);
	wnd.w = w;
	wnd.h = h;
	wnd.logical_valid = true;
	wnd.logical_x = lx;
	wnd.logical_y = ly;
	wnd.logical_scale = scale;
	return wnd;
}

//! The single-panel phase rule (u_weave_present_origin.h) for screen @p s.
void
rule_origin(const xrt_screen &s, const comp_weave_window &w, int32_t *ox, int32_t *oy)
{
	u_weave_panel_frame p{};
	p.screen_left = s.desktop_left;
	p.screen_top = s.desktop_top;
	p.desktop_w = s.desktop_width;
	p.desktop_h = s.desktop_height;
	p.native_w = s.native_width;
	p.native_h = s.native_height;
	p.scale = s.desktop_scale;
	(void)u_weave_panel_relative_origin(&p, w.x, w.y, w.logical_valid, w.logical_x, w.logical_y, w.logical_scale,
	                                    ox, oy);
}

} // namespace

TEST_CASE("weave_screens: a window wholly on the laptop is woven by the laptop's DP")
{
	const xrt_screen_list l = desk_mixed();
	// Laptop at logical (0,0); the window's own monitor is the laptop.
	const comp_weave_window w = win_logical(0, 100, 100, LAPTOP_SCALE, 1000, 600);
	const cut c = place(l, w);
	REQUIRE(c.frame == COMP_WEAVE_FRAME_LOGICAL);

	// The laptop is 1:1 for a window at the laptop's own factor; the panel
	// (another factor) resamples it.
	CHECK(c.proj.screens[1].desktop_width == 2880);
	CHECK(c.proj.screens[1].native_width == 2880);
	CHECK(c.proj.screens[0].native_width != c.proj.screens[0].desktop_width);

	REQUIRE(c.t.count == 1);
	CHECK(c.t.seg[0].screen_id == EDP);
	CHECK_FALSE(c.t.seg[0].is_primary);
	CHECK(c.t.seg[0].screen_1to1 == COMP_SEG_1TO1_YES);
	CHECK(c.split); // not on the default screen: leaves the single-DP path

	comp_weave_segment_owner own[4];
	REQUIRE(comp_weave_screens_owners(&c.t, &l, own, 4) == 1);
	CHECK(std::strcmp(own[0].plugin_id, "sim-display") == 0);
	CHECK_FALSE(own[0].session_dp);

	// The present origin is exactly the single-panel phase rule, as if the
	// laptop were the panel.
	int32_t ox = 0, oy = 0;
	rule_origin(l.screens[1], w, &ox, &oy);
	CHECK(own[0].present_origin_x == ox);
	CHECK(own[0].present_origin_y == oy);
	CHECK(ox == 167);

	bool prim = true;
	CHECK(comp_weave_screens_eye_screen(&c.t, &prim) == EDP);
	CHECK_FALSE(prim);
}

TEST_CASE("weave_screens: a window wholly on the default screen keeps the single-DP path")
{
	const xrt_screen_list l = desk_mixed();
	// On the panel (logical origin 1728, factor 1).
	const comp_weave_window w = win_logical(1728, 2000, 200, 1.0f, 800, 600);
	const cut c = place(l, w);
	REQUIRE(c.frame == COMP_WEAVE_FRAME_LOGICAL);
	REQUIRE(c.t.count == 1);
	CHECK(c.t.seg[0].screen_id == ZR);
	CHECK(c.t.seg[0].is_primary);
	CHECK_FALSE(c.split); // byte for byte the pre-#2218 weave

	// And the origin the table carries for it is the one the engine already
	// feeds its own DP.
	int32_t ox = 0, oy = 0;
	rule_origin(l.screens[0], w, &ox, &oy);
	CHECK(c.t.seg[0].present_origin_x == ox);
	CHECK(c.t.seg[0].present_origin_y == oy);
	CHECK(ox == 272);
}

TEST_CASE("weave_screens: a window straddling the seam is cut per screen")
{
	const xrt_screen_list l = desk_mixed();
	// On the laptop, logical 1500..1980 (480 logical = 800 px at 167 %).
	const comp_weave_window w = win_logical(0, 1500, 100, LAPTOP_SCALE, 800, 600);
	const cut c = place(l, w);
	REQUIRE(c.t.count == 2);
	CHECK(c.split);
	// Left to right: the laptop's part, then the panel's.
	CHECK(c.t.seg[0].screen_id == EDP);
	CHECK(c.t.seg[1].screen_id == ZR);
	CHECK(c.t.seg[0].window_rect.x == 0);
	CHECK(c.t.seg[0].window_rect.w + c.t.seg[1].window_rect.w == 800); // no gap, no overlap
	CHECK(c.t.seg[1].window_rect.x == (int32_t)c.t.seg[0].window_rect.w);
	CHECK(c.t.seg[0].screen_1to1 == COMP_SEG_1TO1_YES);
	// The panel shows a laptop-scaled window resampled: its segment is flat
	// unless the DP tolerates it (comp_segments_decide), never phased wrong.
	CHECK(c.t.seg[1].screen_1to1 == COMP_SEG_1TO1_NO);
	CHECK(comp_segments_decide(true, false, c.t.seg[1].screen_1to1) == COMP_SEG_RENDER_FLAT_2D);

	comp_weave_segment_owner own[4];
	REQUIRE(comp_weave_screens_owners(&c.t, &l, own, 4) == 2);
	CHECK(std::strcmp(own[0].plugin_id, "sim-display") == 0);
	CHECK(std::strcmp(own[1].plugin_id, "leia-sr") == 0);
	CHECK(own[1].session_dp);
	// Majority: 420 px on the panel vs 380 on the laptop.
	bool prim = false;
	CHECK(comp_weave_screens_eye_screen(&c.t, &prim) == ZR);
	CHECK(prim);
}

TEST_CASE("weave_screens: a window moving across the seam follows the screens")
{
	const xrt_screen_list l = desk_mixed();
	// Drag left to right on the laptop at its factor, then (the compositor
	// moved the surface to the panel) at the panel's factor.
	uint64_t last_major = 0;
	int majors = 0;
	for (int32_t lx = 1000; lx <= 1800; lx += 40) {
		const bool on_panel = lx >= 1728;
		const float f = on_panel ? 1.0f : LAPTOP_SCALE;
		const uint32_t w = on_panel ? 480u : 800u; // 480 logical either way
		const uint32_t h = on_panel ? 360u : 600u;
		const comp_weave_window wnd = win_logical(on_panel ? 1728 : 0, lx, 100, f, w, h);
		const cut c = place(l, wnd);
		REQUIRE(c.frame == COMP_WEAVE_FRAME_LOGICAL);
		REQUIRE(c.t.count >= 1);
		uint32_t covered = 0;
		for (uint32_t k = 0; k < c.t.count; k++) {
			covered += c.t.seg[k].window_rect.w;
		}
		CHECK(covered == w);
		const uint64_t major = comp_weave_screens_eye_screen(&c.t, nullptr);
		if (major != last_major) {
			majors++;
			last_major = major;
		}
		// Wholly on the panel: back on the single-DP path.
		if (lx >= 1728) {
			CHECK_FALSE(c.split);
		}
	}
	CHECK(majors == 2); // laptop, then panel: one hand-over
	CHECK(last_major == ZR);
}

TEST_CASE("weave_screens: present origin is the phase rule for every 1:1 screen")
{
	const xrt_screen_list l = desk_mixed();
	for (int32_t lx = -50; lx < 1700; lx += 37) {
		for (int32_t ly = 0; ly < 900; ly += 113) {
			const comp_weave_window w = win_logical(0, lx, ly, LAPTOP_SCALE, 200, 120);
			const cut c = place(l, w);
			for (uint32_t k = 0; k < c.t.count; k++) {
				if (c.t.seg[k].screen_id != EDP) {
					continue;
				}
				int32_t ox = 0, oy = 0;
				rule_origin(l.screens[1], w, &ox, &oy);
				CHECK(c.t.seg[k].present_origin_x == ox);
				CHECK(c.t.seg[k].present_origin_y == oy);
			}
		}
	}
}

TEST_CASE("weave_screens: pure X11 places the window in the device frame")
{
	const xrt_screen_list l = desk_x11();
	comp_weave_window w{};
	w.x = 2000;
	w.y = 100;
	w.w = 1200;
	w.h = 600;
	const cut c = place(l, w);
	REQUIRE(c.frame == COMP_WEAVE_FRAME_DEVICE);
	// Identity: the screens' rects are their X11 rects.
	CHECK(std::memcmp(&c.proj, &l, sizeof(l)) == 0);
	REQUIRE(c.t.count == 2);
	CHECK(c.t.seg[0].screen_id == EDP);
	CHECK(c.t.seg[0].window_rect.w == 880);
	CHECK(c.t.seg[1].screen_id == ZR);
	CHECK(c.t.seg[1].present_origin_x == 2000 - 2880);
	CHECK(c.t.seg[1].screen_1to1 == COMP_SEG_1TO1_YES);
}

TEST_CASE("weave_screens: no logical origin on a scaled desktop refuses (single-DP weave)")
{
	const xrt_screen_list l = desk_mixed();
	comp_weave_window w{};
	w.x = 167;
	w.y = 167;
	w.w = 1000;
	w.h = 600;
	xrt_screen_list proj{};
	comp_seg_rect wr{};
	CHECK(comp_weave_screens_project(&l, &w, &proj, &wr) == COMP_WEAVE_FRAME_NONE);
	CHECK(std::memcmp(&proj, &l, sizeof(l)) == 0); // untouched copy
	// A degenerate window refuses too.
	w.w = 0;
	const xrt_screen_list x11 = desk_x11();
	CHECK(comp_weave_screens_project(&x11, &w, &proj, &wr) == COMP_WEAVE_FRAME_NONE);
}

TEST_CASE("weave_screens: single screen — never split while on it")
{
	xrt_screen_list l{};
	l.count = 1;
	l.screens[0] = mk_screen(ZR, XRT_SCREEN_FLAG_SYSTEM_DEFAULT | XRT_SCREEN_FLAG_PRIMARY, 0, 0, 3840, 2160, 3840,
	                         2160, 1.0f, "leia-sr");
	for (int32_t x = 0; x + 800 <= 3840; x += 301) {
		const comp_weave_window w = win_logical(0, x, 50, 1.0f, 800, 600);
		const cut c = place(l, w);
		REQUIRE(c.t.count == 1);
		CHECK(c.t.seg[0].is_primary);
		CHECK_FALSE(c.split);
	}
}

TEST_CASE("weave_screens: hotplug and re-claims classify the change")
{
	const xrt_screen_list a = desk_mixed();
	xrt_screen_list b = a;
	CHECK(comp_weave_screens_compare(&a, &b) == COMP_WEAVE_SCREENS_SAME);
	CHECK(comp_weave_screens_compare(nullptr, nullptr) == COMP_WEAVE_SCREENS_SAME);

	// The panel moved (display settings): geometry only, DPs kept.
	b.screens[0].desktop_left = 0;
	b.screens[1].desktop_left = 7680;
	CHECK(comp_weave_screens_compare(&a, &b) == COMP_WEAVE_SCREENS_GEOMETRY);

	// Unplugged: membership.
	xrt_screen_list c = a;
	c.count = 1;
	CHECK(comp_weave_screens_compare(&a, &c) == COMP_WEAVE_SCREENS_MEMBERSHIP);
	CHECK(comp_weave_screens_compare(&a, nullptr) == COMP_WEAVE_SCREENS_MEMBERSHIP);

	// `dp use sim-display --screen <ZR>`: the claimant changed — rebuild.
	const xrt_screen_list d = desk_mixed("sim-display", "sim-display");
	CHECK(comp_weave_screens_compare(&a, &d) == COMP_WEAVE_SCREENS_MEMBERSHIP);

	// The system default moved to the laptop.
	xrt_screen_list e = a;
	e.screens[0].flags = 0;
	e.screens[1].flags |= XRT_SCREEN_FLAG_SYSTEM_DEFAULT;
	CHECK(comp_weave_screens_compare(&a, &e) == COMP_WEAVE_SCREENS_MEMBERSHIP);

	// After the re-claim, the window on the panel is woven by sim-display.
	const comp_weave_window w = win_logical(1728, 2000, 200, 1.0f, 800, 600);
	const cut cd = place(d, w);
	comp_weave_segment_owner own[2];
	REQUIRE(comp_weave_screens_owners(&cd.t, &d, own, 2) == 1);
	CHECK(std::strcmp(own[0].plugin_id, "sim-display") == 0);
}

TEST_CASE("weave_screens: registry change detection")
{
	xrt_dp_factory_registry a{};
	a.entry_count = 2;
	a.entries[0].monitor_id = ZR;
	std::snprintf(a.entries[0].plugin_id, sizeof(a.entries[0].plugin_id), "leia-sr");
	a.entries[1].monitor_id = EDP;
	std::snprintf(a.entries[1].plugin_id, sizeof(a.entries[1].plugin_id), "sim-display");
	xrt_dp_factory_registry b = a;
	CHECK_FALSE(comp_weave_screens_registry_changed(&a, &b));
	std::snprintf(b.entries[0].plugin_id, sizeof(b.entries[0].plugin_id), "sim-display");
	CHECK(comp_weave_screens_registry_changed(&a, &b));
	b = a;
	b.entries[1].not_ready = true;
	CHECK(comp_weave_screens_registry_changed(&a, &b));
	b = a;
	b.entry_count = 1;
	CHECK(comp_weave_screens_registry_changed(&a, &b));
	CHECK_FALSE(comp_weave_screens_registry_changed(nullptr, nullptr));
}

TEST_CASE("weave_screens: an unplaced screen keeps its slot")
{
	xrt_screen_list l = desk_mixed();
	l.count = 3;
	l.screens[2] = mk_screen(0x3003, 0, 0, 0, 0, 0, 1920, 1080, 0.0f, ""); // pure-DRM record
	const comp_weave_window w = win_logical(0, 100, 100, LAPTOP_SCALE, 1000, 600);
	xrt_screen_list proj{};
	comp_seg_rect wr{};
	REQUIRE(comp_weave_screens_project(&l, &w, &proj, &wr) == COMP_WEAVE_FRAME_LOGICAL);
	CHECK(proj.count == 3);
	CHECK(proj.screens[2].desktop_width == 0); // still skipped by the manager's filter
	CHECK(proj.screens[2].id == 0x3003);
}


/*
 *
 * macOS: the CG frame (comp_weave_screens_project_cg).
 *
 * The screen list holds CGDisplayBounds (top-down POINTS, origin = the main
 * display's top-left) + each display's backing scale and native mode; the
 * present-owner reports its window in BACKING px of the display it is on
 * (points x that display's scale, y down — AppKit's y-up frame flipped by
 * the caller, XR_DXR_weave spec v7).
 *
 */

namespace {

const uint64_t MBP = 0x3003;  // the laptop panel, main display (CG origin)
const uint64_t ODY = 0x4004;  // the 3D panel, system default (the active plug-in's)
const uint64_t EXT1 = 0x5005; // a 1x external monitor

/*!
 * This box's layout (mac, 2026-10): the 3D panel LEFT of the laptop at
 * x = -1920 pt (negative CG x), both at 2x; a 1x external right of the laptop.
 *  - ODY  : 1920x1080 pt at (-1920, 0), 2x, native 3840x2160 — system default
 *  - MBP  : 1512x982 pt at (0, 0), 2x, native 3024x1964 — main display
 *  - EXT1 : 1920x1080 pt at (1512, 0), 1x, native 1920x1080
 */
xrt_screen_list
mac_desk(const char *ody_plugin = "leia-sr", const char *mbp_plugin = "sim-display")
{
	xrt_screen_list l{};
	l.count = 3;
	l.screens[0] =
	    mk_screen(ODY, XRT_SCREEN_FLAG_SYSTEM_DEFAULT, -1920, 0, 1920, 1080, 3840, 2160, 2.0f, ody_plugin);
	l.screens[1] = mk_screen(MBP, XRT_SCREEN_FLAG_PRIMARY, 0, 0, 1512, 982, 3024, 1964, 2.0f, mbp_plugin);
	l.screens[2] = mk_screen(EXT1, 0, 1512, 0, 1920, 1080, 1920, 1080, 1.0f, "sim-display");
	return l;
}

//! The window as the browser reports it: its CG point rect x the scale of its display.
comp_weave_window_cg
win_cg(int32_t x_pt, int32_t y_pt, uint32_t w_pt, uint32_t h_pt, float scale)
{
	comp_weave_window_cg w{};
	w.x = (int32_t)lround((double)x_pt * scale);
	w.y = (int32_t)lround((double)y_pt * scale);
	w.w = (uint32_t)lround((double)w_pt * scale);
	w.h = (uint32_t)lround((double)h_pt * scale);
	w.scale = scale;
	return w;
}

//! The single-panel present origin the macOS engine already feeds a DP
//! (comp_multi_weave_macos.c): window origin - lround(panel points x scale).
void
mac_rule_origin(const xrt_screen &s, const comp_weave_window_cg &w, int32_t *ox, int32_t *oy)
{
	const double sc = (double)comp_weave_screens_cg_scale(&s);
	*ox = w.x - (int32_t)lround((double)s.desktop_left * sc);
	*oy = w.y - (int32_t)lround((double)s.desktop_top * sc);
}

cut
place_cg(const xrt_screen_list &l, const comp_weave_window_cg &w)
{
	cut c{};
	c.frame = comp_weave_screens_project_cg(&l, &w, &c.proj, &c.win);
	comp_segments_screen scr[COMP_SEGMENTS_MAX_SCREENS];
	const uint32_t n = seg_screens(c.proj, scr);
	const comp_seg_rect canvas = {0, 0, c.win.w, c.win.h};
	comp_segments_compute(&c.win, &canvas, scr, n, &c.t);
	c.split = comp_segments_table_is_split(&c.t, c.win.w, c.win.h);
	return c;
}

} // namespace

TEST_CASE("weave_screens CG: a window wholly on the default (3D) panel keeps the single-DP path")
{
	const xrt_screen_list l = mac_desk();
	// On the panel, left of the main display: negative CG x.
	const comp_weave_window_cg w = win_cg(-1500, 200, 800, 500, 2.0f);
	CHECK(w.x == -3000);
	const cut c = place_cg(l, w);
	REQUIRE(c.frame == COMP_WEAVE_FRAME_CG);
	CHECK(c.proj.screens[0].desktop_left == -3840);
	CHECK(c.proj.screens[0].desktop_width == 3840);
	REQUIRE(c.t.count == 1);
	CHECK(c.t.seg[0].screen_id == ODY);
	CHECK(c.t.seg[0].is_primary);
	CHECK(c.t.seg[0].screen_1to1 == COMP_SEG_1TO1_YES);
	CHECK_FALSE(c.split); // byte for byte the pre-#2218 weave

	// The origin the table carries is exactly the one the engine feeds today.
	int32_t ox = 0, oy = 0;
	mac_rule_origin(l.screens[0], w, &ox, &oy);
	CHECK(c.t.seg[0].present_origin_x == ox);
	CHECK(c.t.seg[0].present_origin_y == oy);
	CHECK(ox == 840);
	CHECK(oy == 400);
}

TEST_CASE("weave_screens CG: a window wholly on the laptop is woven by the laptop's DP")
{
	const xrt_screen_list l = mac_desk();
	const comp_weave_window_cg w = win_cg(100, 50, 800, 500, 2.0f);
	const cut c = place_cg(l, w);
	REQUIRE(c.t.count == 1);
	CHECK(c.t.seg[0].screen_id == MBP);
	CHECK_FALSE(c.t.seg[0].is_primary);
	CHECK(c.t.seg[0].screen_1to1 == COMP_SEG_1TO1_YES);
	CHECK(c.split); // not on the default screen: leaves the single-DP path

	comp_weave_segment_owner own[4];
	REQUIRE(comp_weave_screens_owners(&c.t, &l, own, 4) == 1);
	CHECK(std::strcmp(own[0].plugin_id, "sim-display") == 0);
	CHECK_FALSE(own[0].session_dp);
	CHECK(own[0].present_origin_x == 200);
	CHECK(own[0].present_origin_y == 100);

	bool prim = true;
	CHECK(comp_weave_screens_eye_screen(&c.t, &prim) == MBP);
	CHECK_FALSE(prim);
}

TEST_CASE("weave_screens CG: straddling the 3D panel / laptop seam (both 2x) is cut per screen")
{
	const xrt_screen_list l = mac_desk();
	// 300 pt on the panel, 500 pt on the laptop.
	const comp_weave_window_cg w = win_cg(-300, 100, 800, 400, 2.0f);
	const cut c = place_cg(l, w);
	REQUIRE(c.t.count == 2);
	CHECK(c.split);
	// Left to right: the panel's part, then the laptop's.
	CHECK(c.t.seg[0].screen_id == ODY);
	CHECK(c.t.seg[1].screen_id == MBP);
	CHECK(c.t.seg[0].window_rect.x == 0);
	CHECK(c.t.seg[0].window_rect.w == 600);
	CHECK(c.t.seg[1].window_rect.x == 600);
	CHECK(c.t.seg[0].window_rect.w + c.t.seg[1].window_rect.w == 1600); // no gap, no overlap
	CHECK(c.t.seg[0].screen_1to1 == COMP_SEG_1TO1_YES);
	CHECK(c.t.seg[1].screen_1to1 == COMP_SEG_1TO1_YES);

	// Present origin per screen: each screen's own backing px. phase =
	// origin + canvas offset = the segment's top-left on that panel.
	int32_t ox = 0, oy = 0;
	mac_rule_origin(l.screens[0], w, &ox, &oy);
	CHECK(c.t.seg[0].present_origin_x == ox);
	CHECK(ox == 3240); // 3840 - 600: the window's left edge on the panel
	mac_rule_origin(l.screens[1], w, &ox, &oy);
	CHECK(c.t.seg[1].present_origin_x == ox);
	CHECK(ox == -600);
	CHECK(c.t.seg[1].present_origin_x + c.t.seg[1].window_rect.x == 0); // lands at the laptop's x = 0

	comp_weave_segment_owner own[4];
	REQUIRE(comp_weave_screens_owners(&c.t, &l, own, 4) == 2);
	CHECK(std::strcmp(own[0].plugin_id, "leia-sr") == 0);
	CHECK(own[0].session_dp);
	CHECK(std::strcmp(own[1].plugin_id, "sim-display") == 0);
	// Majority: 1000 px on the laptop.
	bool prim = true;
	CHECK(comp_weave_screens_eye_screen(&c.t, &prim) == MBP);
	CHECK_FALSE(prim);
}

TEST_CASE("weave_screens CG: a 2x window straddling onto a 1x external resamples that part")
{
	const xrt_screen_list l = mac_desk();
	// On the laptop (2x), its right 200 pt hanging over the 1x external.
	const comp_weave_window_cg w = win_cg(1112, 100, 600, 400, 2.0f);
	const cut c = place_cg(l, w);
	REQUIRE(c.t.count == 2);
	CHECK(c.t.seg[0].screen_id == MBP);
	CHECK(c.t.seg[1].screen_id == EXT1);
	CHECK(c.t.seg[0].window_rect.w == 800);
	CHECK(c.t.seg[1].window_rect.w == 400);
	CHECK(c.t.seg[0].screen_1to1 == COMP_SEG_1TO1_YES);
	// The WindowServer resamples the 2x window on the 1x display: flat unless
	// its DP tolerates it, never phased in the wrong units.
	CHECK(c.t.seg[1].screen_1to1 == COMP_SEG_1TO1_NO);
	CHECK(comp_segments_decide(true, false, c.t.seg[1].screen_1to1) == COMP_SEG_RENDER_FLAT_2D);
	CHECK(comp_segments_decide(true, true, c.t.seg[1].screen_1to1) == COMP_SEG_RENDER_WEAVE);
}

TEST_CASE("weave_screens CG: a 1x window on the external sees the 2x displays resampled")
{
	const xrt_screen_list l = mac_desk();
	const comp_weave_window_cg w = win_cg(1600, 100, 600, 400, 1.0f);
	const cut c = place_cg(l, w);
	CHECK(c.proj.screens[2].native_width == c.proj.screens[2].desktop_width); // EXT1 1:1 at 1x
	CHECK(c.proj.screens[1].native_width != c.proj.screens[1].desktop_width); // MBP is 2x
	CHECK(c.proj.screens[0].native_width != c.proj.screens[0].desktop_width); // ODY is 2x
	REQUIRE(c.t.count == 1);
	CHECK(c.t.seg[0].screen_id == EXT1);
	CHECK(c.t.seg[0].screen_1to1 == COMP_SEG_1TO1_YES);
	CHECK(c.t.seg[0].present_origin_x == 88);
	CHECK(c.split);
}

TEST_CASE("weave_screens CG: a 3D panel above the laptop (flipped AppKit y, negative CG y)")
{
	// Panel ABOVE the main display: AppKit puts it at y = +982 (y up from the
	// main display's bottom-left), CoreGraphics at y = -1080 (y down from its
	// top-left). The screen list and the window are both CG.
	xrt_screen_list l{};
	l.count = 2;
	l.screens[0] =
	    mk_screen(ODY, XRT_SCREEN_FLAG_SYSTEM_DEFAULT, 0, -1080, 1920, 1080, 3840, 2160, 2.0f, "leia-sr");
	l.screens[1] = mk_screen(MBP, XRT_SCREEN_FLAG_PRIMARY, 0, 0, 1512, 982, 3024, 1964, 2.0f, "sim-display");

	// An NSWindow content frame in AppKit points: (200, 882) 600x300, i.e.
	// its top 200 pt on the panel, its bottom 100 pt on the laptop.
	const int32_t main_h_pt = 982;
	const int32_t ak_x = 200, ak_y = 882, wpt = 600, hpt = 300;
	const int32_t cg_y = main_h_pt - (ak_y + hpt); // the caller's flip
	CHECK(cg_y == -200);
	const comp_weave_window_cg w = win_cg(ak_x, cg_y, wpt, hpt, 2.0f);
	const cut c = place_cg(l, w);
	REQUIRE(c.t.count == 2);
	CHECK(c.split);
	// Top to bottom: the panel's band, then the laptop's — full width each.
	CHECK(c.t.seg[0].screen_id == ODY);
	CHECK(c.t.seg[0].window_rect.y == 0);
	CHECK(c.t.seg[0].window_rect.h == 400);
	CHECK(c.t.seg[1].screen_id == MBP);
	CHECK(c.t.seg[1].window_rect.y == 400);
	CHECK(c.t.seg[1].window_rect.h == 200);
	// The panel band lands at the panel's bottom rows: origin 2160 - 400.
	CHECK(c.t.seg[0].present_origin_y == 1760);
	CHECK(c.t.seg[1].present_origin_y == -400);
	CHECK(c.t.seg[1].present_origin_y + c.t.seg[1].window_rect.y == 0);
}

TEST_CASE("weave_screens CG: a window at a half-point position keeps an exact present origin")
{
	const xrt_screen_list l = mac_desk();
	comp_weave_window_cg w = win_cg(-1000, 100, 400, 300, 2.0f);
	w.x += 1; // backing-aligned at a half point
	w.y += 1;
	const cut c = place_cg(l, w);
	REQUIRE(c.t.count == 1);
	int32_t ox = 0, oy = 0;
	mac_rule_origin(l.screens[0], w, &ox, &oy);
	CHECK(c.t.seg[0].present_origin_x == ox);
	CHECK(c.t.seg[0].present_origin_y == oy);
	CHECK(ox == 1841);
	CHECK(oy == 201);
}

TEST_CASE("weave_screens CG: present origin is the engine's phase rule on every 1:1 screen")
{
	const xrt_screen_list l = mac_desk();
	for (int32_t x = -2100; x < 1500; x += 97) {
		for (int32_t y = -50; y < 900; y += 131) {
			const comp_weave_window_cg w = win_cg(x, y, 400, 240, 2.0f);
			const cut c = place_cg(l, w);
			uint32_t covered = 0;
			for (uint32_t k = 0; k < c.t.count; k++) {
				const comp_segment &g = c.t.seg[k];
				if (g.screen_1to1 != COMP_SEG_1TO1_YES) {
					continue;
				}
				const xrt_screen *s = xrt_screen_list_find(&l, g.screen_id);
				REQUIRE(s != nullptr);
				int32_t ox = 0, oy = 0;
				mac_rule_origin(*s, w, &ox, &oy);
				CHECK(g.present_origin_x == ox);
				CHECK(g.present_origin_y == oy);
				covered += g.window_rect.w * g.window_rect.h;
			}
			CHECK(covered <= w.w * w.h);
		}
	}
}

TEST_CASE("weave_screens CG: a single screen is never split, a degenerate window or scale refuses")
{
	xrt_screen_list l{};
	l.count = 1;
	l.screens[0] = mk_screen(ODY, XRT_SCREEN_FLAG_SYSTEM_DEFAULT | XRT_SCREEN_FLAG_PRIMARY, 0, 0, 1920, 1080, 3840,
	                         2160, 2.0f, "leia-sr");
	for (int32_t x = 0; x + 400 <= 1920; x += 211) {
		const cut c = place_cg(l, win_cg(x, 30, 400, 300, 2.0f));
		REQUIRE(c.t.count == 1);
		CHECK(c.t.seg[0].is_primary);
		CHECK_FALSE(c.split);
	}
	const xrt_screen_list m = mac_desk();
	xrt_screen_list proj{};
	comp_seg_rect wr{};
	comp_weave_window_cg w = win_cg(0, 0, 400, 300, 2.0f);
	w.scale = 0.0f;
	CHECK(comp_weave_screens_project_cg(&m, &w, &proj, &wr) == COMP_WEAVE_FRAME_NONE);
	CHECK(std::memcmp(&proj, &m, sizeof(m)) == 0); // untouched copy
	w = win_cg(0, 0, 400, 300, 2.0f);
	w.w = 0;
	CHECK(comp_weave_screens_project_cg(&m, &w, &proj, &wr) == COMP_WEAVE_FRAME_NONE);
}

TEST_CASE("weave_screens CG: a 'looks like' scaled mode is not 1:1 at its own scale")
{
	xrt_screen_list l = mac_desk();
	// The laptop at "more space": 1800x1169 pt at 2x = 3600x2338 backing,
	// down-sampled by the WindowServer to the 3024x1964 panel.
	l.screens[1].desktop_width = 1800;
	l.screens[1].desktop_height = 1169;
	l.screens[2].desktop_left = 1800;
	const cut c = place_cg(l, win_cg(100, 100, 400, 300, 2.0f));
	REQUIRE(c.t.count == 1);
	CHECK(c.t.seg[0].screen_id == MBP);
	CHECK(c.t.seg[0].screen_1to1 == COMP_SEG_1TO1_NO);
}

TEST_CASE("weave_screens: an EDID physical-size refinement alone is not a new table (#2097)")
{
	const xrt_screen_list a = mac_desk();
	xrt_screen_list b = a;
	// CoreGraphics' 598x341 mm, then IOKit's EDID 600x340 mm ~1 s later.
	b.screens[0].physical_width_mm = 600;
	b.screens[0].physical_height_mm = 340;
	CHECK(comp_weave_screens_compare(&a, &b) == COMP_WEAVE_SCREENS_PHYSICAL_SIZE);
	// The derived metres move with it: still only that.
	b.screens[0].info.width_m = 0.600f;
	CHECK(comp_weave_screens_compare(&a, &b) == COMP_WEAVE_SCREENS_PHYSICAL_SIZE);
	// Combined with a real move it is a geometry change...
	b.screens[0].desktop_left = -1800;
	CHECK(comp_weave_screens_compare(&a, &b) == COMP_WEAVE_SCREENS_GEOMETRY);
	// ...and a re-claim wins over both.
	b.screens[0].confidence = 1;
	CHECK(comp_weave_screens_compare(&a, &b) == COMP_WEAVE_SCREENS_MEMBERSHIP);
}
