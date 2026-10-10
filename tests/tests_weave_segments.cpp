// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief XR_DXR_weave v19 (#1884, ADR-047 Amendment 4): a present-owner's
 *        weave rects split per screen. The segment table the service cuts the
 *        bound window into (comp_segments_compute), the per-rect parts the
 *        client derives from it (u_weave_rect_parts), and the wire message
 *        that carries the table with every screen's eyes.
 */

#include "util/comp_segments.h"
#include "util/u_weave_rect_parts.h"

#ifdef DXR_TESTS_HAVE_IPC_PROTOCOL
#include "shared/ipc_protocol.h"
#include "ipc_protocol_generated.h"
#endif

#include "catch_amalgamated.hpp"

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
