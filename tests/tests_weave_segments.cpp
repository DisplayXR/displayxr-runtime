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
 *        signal (u_weave_segments_fetch_needed).
 */

#include "util/comp_segments.h"
#include "util/u_canvas.h"
#include "util/u_weave_rect_parts.h"

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

TEST_CASE("element views: a straddling element keeps the primary's views")
{
	comp_segments_screen screens[2];
	two_screens(screens);
	const comp_seg_rect win = {1400, 100, 1000, 600};
	xrt_segment_metrics m = publish(win, screens, 2);
	REQUIRE(m.count == 2);
	with_geometry(m, screens, 2);

	// Window x 400..700 crosses the seam at 520: woven per part, but one view
	// set per element until the page can take two (browser-pvt#272).
	const xrt_rect straddle = xr(400, 50, 300, 200);
	CHECK(u_weave_rect_single_segment(&m, &straddle) == -1);
	CHECK(u_weave_zone_screen_segment(&m, &straddle, win.w, win.h) == -1);

	// One pixel across the seam is already straddling...
	const xrt_rect sliver = xr(420, 50, 101, 200);
	CHECK(u_weave_zone_screen_segment(&m, &sliver, win.w, win.h) == -1);
	// ...and flush against it, the element is wholly on the primary.
	const xrt_rect flush = xr(420, 50, 100, 200);
	CHECK(u_weave_rect_single_segment(&m, &flush) == 0);
	CHECK(u_weave_zone_screen_segment(&m, &flush, win.w, win.h) == -1);
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
