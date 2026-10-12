// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief XR_DXR_weave v19 (#1884, ADR-047 Amendment 4): a present-owner's
 *        weave rects split per screen. The segment table the service cuts the
 *        bound window into (comp_segments_compute), the per-rect parts the
 *        client derives from it (u_weave_rect_parts), and the wire message
 *        that carries the table with every screen's eyes. The Amendment 4
 *        addendum: the screen whose eyes and geometry frame an element's
 *        zone-scoped locate (u_weave_zone_screen_segment +
 *        u_weave_segment_window_metrics), and the submit reply's cheap split
 *        signal (u_weave_segments_fetch_needed). Amendment 9: the in-process /
 *        direct-IPC twin of that rule (oxr_segment_views_zone_segment).
 *        Multi-screen § 8 (decided 2026-10-12): a straddling zone's one view
 *        set is framed from the screen holding most of the ZONE on every path
 *        (u_weave_rect_majority_segment), and its parts get a view set each
 *        (u_weave_zone_part_table, oxr_segment_views_zone_parts, XrDisplayZonePartsDXR)
 *        under a per-frame budget (oxr_zone_parts_budget_admit).
 */

#include "util/comp_segments.h"
#include "util/comp_segments_route.h"
#include "util/u_canvas.h"
#include "util/u_weave_rect_parts.h"
#include "util/u_segment_layout.h"

#include "oxr_segment_views.h"

#ifdef DXR_TESTS_HAVE_IPC_PROTOCOL
#include "shared/ipc_protocol.h"
#include "ipc_protocol_generated.h"
#endif

#include "catch_amalgamated.hpp"

#include <cstddef>
#include <cstring>

namespace {

/*
 * Two 1920x1080 3D screens side by side: the primary (system default) at the
 * desktop origin, the second to its right.
 */
const uint64_t PRIMARY_ID = 0x1001;
const uint64_t SECOND_ID = 0x2002;

void
two_screens(comp_segments_screen out[2])
{
	std::memset(out, 0, sizeof(comp_segments_screen) * 2);
	out[0].id = PRIMARY_ID;
	out[0].desktop = {0, 0, 1920, 1080};
	out[0].native_w = 1920;
	out[0].native_h = 1080;
	out[0].is_primary = true;
	out[0].has_dp_factory = true;
	out[1].id = SECOND_ID;
	out[1].desktop = {1920, 0, 1920, 1080};
	out[1].native_w = 1920;
	out[1].native_h = 1080;
	out[1].has_dp_factory = true;
}

xrt_rect
xr(int x, int y, int w, int h)
{
	xrt_rect r;
	r.offset.w = x;
	r.offset.h = y;
	r.extent.w = w;
	r.extent.h = h;
	return r;
}

xrt_rect
from_seg(const comp_seg_rect &r)
{
	return xr(r.x, r.y, (int)r.w, (int)r.h);
}

/*
 * The table the service publishes for a weave (what comp_d3d11_segments_get_metrics
 * makes of a split table): the geometry, eyes left for the caller to fill. A
 * table that is not split publishes count 0 — that is the single-DP weave.
 */
xrt_segment_metrics
publish(const comp_seg_rect &window_desktop, const comp_segments_screen *screens, uint32_t n)
{
	xrt_segment_metrics m;
	std::memset(&m, 0, sizeof(m));
	if (n < 2) {
		return m; // the manager is enabled only with two or more screens (comp_d3d11_segments_set_screens)
	}
	comp_segment_table t;
	const comp_seg_rect canvas = {0, 0, window_desktop.w, window_desktop.h};
	comp_segments_compute(&window_desktop, &canvas, screens, n, &t);
	if (!comp_segments_table_is_split(&t, window_desktop.w, window_desktop.h) || t.count > XRT_MAX_SEGMENTS) {
		return m;
	}
	for (uint32_t k = 0; k < t.count; k++) {
		m.seg[k].screen_id = t.seg[k].screen_id;
		m.seg[k].window_rect = from_seg(t.seg[k].window_rect);
		m.seg[k].screen_rect = from_seg(t.seg[k].screen_rect);
		m.seg[k].is_primary = t.seg[k].is_primary;
		m.seg[k].has_dp = true;
		m.seg[k].woven = true;
	}
	m.count = t.count;
	m.canvas = from_seg(canvas);
	m.window_screen_left = window_desktop.x;
	m.window_screen_top = window_desktop.y;
	m.window_pixel_width = window_desktop.w;
	m.window_pixel_height = window_desktop.h;
	return m;
}

void
check_rect(const xrt_rect &r, int x, int y, int w, int h)
{
	CHECK(r.offset.w == x);
	CHECK(r.offset.h == y);
	CHECK(r.extent.w == w);
	CHECK(r.extent.h == h);
}

// A 1920x1080 panel's physical size, metres (the 15.6" class).
const float PANEL_W_M = 0.3442f;
const float PANEL_H_M = 0.1936f;

/*
 * What comp_d3d11_segments_get_metrics adds to the geometry: every screen's
 * desktop rect, physical size and nominal viewer, and each screen's own eyes
 * (weave_seg_publish / comp_d3d11_service_get_zone_weave_screen). Screen i's
 * viewer sits at x = 0.1 m * i in ITS OWN display space, so the two eye sets
 * are told apart.
 */
void
with_geometry(xrt_segment_metrics &m, const comp_segments_screen *screens, uint32_t n)
{
	for (uint32_t k = 0; k < m.count; k++) {
		xrt_segment_metric &g = m.seg[k];
		for (uint32_t i = 0; i < n; i++) {
			if (screens[i].id != g.screen_id) {
				continue;
			}
			g.screen_desktop_left = screens[i].desktop.x;
			g.screen_desktop_top = screens[i].desktop.y;
			g.screen_desktop_width = screens[i].desktop.w;
			g.screen_desktop_height = screens[i].desktop.h;
			g.screen_width_m = PANEL_W_M;
			g.screen_height_m = PANEL_H_M;
			g.nominal_viewer_z_m = 0.6f + 0.05f * (float)i;
			g.eyes.count = 2;
			g.eyes.valid = true;
			g.eyes.is_tracking = true;
			g.eyes.eyes[0].x = -0.032f + 0.1f * (float)i;
			g.eyes.eyes[0].z = 0.6f;
			g.eyes.eyes[1].x = 0.032f + 0.1f * (float)i;
			g.eyes.eyes[1].z = 0.6f;
			g.have_eyes = true;
		}
	}
}

//! The element's canvas as a locate frames it: the window metrics, rebased to the zone.
xrt_window_metrics
zone_canvas(const xrt_window_metrics &window, const xrt_rect &zone)
{
	xrt_window_metrics wm = window;
	u_canvas_rect c = {};
	c.valid = true;
	c.x = zone.offset.w;
	c.y = zone.offset.h;
	c.w = (uint32_t)zone.extent.w;
	c.h = (uint32_t)zone.extent.h;
	u_canvas_apply_to_metrics(&wm, &c);
	return wm;
}

} // namespace

TEST_CASE("weave segments: a rect straddling the seam is cut into one part per screen")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	// A 1000x600 window whose client area starts at desktop x = 1400: its
	// first 520 px are on the primary screen, the rest on the second.
	const comp_seg_rect win = {1400, 100, 1000, 600};
	const xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	CHECK(m.seg[0].screen_id == PRIMARY_ID);
	CHECK(m.seg[0].is_primary);
	check_rect(m.seg[0].window_rect, 0, 0, 520, 600);
	check_rect(m.seg[0].screen_rect, 1400, 100, 520, 600);
	CHECK(m.seg[1].screen_id == SECOND_ID);
	check_rect(m.seg[1].window_rect, 520, 0, 480, 600);
	check_rect(m.seg[1].screen_rect, 0, 100, 480, 600);

	// An inline-3D element across the seam (window x 400..700).
	const xrt_rect rects[] = {xr(400, 50, 300, 200)};
	u_weave_rect_part parts[8];
	const uint32_t n = u_weave_rect_parts(&m, rects, 1, parts, 8);
	REQUIRE(n == 2);

	CHECK(parts[0].rect_index == 0);
	CHECK(parts[0].segment_index == 0);
	check_rect(parts[0].window_rect, 400, 50, 120, 200);
	check_rect(parts[0].rect_rel, 0, 0, 120, 200);
	check_rect(parts[0].screen_rect, 1800, 150, 120, 200);

	CHECK(parts[1].rect_index == 0);
	CHECK(parts[1].segment_index == 1);
	check_rect(parts[1].window_rect, 520, 50, 180, 200);
	check_rect(parts[1].rect_rel, 120, 0, 180, 200);
	check_rect(parts[1].screen_rect, 0, 150, 180, 200);

	// The two parts tile the rect exactly: no gap, no overlap at the seam.
	CHECK(parts[0].rect_rel.extent.w + parts[1].rect_rel.extent.w == rects[0].extent.w);
	CHECK(parts[0].rect_rel.offset.w + parts[0].rect_rel.extent.w == parts[1].rect_rel.offset.w);
}

TEST_CASE("weave segments: a rect wholly on one screen of a split window is one part, the whole rect")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1400, 100, 1000, 600};
	const xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);

	const xrt_rect rects[] = {xr(10, 10, 100, 100), xr(800, 300, 150, 120)};
	u_weave_rect_part parts[8];
	const uint32_t n = u_weave_rect_parts(&m, rects, 2, parts, 8);
	REQUIRE(n == 2);
	CHECK(parts[0].rect_index == 0);
	CHECK(parts[0].segment_index == 0);
	check_rect(parts[0].window_rect, 10, 10, 100, 100);
	check_rect(parts[0].rect_rel, 0, 0, 100, 100);
	check_rect(parts[0].screen_rect, 1410, 110, 100, 100);
	CHECK(parts[1].rect_index == 1);
	CHECK(parts[1].segment_index == 1);
	check_rect(parts[1].window_rect, 800, 300, 150, 120);
	check_rect(parts[1].rect_rel, 0, 0, 150, 120);
	check_rect(parts[1].screen_rect, 280, 400, 150, 120);
}

TEST_CASE("weave segments: a window on the primary screen alone is not split — no parts, base eyes")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	// Wholly on the primary: the single-DP weave, byte for byte (count 0).
	const comp_seg_rect win = {100, 100, 1000, 600};
	const xrt_segment_metrics m = publish(win, screens, 2);
	CHECK(m.count == 0);
	const xrt_rect rects[] = {xr(400, 50, 300, 200)};
	u_weave_rect_part parts[8];
	CHECK(u_weave_rect_parts(&m, rects, 1, parts, 8) == 0);

	// A one-screen system can never split.
	const xrt_segment_metrics one = publish({1400, 100, 1000, 600}, screens, 1);
	CHECK(one.count == 0);
}

TEST_CASE("weave segments: a window wholly on the second screen is still per screen")
{
	// Off the primary entirely: one segment, but not the primary's — the
	// service weaves it with the second screen's DP and reports it.
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {2100, 100, 800, 600};
	const xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 1);
	CHECK(m.seg[0].screen_id == SECOND_ID);
	CHECK_FALSE(m.seg[0].is_primary);
	const xrt_rect rects[] = {xr(0, 0, 800, 600)};
	u_weave_rect_part parts[4];
	REQUIRE(u_weave_rect_parts(&m, rects, 1, parts, 4) == 1);
	check_rect(parts[0].screen_rect, 180, 100, 800, 600);
}

TEST_CASE("weave segments: a screen left of the primary (negative desktop x)")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	screens[1].desktop = {-1920, 0, 1920, 1080};
	const comp_seg_rect win = {-300, 0, 1000, 500};
	const xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	// Left to right: the left screen first.
	CHECK(m.seg[0].screen_id == SECOND_ID);
	CHECK(m.seg[1].screen_id == PRIMARY_ID);
	const xrt_rect rects[] = {xr(200, 0, 200, 100)};
	u_weave_rect_part parts[4];
	REQUIRE(u_weave_rect_parts(&m, rects, 1, parts, 4) == 2);
	check_rect(parts[0].window_rect, 200, 0, 100, 100);
	check_rect(parts[0].screen_rect, 1820, 0, 100, 100);
	check_rect(parts[1].window_rect, 300, 0, 100, 100);
	check_rect(parts[1].screen_rect, 0, 0, 100, 100);
}

TEST_CASE("weave segments: empty and off-screen rects yield nothing; the cap holds")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const xrt_segment_metrics m = publish({1400, 100, 1000, 600}, screens, 2);
	REQUIRE(m.count == 2);
	u_weave_rect_part parts[4];

	const xrt_rect empty[] = {xr(400, 50, 0, 200), xr(400, 50, 300, -1)};
	CHECK(u_weave_rect_parts(&m, empty, 2, parts, 4) == 0);

	const xrt_rect outside[] = {xr(1200, 0, 100, 100)};
	CHECK(u_weave_rect_parts(&m, outside, 1, parts, 4) == 0);

	const xrt_rect two_straddling[] = {xr(400, 0, 300, 100), xr(400, 200, 300, 100)};
	CHECK(u_weave_rect_parts(&m, two_straddling, 2, parts, 3) == 3); // the 4th part dropped
	CHECK(u_weave_rect_parts(&m, two_straddling, 2, parts, 4) == 4);

	// A count past the per-segment view sets is "not segmented".
	xrt_segment_metrics bad = m;
	bad.count = XRT_MAX_SEGMENTS + 1;
	CHECK(u_weave_rect_parts(&bad, two_straddling, 2, parts, 4) == 0);
}

/*
 * ADR-047 Amendment 4, addendum: a present-owner's zone-scoped locate (an
 * inline-3D element, XR_DXR_view_rig + XrDisplayZoneDXR) of a window woven per
 * screen. The server (ipc_try_get_sr_view_poses) asks
 * u_weave_zone_screen_segment which screen frames the element; when one does,
 * it locates against u_weave_segment_window_metrics rebased to the zone, with
 * that segment's eyes. -1 keeps the session's own (primary) framing.
 */

TEST_CASE("element views: an element wholly on screen 2 is framed from screen 2's eyes and geometry")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1400, 100, 1000, 600};
	xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	with_geometry(m, screens, 2);

	// Window x 800..950: wholly on the second screen (its seam is at x 520).
	const xrt_rect zone = xr(800, 300, 150, 120);
	const int32_t k = u_weave_zone_screen_segment(&m, &zone, win.w, win.h);
	REQUIRE(k == 1);
	CHECK(m.seg[k].screen_id == SECOND_ID);

	// Screen 2's eyes, not the primary's.
	CHECK(m.seg[k].eyes.eyes[0].x == Catch::Approx(0.068f));
	CHECK(m.seg[k].eyes.eyes[1].x == Catch::Approx(0.132f));
	CHECK(m.seg[k].nominal_viewer_z_m == Catch::Approx(0.65f));

	xrt_window_metrics whole;
	REQUIRE(u_weave_segment_window_metrics(&m, (uint32_t)k, &whole));
	CHECK(whole.display_screen_left == 1920);
	CHECK(whole.display_pixel_width == 1920u);
	CHECK(whole.display_width_m == Catch::Approx(PANEL_W_M));

	// Rebased to the element: Kooima against screen 2's physical size, the
	// element where it sits ON screen 2 (its on-screen rect, as the weave
	// parts report it, is 280,400 150x120).
	const xrt_window_metrics el = zone_canvas(whole, zone);
	const float px = PANEL_W_M / 1920.0f;
	const float py = PANEL_H_M / 1080.0f;
	u_weave_rect_part part;
	REQUIRE(u_weave_rect_parts(&m, &zone, 1, &part, 1) == 1);
	check_rect(part.screen_rect, 280, 400, 150, 120);
	CHECK(el.window_screen_left - el.display_screen_left == part.screen_rect.offset.w);
	CHECK(el.window_screen_top - el.display_screen_top == part.screen_rect.offset.h);
	CHECK(el.window_width_m == Catch::Approx(150.0f * px));
	CHECK(el.window_height_m == Catch::Approx(120.0f * py));
	CHECK(el.window_center_offset_x_m == Catch::Approx((280.0f + 75.0f - 960.0f) * px));
	CHECK(el.window_center_offset_y_m == Catch::Approx(-((400.0f + 60.0f - 540.0f) * py)));

	// What the primary framing put there instead: the same pixels measured
	// from the PRIMARY screen's centre, one panel (1920 px) to the left of
	// where screen 2's eyes are measured from.
	xrt_window_metrics primary_whole;
	REQUIRE(u_weave_segment_window_metrics(&m, 0, &primary_whole));
	const xrt_window_metrics wrong = zone_canvas(primary_whole, zone);
	CHECK(wrong.window_center_offset_x_m - el.window_center_offset_x_m == Catch::Approx(1920.0f * px));
}

TEST_CASE("element views: a straddling element is framed from the screen holding most of it")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1400, 100, 1000, 600};
	xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	with_geometry(m, screens, 2);

	// Window x 400..700 crosses the seam at 520: 120 px on the primary, 180 on
	// screen 2. Its one view set comes from screen 2 (multi-screen § 8,
	// decided 2026-10-12: it kept the primary's before).
	const xrt_rect straddle = xr(400, 50, 300, 200);
	CHECK(u_weave_rect_single_segment(&m, &straddle) == -1);
	CHECK(u_weave_rect_majority_segment(&m, &straddle) == 1);
	CHECK(u_weave_zone_screen_segment(&m, &straddle, win.w, win.h) == 1);

	// Mostly on the primary (one pixel across the seam): the session's own framing.
	const xrt_rect sliver = xr(420, 50, 101, 200);
	CHECK(u_weave_rect_majority_segment(&m, &sliver) == 0);
	CHECK(u_weave_zone_screen_segment(&m, &sliver, win.w, win.h) == -1);
	// Flush against the seam, the element is wholly on the primary.
	const xrt_rect flush = xr(420, 50, 100, 200);
	CHECK(u_weave_rect_single_segment(&m, &flush) == 0);
	CHECK(u_weave_zone_screen_segment(&m, &flush, win.w, win.h) == -1);
	// An exact tie goes to the primary.
	const xrt_rect tie = xr(420, 50, 200, 200);
	CHECK(u_weave_rect_majority_segment(&m, &tie) == 0);
	CHECK(u_weave_zone_screen_segment(&m, &tie, win.w, win.h) == -1);
	// Off the window, or no table: no screen.
	const xrt_rect off = xr(2000, 50, 10, 10);
	CHECK(u_weave_rect_majority_segment(&m, &off) == -1);
	xrt_segment_metrics none;
	std::memset(&none, 0, sizeof(none));
	CHECK(u_weave_rect_majority_segment(&none, &straddle) == -1);
}

TEST_CASE("element views: an element on the primary screen of a split window keeps the session's framing")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1400, 100, 1000, 600};
	xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	with_geometry(m, screens, 2);
	const xrt_rect zone = xr(10, 10, 100, 100);
	CHECK(u_weave_rect_single_segment(&m, &zone) == 0);
	CHECK(u_weave_zone_screen_segment(&m, &zone, win.w, win.h) == -1);
}

TEST_CASE("element views: a window on one screen is unchanged and costs no table fetch")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {100, 100, 1000, 600};
	const xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 0); // the single-DP weave: weave_submit's reply says 0

	const xrt_rect zone = xr(400, 50, 300, 200);
	CHECK(u_weave_zone_screen_segment(&m, &zone, win.w, win.h) == -1);

	// The cheap split signal: the state tracker fetches the per-screen table
	// (weave_get_segments) only when the submit that just landed was woven per
	// screen. A window on one screen (count 0), and every window on a
	// single-screen box (which never enables segments), sends nothing more.
	CHECK_FALSE(u_weave_segments_fetch_needed(1, m.count));
	CHECK_FALSE(u_weave_segments_fetch_needed(32, 0));
	// The legacy single-rect submit is never segmented.
	CHECK_FALSE(u_weave_segments_fetch_needed(0, 2));
	// A split window does fetch; a corrupt count past the bound does not.
	const xrt_segment_metrics split = publish({1400, 100, 1000, 600}, screens, 2);
	CHECK(u_weave_segments_fetch_needed(1, split.count));
	CHECK_FALSE(u_weave_segments_fetch_needed(1, XRT_MAX_SEGMENTS + 1));
}

TEST_CASE("element views: a window wholly on screen 2 frames every element from screen 2")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {2100, 100, 800, 600};
	xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 1);
	REQUIRE_FALSE(m.seg[0].is_primary);
	with_geometry(m, screens, 2);

	const xrt_rect zone = xr(100, 100, 200, 150);
	REQUIRE(u_weave_zone_screen_segment(&m, &zone, win.w, win.h) == 0);
	xrt_window_metrics whole;
	REQUIRE(u_weave_segment_window_metrics(&m, 0, &whole));
	const xrt_window_metrics el = zone_canvas(whole, zone);
	// On screen 2 at (180 + 100, 100 + 100).
	CHECK(el.window_screen_left - el.display_screen_left == 280);
	CHECK(el.window_screen_top - el.display_screen_top == 200);
}

TEST_CASE("element views: a resize in flight or a screen of unknown size keeps the session's framing")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1400, 100, 1000, 600};
	xrt_segment_metrics m = publish(win, screens, 2);
	with_geometry(m, screens, 2);
	const xrt_rect zone = xr(800, 300, 150, 120);
	REQUIRE(u_weave_zone_screen_segment(&m, &zone, win.w, win.h) == 1);

	// The table was cut from a 1000x600 window; the live one is already larger.
	CHECK(u_weave_zone_screen_segment(&m, &zone, 1100, 600) == -1);

	xrt_segment_metrics no_size = m;
	no_size.seg[1].screen_width_m = 0.0f;
	CHECK(u_weave_zone_screen_segment(&no_size, &zone, win.w, win.h) == -1);
	xrt_window_metrics wm;
	CHECK_FALSE(u_weave_segment_window_metrics(&no_size, 1, &wm));
	CHECK_FALSE(wm.valid);
}

TEST_CASE("element views: the server's window metrics are the per-segment views' own")
{
	// u_weave_segment_window_metrics (aux, for the IPC server) and
	// oxr_segment_views_whole_window_metrics (the state tracker's per-segment
	// views) must frame a screen identically: two layouts, every segment.
	comp_segments_screen screens[2];
	two_screens(screens);
	for (int layout = 0; layout < 2; layout++) {
		if (layout == 1) {
			screens[1].desktop = {-1920, 0, 1920, 1080};
		}
		const comp_seg_rect win =
		    layout == 0 ? comp_seg_rect{1400, 100, 1000, 600} : comp_seg_rect{-300, 40, 1000, 500};
		xrt_segment_metrics m = publish(win, screens, 2);
		REQUIRE(m.count == 2);
		with_geometry(m, screens, 2);
		for (uint32_t k = 0; k < m.count; k++) {
			xrt_window_metrics a;
			xrt_window_metrics b;
			REQUIRE(u_weave_segment_window_metrics(&m, k, &a));
			oxr_segment_views_whole_window_metrics(&m, k, &b);
			REQUIRE(b.valid);
			CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
		}
	}
}

/*
 * The in-process / direct-IPC twin (displayxr-browser-pvt#272): a zone-scoped
 * locate on a session whose OWN compositor segments its window (in-process
 * D3D11 / D3D12 / GL / Vulkan, or a direct IPC client of the D3D11 service).
 * oxr_session_locate_views keeps one view set for it and asks
 * oxr_segment_views_zone_segment which screen frames it: the zone's own screen
 * when the zone lies wholly on one, else the majority (the pre-#272 answer).
 * Unlike the present-owner rule it may answer the PRIMARY screen: an in-process
 * window's one view set is framed from the majority screen, so a zone on the
 * primary of a window mostly on screen 2 was framed from screen 2 before.
 */

TEST_CASE("zone framing: a zone wholly on screen 2 is framed from screen 2, as for a present-owner")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1400, 100, 1000, 600}; // seam at window x 520
	xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	with_geometry(m, screens, 2);
	const uint32_t mj = oxr_segment_views_majority(&m);
	REQUIRE(m.seg[mj].is_primary); // 520 px on the primary, 480 on screen 2

	const xrt_rect zone = xr(800, 300, 150, 120);
	const uint32_t k = oxr_segment_views_zone_segment(&m, &zone, mj);
	REQUIRE(k == 1);
	CHECK(m.seg[k].screen_id == SECOND_ID);
	// The present-owner rule picks the same screen.
	CHECK(u_weave_zone_screen_segment(&m, &zone, win.w, win.h) == (int32_t)k);

	// The canvas the locate frames: the whole window relative to screen 2,
	// rebased to the zone - the element where it sits ON screen 2.
	xrt_window_metrics whole;
	oxr_segment_views_whole_window_metrics(&m, k, &whole);
	REQUIRE(whole.valid);
	const xrt_window_metrics el = zone_canvas(whole, zone);
	u_weave_rect_part part;
	REQUIRE(u_weave_rect_parts(&m, &zone, 1, &part, 1) == 1);
	CHECK(el.window_screen_left - el.display_screen_left == part.screen_rect.offset.w);
	CHECK(el.window_screen_top - el.display_screen_top == part.screen_rect.offset.h);
	const float px = PANEL_W_M / 1920.0f;
	CHECK(el.window_center_offset_x_m == Catch::Approx((280.0f + 75.0f - 960.0f) * px));

	// And screen 2's eyes, which oxr_segment_views_segment_eyes accepts
	// untranslated (the whole-window canvas is in that screen's own space).
	xrt_eye_positions eyes;
	REQUIRE(oxr_segment_views_segment_eyes(&m.seg[k], 0.0f, 0.0f, 2, &eyes));
	CHECK(eyes.eyes[0].x == Catch::Approx(0.068f));
	CHECK(eyes.eyes[1].x == Catch::Approx(0.132f));
}

TEST_CASE("zone framing: a zone on the primary of a window mostly on screen 2 is framed from the primary")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1700, 100, 1000, 600}; // 220 px on the primary, 780 on screen 2
	xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	with_geometry(m, screens, 2);
	const uint32_t mj = oxr_segment_views_majority(&m);
	REQUIRE(m.seg[mj].screen_id == SECOND_ID);

	// Wholly on the primary: its screen frames it (the locate's single-screen
	// path, the session DP's own eyes and window metrics), not the majority.
	const xrt_rect zone = xr(10, 10, 200, 100);
	const uint32_t k = oxr_segment_views_zone_segment(&m, &zone, mj);
	CHECK(k != mj);
	CHECK(m.seg[k].is_primary);
	// The present-owner rule leaves this one to the session's framing, which
	// for a present-owner already IS the primary's.
	CHECK(u_weave_zone_screen_segment(&m, &zone, win.w, win.h) == -1);

	// A zone wholly on the majority screen is framed from it, as before.
	const xrt_rect on_major = xr(500, 10, 200, 100);
	CHECK(oxr_segment_views_zone_segment(&m, &on_major, mj) == mj);
}

TEST_CASE("zone framing: a straddling zone is framed from ITS majority; empty or off-window keeps the window's")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1400, 100, 1000, 600};
	xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	with_geometry(m, screens, 2);
	const uint32_t mj = oxr_segment_views_majority(&m);

	// A temporary zone lives for the whole call it is passed to.
	auto zs = [mj](const xrt_segment_metrics &t, const xrt_rect &z) {
		return oxr_segment_views_zone_segment(&t, &z, mj);
	};
	// Across the seam at 520: the screen holding most of the ZONE (120 px on
	// the primary, 180 on screen 2), not the window's majority (the primary).
	REQUIRE(mj == 0);
	CHECK(zs(m, xr(400, 50, 300, 200)) == 1);
	// One pixel across: mostly on the primary.
	CHECK(zs(m, xr(420, 50, 101, 200)) == 0);
	// Flush against the seam: wholly on the primary.
	CHECK(zs(m, xr(420, 50, 100, 200)) == 0);
	// Flush on the other side: wholly on screen 2.
	CHECK(zs(m, xr(520, 50, 100, 200)) == 1);
	// Empty, and wholly outside the window.
	CHECK(zs(m, xr(800, 300, 0, 120)) == mj);
	CHECK(zs(m, xr(2000, 300, 50, 50)) == mj);
	// No table, or no zone.
	xrt_segment_metrics none;
	std::memset(&none, 0, sizeof(none));
	CHECK(zs(none, xr(10, 10, 10, 10)) == mj);
	CHECK(oxr_segment_views_zone_segment(&m, nullptr, mj) == mj);

	// A screen of unknown size cannot be framed against: majority.
	xrt_segment_metrics no_size = m;
	no_size.seg[1].screen_height_m = 0.0f;
	CHECK(zs(no_size, xr(800, 300, 150, 120)) == mj);
	no_size = m;
	no_size.seg[1].screen_desktop_width = 0;
	CHECK(zs(no_size, xr(800, 300, 150, 120)) == mj);
}

TEST_CASE("zone framing: one majority rule for the present-owner and every other session")
{
	// Sweep zones across a straddling window in both layouts: the state
	// tracker's rule and the server's (u_weave_zone_screen_segment) pick the
	// same screen - the one holding most of the zone - wherever the server
	// re-frames, and the server keeps the session's own (primary) framing
	// exactly when that screen is the primary (multi-screen § 8.1).
	comp_segments_screen screens[2];
	two_screens(screens);
	for (int layout = 0; layout < 2; layout++) {
		if (layout == 1) {
			screens[1].desktop = {-1920, 0, 1920, 1080};
		}
		const comp_seg_rect win =
		    layout == 0 ? comp_seg_rect{1400, 100, 1000, 600} : comp_seg_rect{-300, 40, 1000, 500};
		xrt_segment_metrics m = publish(win, screens, 2);
		REQUIRE(m.count == 2);
		with_geometry(m, screens, 2);
		const uint32_t mj = oxr_segment_views_majority(&m);
		for (int x = 0; x < 1000; x += 37) {
			for (int w = 10; w < 400; w += 61) {
				const xrt_rect zone = xr(x, 20, w, 80);
				const int32_t server = u_weave_zone_screen_segment(&m, &zone, win.w, win.h);
				const int32_t single = u_weave_rect_single_segment(&m, &zone);
				const int32_t major = u_weave_rect_majority_segment(&m, &zone);
				const uint32_t client = oxr_segment_views_zone_segment(&m, &zone, mj);
				REQUIRE(major >= 0);
				CHECK(client == (uint32_t)major);
				if (single >= 0) {
					CHECK(major == single);
				}
				if (server >= 0) {
					CHECK(client == (uint32_t)server);
				} else {
					CHECK(m.seg[client].is_primary);
				}
				// The parts' own window majority is the zone's majority:
				// the frame the parts are placed in is that screen's.
				xrt_segment_metrics pm;
				const uint32_t np = u_weave_zone_part_table(&m, &zone, &pm, nullptr);
				REQUIRE(np >= 1);
				CHECK(pm.seg[oxr_segment_views_majority(&pm)].screen_id == m.seg[client].screen_id);
			}
		}
	}
}

/*
 * Multi-screen § 8.1: a straddling zone's parts, one view set each
 * (XrDisplayZonePartsDXR). oxr_session_locate_views locates part p with
 * locate_segment_override over the parts' own table: the part as the canvas on
 * its own screen, that screen's eyes carried into the frame of the screen
 * holding most of the zone, and one rig scale for the whole zone.
 */

TEST_CASE("zone parts: a straddling zone is cut at the seam, zone-relative and on its screens")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1400, 100, 1000, 600}; // seam at window x 520
	xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	with_geometry(m, screens, 2);
	m.generation = 7;

	const xrt_rect zone = xr(400, 50, 300, 200);
	xrt_segment_metrics pm;
	xrt_rect rel[XRT_MAX_SEGMENTS];
	REQUIRE(u_weave_zone_part_table(&m, &zone, &pm, rel) == 2);
	CHECK(pm.count == 2);
	CHECK(pm.generation == 7);
	CHECK(pm.window_pixel_width == m.window_pixel_width);
	// Left part on the primary: window x 400..520, at 1800 on the desktop.
	CHECK(pm.seg[0].screen_id == PRIMARY_ID);
	check_rect(pm.seg[0].window_rect, 400, 50, 120, 200);
	check_rect(pm.seg[0].screen_rect, 1800, 150, 120, 200);
	check_rect(rel[0], 0, 0, 120, 200);
	// Right part on screen 2: window x 520..700, its screen x 0..180.
	CHECK(pm.seg[1].screen_id == SECOND_ID);
	check_rect(pm.seg[1].window_rect, 520, 50, 180, 200);
	check_rect(pm.seg[1].screen_rect, 0, 150, 180, 200);
	check_rect(rel[1], 120, 0, 180, 200);
	// The eyes ride along verbatim.
	CHECK(pm.seg[1].eyes.eyes[0].x == Catch::Approx(0.068f));
	// The same cut as the weave's rect parts.
	u_weave_rect_part wp[2];
	REQUIRE(u_weave_rect_parts(&m, &zone, 1, wp, 2) == 2);
	for (uint32_t k = 0; k < 2; k++) {
		CHECK(std::memcmp(&wp[k].window_rect, &pm.seg[k].window_rect, sizeof(xrt_rect)) == 0);
		CHECK(std::memcmp(&wp[k].screen_rect, &pm.seg[k].screen_rect, sizeof(xrt_rect)) == 0);
		CHECK(std::memcmp(&wp[k].rect_rel, &rel[k], sizeof(xrt_rect)) == 0);
	}

	// Located as parts: two, in one frame.
	xrt_segment_metrics pm2;
	oxr_segment_layout l;
	CHECK(oxr_segment_views_zone_parts(&m, &zone, &pm2, rel, &l) == 2);
	CHECK(l.count == 2);
}

TEST_CASE("zone parts: a zone on one screen, off the window, or of unknown screen size is not split")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1400, 100, 1000, 600};
	xrt_segment_metrics m = publish(win, screens, 2);
	with_geometry(m, screens, 2);
	xrt_segment_metrics pm;
	xrt_rect rel[XRT_MAX_SEGMENTS];
	oxr_segment_layout l;
	auto parts = [&](const xrt_segment_metrics &t, const xrt_rect &z) {
		return oxr_segment_views_zone_parts(&t, &z, &pm, rel, &l);
	};
	CHECK(parts(m, xr(10, 10, 100, 100)) == 0);  // wholly on the primary
	CHECK(parts(m, xr(800, 10, 100, 100)) == 0); // wholly on screen 2
	CHECK(parts(m, xr(420, 10, 100, 100)) == 0); // flush against the seam
	CHECK(parts(m, xr(2000, 10, 10, 10)) == 0);  // off the window
	CHECK(parts(m, xr(400, 10, 0, 100)) == 0);   // empty
	CHECK(parts(m, xr(420, 10, 101, 100)) == 2); // one pixel across IS split
	xrt_segment_metrics none;
	std::memset(&none, 0, sizeof(none));
	CHECK(parts(none, xr(400, 10, 300, 100)) == 0); // a window on one screen
	xrt_segment_metrics no_size = m;
	no_size.seg[1].screen_width_m = 0.0f;
	CHECK(parts(no_size, xr(400, 10, 300, 100)) == 0);
	CHECK(oxr_segment_views_zone_parts(&m, nullptr, &pm, rel, &l) == 0);
}

namespace {

/*
 * One part as oxr_session_locate_views frames it: the canvas
 * (oxr_segment_views_window_metrics), the rig offset and virtual display
 * height factor (locate_segment_override), and the canvas' top-left / centre
 * on its OWN screen.
 */
struct part_frame
{
	xrt_window_metrics wm;
	float rig_dx, rig_dy, vdh_scale;
	float own_dx, own_dy; // own -> reference translation
};

part_frame
frame_part(const xrt_segment_metrics &pm, const oxr_segment_layout &l, uint32_t p)
{
	part_frame f;
	oxr_segment_views_window_metrics(&pm, &l, p, &f.wm);
	f.rig_dx = l.seg[p].ref_cx - l.window_ref_cx;
	f.rig_dy = l.seg[p].ref_cy - l.window_ref_cy;
	f.vdh_scale = oxr_segment_views_vdh_scale(&l, p);
	oxr_segment_views_own_to_ref(&l, p, &f.own_dx, &f.own_dy);
	return f;
}

} // namespace

TEST_CASE("zone parts: each part is its own screen's canvas, one rig scale, meeting at the seam")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	// Window mostly on the primary; the ZONE mostly on screen 2.
	const comp_seg_rect win = {1400, 100, 1000, 600}; // seam at window x 520
	xrt_segment_metrics m = publish(win, screens, 2);
	with_geometry(m, screens, 2);
	REQUIRE(m.seg[oxr_segment_views_majority(&m)].is_primary);

	const xrt_rect zone = xr(400, 50, 300, 200);
	xrt_segment_metrics pm;
	xrt_rect rel[XRT_MAX_SEGMENTS];
	oxr_segment_layout l;
	REQUIRE(oxr_segment_views_zone_parts(&m, &zone, &pm, rel, &l) == 2);
	// The frame is the zone's majority screen: screen 2.
	REQUIRE(pm.seg[l.majority].screen_id == SECOND_ID);

	const float px = PANEL_W_M / 1920.0f;
	const float py = PANEL_H_M / 1080.0f;
	const part_frame a = frame_part(pm, l, 0);
	const part_frame b = frame_part(pm, l, 1);

	// Canvas = the part, on its own screen (what the raw channel reports).
	CHECK(a.wm.display_screen_left == 0);
	CHECK(a.wm.window_screen_left - a.wm.display_screen_left == 1800);
	CHECK(a.wm.window_pixel_width == 120u);
	CHECK(a.wm.window_width_m == Catch::Approx(120.0f * px));
	CHECK(b.wm.display_screen_left == 1920);
	CHECK(b.wm.window_screen_left - b.wm.display_screen_left == 0);
	CHECK(b.wm.window_pixel_width == 180u);
	CHECK(b.wm.window_height_m == Catch::Approx(200.0f * py));

	// Eye minus canvas centre is the eye relative to the part ON ITS OWN
	// screen: both are carried into the reference frame by the same
	// translation, so the canvas centre minus that translation is the part's
	// own-screen centre.
	CHECK(b.wm.window_center_offset_x_m - b.own_dx == Catch::Approx((0.0f + 90.0f - 960.0f) * px));
	CHECK(a.wm.window_center_offset_x_m - a.own_dx == Catch::Approx((1800.0f + 60.0f - 960.0f) * px));
	CHECK(a.wm.window_center_offset_y_m - a.own_dy == Catch::Approx(-((150.0f + 100.0f - 540.0f) * py)));
	// The majority part's frame is its own.
	CHECK(b.own_dx == Catch::Approx(0.0f));

	// Continuous across the seam in the reference frame: no gap, no overlap.
	CHECK(l.seg[0].ref_cx + l.seg[0].w_m * 0.5f == Catch::Approx(l.seg[1].ref_cx - l.seg[1].w_m * 0.5f));
	// The union is the zone.
	CHECK(l.window_ref_w == Catch::Approx(300.0f * px));
	CHECK(l.window_ref_h == Catch::Approx(200.0f * py));

	// One m2v for the whole zone: virtual height x factor / part height is the
	// same for both parts (= vH / zone height), and each part's rig offset is
	// its centre minus the zone's centre.
	const float vH = 1.0f;
	const float m2v_a = vH * a.vdh_scale / a.wm.window_height_m;
	const float m2v_b = vH * b.vdh_scale / b.wm.window_height_m;
	CHECK(m2v_a == Catch::Approx(m2v_b));
	CHECK(m2v_a == Catch::Approx(vH / (200.0f * py)));
	CHECK(a.rig_dx == Catch::Approx(-(300.0f * 0.5f - 60.0f) * px));
	CHECK(b.rig_dx == Catch::Approx((300.0f * 0.5f - 90.0f) * px));
	CHECK(a.rig_dy == Catch::Approx(0.0f).margin(1e-6));
	// Scaled by the same m2v, the parts' virtual canvases still abut.
	CHECK((a.rig_dx + a.wm.window_width_m * 0.5f) * m2v_a ==
	      Catch::Approx((b.rig_dx - b.wm.window_width_m * 0.5f) * m2v_b).margin(1e-5));

	// Eyes: screen 2's set in its own frame; the primary's carried over.
	xrt_eye_positions ea, eb;
	REQUIRE(oxr_segment_views_segment_eyes(&pm.seg[0], a.own_dx, a.own_dy, 2, &ea));
	REQUIRE(oxr_segment_views_segment_eyes(&pm.seg[1], b.own_dx, b.own_dy, 2, &eb));
	CHECK(eb.eyes[0].x == Catch::Approx(0.068f));
	CHECK(ea.eyes[0].x - a.wm.window_center_offset_x_m == Catch::Approx(-0.032f - (1800.0f + 60.0f - 960.0f) * px));
}

TEST_CASE("zone parts: panels of different pitch still meet at the seam at one scale")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	// Screen 2: the same physical panel at 3840x2160 (half the pitch).
	screens[1].desktop = {1920, 0, 3840, 2160};
	screens[1].native_w = 3840;
	screens[1].native_h = 2160;
	const comp_seg_rect win = {1400, 100, 1000, 600};
	xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	with_geometry(m, screens, 2);

	const xrt_rect zone = xr(300, 100, 400, 300);
	xrt_segment_metrics pm;
	xrt_rect rel[XRT_MAX_SEGMENTS];
	oxr_segment_layout l;
	REQUIRE(oxr_segment_views_zone_parts(&m, &zone, &pm, rel, &l) == 2);
	const part_frame a = frame_part(pm, l, 0);
	const part_frame b = frame_part(pm, l, 1);
	// Same pixel height, different metres.
	CHECK(a.wm.window_height_m == Catch::Approx(2.0f * b.wm.window_height_m));
	// The seam is continuous in the reference frame...
	CHECK(l.seg[0].ref_cx + l.seg[0].w_m * 0.5f == Catch::Approx(l.seg[1].ref_cx - l.seg[1].w_m * 0.5f));
	// ...and both parts share one metres-to-virtual scale.
	CHECK(a.vdh_scale / a.wm.window_height_m == Catch::Approx(b.vdh_scale / b.wm.window_height_m));
}

TEST_CASE("zone parts: the per-frame budget admits the first N distinct zones of a display time")
{
	oxr_zone_parts_budget b;
	std::memset(&b, 0, sizeof(b));
	const int64_t t0 = 1000;
	CHECK(oxr_zone_parts_budget_admit(&b, t0, 10, 2));
	CHECK(oxr_zone_parts_budget_admit(&b, t0, 11, 2));
	// A zone located twice in one frame is counted once.
	CHECK(oxr_zone_parts_budget_admit(&b, t0, 10, 2));
	// The third distinct zone keeps one view set.
	CHECK_FALSE(oxr_zone_parts_budget_admit(&b, t0, 12, 2));
	// A new display time is a new frame's budget.
	CHECK(oxr_zone_parts_budget_admit(&b, t0 + 16, 12, 2));
	CHECK(b.count == 1);
	// 0 = never split; the cap is clamped to OXR_ZONE_PARTS_MAX_CAP.
	CHECK_FALSE(oxr_zone_parts_budget_admit(&b, t0 + 32, 1, 0));
	for (uint32_t z = 0; z < OXR_ZONE_PARTS_MAX_CAP; z++) {
		CHECK(oxr_zone_parts_budget_admit(&b, t0 + 48, z, 1000));
	}
	CHECK_FALSE(oxr_zone_parts_budget_admit(&b, t0 + 48, 999, 1000));
	CHECK(OXR_ZONE_PARTS_DEFAULT_CAP == 4);
	CHECK_FALSE(oxr_zone_parts_budget_admit(nullptr, t0, 1, 4));
}

TEST_CASE("zone parts: the service frames a present-owner's part exactly as the client frames its own")
{
	// ADR-047 Amendment 10: ipc_try_get_sr_view_poses frames part k of an
	// element in that part's OWN screen space (u_weave_segment_window_metrics,
	// rebased to the part's window rect, that screen's eyes), plus the rig
	// share and offset from u_segment_layout over the parts. The client frames
	// it in the reference frame (oxr_segment_views_window_metrics, eyes carried
	// over). Same eye-relative canvas, same rig scale.
	comp_segments_screen screens[2];
	two_screens(screens);
	for (int layout = 0; layout < 2; layout++) {
		if (layout == 1) {
			screens[1].desktop = {1920, 0, 3840, 2160}; // half the pitch
			screens[1].native_w = 3840;
			screens[1].native_h = 2160;
		}
		const comp_seg_rect win = {1400, 100, 1000, 600};
		xrt_segment_metrics m = publish(win, screens, 2);
		REQUIRE(m.count == 2);
		with_geometry(m, screens, 2);
		const xrt_rect zone = xr(300, 100, 400, 300);

		xrt_segment_metrics pm;
		xrt_rect rel[XRT_MAX_SEGMENTS];
		u_segment_layout pl;
		REQUIRE(u_weave_zone_part_table(&m, &zone, &pm, rel) == 2);
		REQUIRE(u_segment_layout_compute(&pm, &pl));
		for (uint32_t p = 0; p < 2; p++) {
			// Service: the whole window on the part's screen, rebased to the part.
			uint32_t k = 0;
			while (m.seg[k].screen_id != pm.seg[p].screen_id) {
				k++;
			}
			xrt_window_metrics whole;
			REQUIRE(u_weave_segment_window_metrics(&m, k, &whole));
			const xrt_window_metrics svc = zone_canvas(whole, pm.seg[p].window_rect);
			// Client: the part placed in the reference frame.
			xrt_window_metrics cli;
			oxr_segment_views_window_metrics(&pm, &pl, p, &cli);
			float dx = 0.0f, dy = 0.0f;
			oxr_segment_views_own_to_ref(&pl, p, &dx, &dy);

			CHECK(svc.window_screen_left - svc.display_screen_left ==
			      cli.window_screen_left - cli.display_screen_left);
			CHECK(svc.window_pixel_width == cli.window_pixel_width);
			CHECK(svc.window_width_m == Catch::Approx(cli.window_width_m));
			CHECK(svc.window_height_m == Catch::Approx(cli.window_height_m));
			// eye_own - centre_own == (eye_own + d) - centre_ref
			CHECK(svc.window_center_offset_x_m ==
			      Catch::Approx(cli.window_center_offset_x_m - dx).margin(1e-6));
			CHECK(svc.window_center_offset_y_m ==
			      Catch::Approx(cli.window_center_offset_y_m - dy).margin(1e-6));
			// One rig scale, one offset: the shared aux math.
			CHECK(u_segment_vdh_scale(&pl, p) == oxr_segment_views_vdh_scale(&pl, p));
		}
	}
}

TEST_CASE("zone parts: a weave painted against an older table is cropped where it was painted")
{
	// XR_DXR_weave v20: the browser paints an element's parts at the seam of
	// the table its locate saw (generation g) and passes g back at submit; the
	// service keeps each published table's partition by generation and crops
	// with it, while the live table (the window moved 6 px since) places the
	// output. comp_segments_source_rect then reads each screen's views at the
	// PAINTED seam.
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect painted_win = {1400, 100, 1000, 600}; // seam at window x 520
	xrt_segment_metrics painted = publish(painted_win, screens, 2);
	painted.generation = 41;
	comp_segments_content content;
	comp_segments_content_from_metrics(&painted, &content);
	REQUIRE(content.count == 2);
	CHECK(content.canvas.w == 1000u);
	CHECK(content.canvas.h == 600u);
	CHECK(content.rect[0].w == 520u);

	comp_segments_content_ring ring;
	std::memset(&ring, 0, sizeof(ring));
	comp_segments_content_ring_put(&ring, painted.generation, &content);
	comp_segments_content got;
	REQUIRE(comp_segments_content_ring_get(&ring, 41, &got));
	CHECK_FALSE(comp_segments_content_ring_get(&ring, 42, &got)); // not held: live seam

	// The live table: the window dragged 6 px right, seam at window x 514.
	const comp_seg_rect live_win = {1406, 100, 1000, 600};
	comp_segment_table t;
	const comp_seg_rect canvas = {0, 0, live_win.w, live_win.h};
	comp_segments_compute(&live_win, &canvas, screens, 2, &t);
	REQUIRE(t.count == 2);
	REQUIRE(t.seg[0].window_rect.w == 514u);

	// One SBS tile per eye, window-sized.
	comp_seg_rect src;
	REQUIRE(comp_segments_source_rect(&t.seg[0], &canvas, 1000, 600, &content, &src));
	CHECK(src.w == 520u); // the painted seam, not the live 514
	REQUIRE(comp_segments_source_rect(&t.seg[1], &canvas, 1000, 600, &content, &src));
	CHECK(src.x == 520);
	// Without the painted partition, the live seam.
	REQUIRE(comp_segments_source_rect(&t.seg[0], &canvas, 1000, 600, nullptr, &src));
	CHECK(src.w == 514u);

	// A table that is not split carries no partition.
	xrt_segment_metrics none;
	std::memset(&none, 0, sizeof(none));
	comp_segments_content_from_metrics(&none, &content);
	CHECK(content.count == 0);
}

#ifdef DXR_TESTS_HAVE_IPC_PROTOCOL
TEST_CASE("element views: weave_submit's reply carries the split signal as a trailing field")
{
	// Appended after every earlier field (the u_git_tag gate refuses a skewed
	// client/service pair, and appending keeps every older offset).
	STATIC_REQUIRE(sizeof(ipc_weave_submit_reply) <= IPC_BUF_SIZE);
	STATIC_REQUIRE(offsetof(ipc_weave_submit_reply, segment_count) > offsetof(ipc_weave_submit_reply, eyes));
	ipc_weave_submit_reply reply;
	std::memset(&reply, 0, sizeof(reply));
	CHECK(reply.segment_count == 0); // a window on one screen: no table fetch
	reply.segment_count = 2;
	ipc_weave_submit_reply wire;
	std::memcpy(&wire, &reply, sizeof(wire));
	CHECK(u_weave_segments_fetch_needed(1, wire.segment_count));
}
#endif

#ifdef DXR_TESTS_HAVE_IPC_PROTOCOL
TEST_CASE("weave segments: weave_get_segments carries the table and every screen's eyes verbatim")
{
	STATIC_REQUIRE(sizeof(ipc_weave_get_segments_reply) <= IPC_BUF_SIZE);

	comp_segments_screen screens[2];
	two_screens(screens);
	xrt_segment_metrics m = publish({1400, 100, 1000, 600}, screens, 2);
	REQUIRE(m.count == 2);
	// Each screen's own tracker: different viewers, each in its own screen's
	// display space.
	for (uint32_t k = 0; k < 2; k++) {
		m.seg[k].screen_width_m = 0.3442f;
		m.seg[k].screen_height_m = 0.1936f;
		m.seg[k].eyes.count = 2;
		m.seg[k].eyes.valid = true;
		m.seg[k].eyes.is_tracking = k == 0;
		m.seg[k].eyes.eyes[0].x = -0.032f + 0.1f * (float)k;
		m.seg[k].eyes.eyes[1].x = 0.032f + 0.1f * (float)k;
		m.seg[k].eyes.eyes[0].z = 0.6f;
		m.seg[k].eyes.eyes[1].z = 0.6f;
		m.seg[k].have_eyes = true;
	}
	m.generation = 9;

	// The generated proxy memcpys the reply struct off the wire.
	ipc_weave_get_segments_reply reply;
	std::memset(&reply, 0, sizeof(reply));
	reply.result = XRT_SUCCESS;
	reply.metrics = m;
	ipc_weave_get_segments_reply wire;
	std::memcpy(&wire, &reply, sizeof(wire));
	REQUIRE(std::memcmp(&wire.metrics, &m, sizeof(m)) == 0);

	// The client cuts its rects from what came off the wire: the straddling
	// rect's right part carries the SECOND screen's eyes.
	const xrt_rect rects[] = {xr(400, 50, 300, 200)};
	u_weave_rect_part parts[4];
	REQUIRE(u_weave_rect_parts(&wire.metrics, rects, 1, parts, 4) == 2);
	const xrt_segment_metric &right = wire.metrics.seg[parts[1].segment_index];
	CHECK(right.screen_id == SECOND_ID);
	CHECK(right.have_eyes);
	CHECK(right.eyes.eyes[0].x == Catch::Approx(0.068f));
	CHECK_FALSE(right.eyes.is_tracking);
	const xrt_segment_metric &left = wire.metrics.seg[parts[0].segment_index];
	CHECK(left.screen_id == PRIMARY_ID);
	CHECK(left.eyes.eyes[0].x == Catch::Approx(-0.032f));
	CHECK(wire.metrics.generation == 9);
}
#endif

#ifdef DXR_TESTS_HAVE_IPC_PROTOCOL
TEST_CASE("zone parts: the rig locate carries the part request and the parts, appended")
{
	// ADR-047 Amendment 10: append-only (the u_git_tag gate refuses a skewed
	// client/service pair; appending keeps every older offset).
	STATIC_REQUIRE(offsetof(ipc_view_rig_info, zone_part) > offsetof(ipc_view_rig_info, zone_h_px));
	STATIC_REQUIRE(offsetof(ipc_info_locate_views_rig, zone_part_count) >
	               offsetof(ipc_info_locate_views_rig, rear_cue));
	STATIC_REQUIRE(offsetof(ipc_info_locate_views_rig, zone_part_framed) >
	               offsetof(ipc_info_locate_views_rig, zone_part_rect));
	STATIC_REQUIRE(sizeof(ipc_session_locate_views_rig_reply) <= IPC_BUF_SIZE);
	STATIC_REQUIRE(sizeof(ipc_session_locate_views_rig_msg) <= IPC_BUF_SIZE);

	ipc_info_locate_views_rig reply;
	std::memset(&reply, 0, sizeof(reply));
	CHECK(reply.zone_part_count == 0); // a zone on one screen: not split
	ipc_view_rig_info rig;
	std::memset(&rig, 0, sizeof(rig));
	CHECK(rig.zone_part == 0); // the zone's one set
}
#endif
