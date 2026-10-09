// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Multi-screen segments on the service / IPC path (ADR-047 Amendment 3):
 *        the routed-view placement the D3D11 service paints its atlas mosaic
 *        with, the change-only publish of the segment table, the client's
 *        change-only routing send, and the wire messages that carry both.
 */

#include "util/comp_segments_route.h"
#include "oxr_segment_views.h"

#ifdef DXR_TESTS_HAVE_IPC_PROTOCOL
#include "shared/ipc_protocol.h"
#include "ipc_protocol_generated.h"
#endif

#include "catch_amalgamated.hpp"

#include <cstring>

namespace {

/*
 * A 1920x1080 window across a seam at window x = 1200: segment 0 (the
 * primary screen) is 1200 px wide, segment 1 720 px. Two views per segment
 * (stereo per screen), so the layer carries 4 views and the atlas keeps the
 * 2-tile stereo grid.
 */
xrt_segment_view_routing
two_screen_routing()
{
	xrt_segment_view_routing r;
	std::memset(&r, 0, sizeof(r));
	r.count = 2;
	r.screen_id[0] = 0x1001;
	r.screen_id[1] = 0x2002;
	r.rect[0] = {{0, 0}, {1200, 1080}};
	r.rect[1] = {{1200, 0}, {720, 1080}};
	r.first_view[0] = 0;
	r.view_count[0] = 2;
	r.first_view[1] = 2;
	r.view_count[1] = 2;
	r.canvas = {{0, 0}, {1920, 1080}};
	return r;
}

// One tile = the window at half scale (a 2x1 stereo atlas of 960x540 views).
const uint32_t TILE_W = 960;
const uint32_t TILE_H = 540;

comp_seg_rect
expected_tile_rect(const xrt_rect &seg, const xrt_rect &canvas)
{
	const comp_seg_rect s = {seg.offset.w, seg.offset.h, (uint32_t)seg.extent.w, (uint32_t)seg.extent.h};
	const comp_seg_rect c = {canvas.offset.w, canvas.offset.h, (uint32_t)canvas.extent.w,
	                         (uint32_t)canvas.extent.h};
	comp_seg_rect out{};
	REQUIRE(comp_segments_tile_rect(&s, &c, TILE_W, TILE_H, &out));
	return out;
}

} // namespace

TEST_CASE("segments ipc: routed views land as a mosaic, at the crop's rect")
{
	const xrt_segment_view_routing r = two_screen_routing();
	const comp_seg_rect r0 = expected_tile_rect(r.rect[0], r.canvas);
	const comp_seg_rect r1 = expected_tile_rect(r.rect[1], r.canvas);

	// The two rects share the seam column inside a tile: no gap, no overlap.
	CHECK(r0.x == 0);
	CHECK(r0.x + (int32_t)r0.w == r1.x);
	CHECK(r1.x + (int32_t)r1.w == (int32_t)TILE_W);

	REQUIRE(comp_segments_route_active(&r, 4, 2, TILE_W, TILE_H));

	struct
	{
		uint32_t view, segment, tile;
	} cases[] = {{0, 0, 0}, {1, 0, 1}, {2, 1, 0}, {3, 1, 1}};
	for (const auto &c : cases) {
		comp_segments_route_slot s{};
		REQUIRE(comp_segments_route_place(&r, 4, 2, TILE_W, TILE_H, c.view, &s));
		CHECK(s.segment == c.segment);
		CHECK(s.tile == c.tile);
		const comp_seg_rect &want = c.segment == 0 ? r0 : r1;
		CHECK(s.rect.x == want.x);
		CHECK(s.rect.y == want.y);
		CHECK(s.rect.w == want.w);
		CHECK(s.rect.h == want.h);
	}
}

TEST_CASE("segments ipc: hardware 2D packs one tile; each segment's first view lands in it")
{
	xrt_segment_view_routing r = two_screen_routing();
	// The locate in a 1-view mode hands each segment one view.
	r.view_count[0] = 1;
	r.first_view[1] = 1;
	r.view_count[1] = 1;
	comp_segments_route_slot s{};
	REQUIRE(comp_segments_route_place(&r, 2, 1, TILE_W, TILE_H, 0, &s));
	CHECK(s.segment == 0);
	CHECK(s.tile == 0);
	REQUIRE(comp_segments_route_place(&r, 2, 1, TILE_W, TILE_H, 1, &s));
	CHECK(s.segment == 1);
	CHECK(s.tile == 0);
}

TEST_CASE("segments ipc: a 3D routing over a 1-tile commit routes only what the tiles hold")
{
	// The mode flipped to 2D between the locate and the commit: two views per
	// segment were located, one tile is packed. Each segment's FIRST view is
	// routed; its second has no tile.
	const xrt_segment_view_routing r = two_screen_routing();
	comp_segments_route_slot s{};
	REQUIRE(comp_segments_route_place(&r, 4, 1, TILE_W, TILE_H, 0, &s));
	CHECK(s.tile == 0);
	CHECK_FALSE(comp_segments_route_place(&r, 4, 1, TILE_W, TILE_H, 1, &s)); // the caller skips it
	REQUIRE(comp_segments_route_place(&r, 4, 1, TILE_W, TILE_H, 2, &s));
	CHECK(s.segment == 1);
	CHECK(s.tile == 0);
	CHECK_FALSE(comp_segments_route_place(&r, 4, 1, TILE_W, TILE_H, 3, &s));
}

TEST_CASE("segments ipc: an inconsistent routing is dropped for the whole frame")
{
	comp_segments_route_slot s{};

	SECTION("unrouted")
	{
		xrt_segment_view_routing r;
		std::memset(&r, 0, sizeof(r));
		CHECK_FALSE(comp_segments_route_active(&r, 2, 2, TILE_W, TILE_H));
		CHECK_FALSE(comp_segments_route_place(&r, 2, 2, TILE_W, TILE_H, 0, &s));
	}
	SECTION("a segment's range past what the layer submitted")
	{
		// The app submitted one stereo set (2 views) for a frame located per
		// segment: segment 1's views do not exist — not even segment 0's
		// views may be routed (all or nothing).
		const xrt_segment_view_routing r = two_screen_routing();
		CHECK_FALSE(comp_segments_route_active(&r, 2, 2, TILE_W, TILE_H));
		CHECK_FALSE(comp_segments_route_place(&r, 2, 2, TILE_W, TILE_H, 0, &s));
	}
	SECTION("a degenerate segment rect")
	{
		xrt_segment_view_routing r = two_screen_routing();
		r.rect[1] = {{1920, 0}, {0, 1080}};
		CHECK_FALSE(comp_segments_route_active(&r, 4, 2, TILE_W, TILE_H));
	}
	SECTION("a count past the arrays")
	{
		xrt_segment_view_routing r = two_screen_routing();
		r.count = XRT_MAX_SEGMENTS + 1;
		CHECK_FALSE(comp_segments_route_active(&r, 4, 2, TILE_W, TILE_H));
	}
	SECTION("no tiles")
	{
		const xrt_segment_view_routing r = two_screen_routing();
		CHECK_FALSE(comp_segments_route_active(&r, 4, 0, TILE_W, TILE_H));
		CHECK_FALSE(comp_segments_route_active(&r, 4, 2, 0, TILE_H));
	}
}

TEST_CASE("segments ipc: the published table bumps its generation only on a change")
{
	xrt_segment_metrics pub;
	std::memset(&pub, 0, sizeof(pub));
	xrt_segment_metrics next;
	std::memset(&next, 0, sizeof(next));

	// Empty -> empty: nothing happened.
	CHECK_FALSE(comp_segments_publish(&pub, &next));
	CHECK(pub.generation == 0);

	next.count = 2;
	next.seg[0].screen_id = 0x1001;
	next.seg[1].screen_id = 0x2002;
	next.window_pixel_width = 1920;
	CHECK(comp_segments_publish(&pub, &next));
	CHECK(pub.generation == 1);
	CHECK(pub.count == 2);

	// The same table again (whatever generation the caller left in it).
	next.generation = 77;
	CHECK_FALSE(comp_segments_publish(&pub, &next));
	CHECK(pub.generation == 1);

	// A move.
	next.window_screen_left = 100;
	CHECK(comp_segments_publish(&pub, &next));
	CHECK(pub.generation == 2);
	CHECK(pub.window_screen_left == 100);

	// Back to one view set.
	std::memset(&next, 0, sizeof(next));
	CHECK(comp_segments_publish(&pub, &next));
	CHECK(pub.count == 0);
	CHECK(pub.generation == 3);
}

TEST_CASE("segments ipc: the client sends the routing only when it changes")
{
	xrt_segment_view_routing sent;
	std::memset(&sent, 0, sizeof(sent));
	xrt_segment_view_routing next;
	std::memset(&next, 0, sizeof(next));

	// Unrouted -> unrouted: nothing to send, ever (the single-screen case).
	CHECK_FALSE(oxr_segment_views_route_differs(&sent, &next));

	next = two_screen_routing();
	CHECK(oxr_segment_views_route_differs(&sent, &next));
	sent = next;
	CHECK_FALSE(oxr_segment_views_route_differs(&sent, &next));

	// A resize moves the seam.
	next.rect[0].extent.w = 1100;
	CHECK(oxr_segment_views_route_differs(&sent, &next));
	next = sent;

	// A mode change re-ranges the views.
	next.view_count[0] = 1;
	CHECK(oxr_segment_views_route_differs(&sent, &next));
	next = sent;

	// The first unrouted frame after a split is sent once (it clears the
	// service's routing)...
	xrt_segment_view_routing none;
	std::memset(&none, 0, sizeof(none));
	CHECK(oxr_segment_views_route_differs(&sent, &none));
	sent = none;
	CHECK_FALSE(oxr_segment_views_route_differs(&sent, &none));

	// ...and stale entries past the count never trigger a send.
	xrt_segment_view_routing junk;
	std::memset(&junk, 0, sizeof(junk));
	junk.first_view[1] = 9;
	junk.screen_id[0] = 42;
	CHECK_FALSE(oxr_segment_views_route_differs(&sent, &junk));
}

#ifdef DXR_TESTS_HAVE_IPC_PROTOCOL
TEST_CASE("segments ipc: the wire messages fit one IPC buffer and carry the payload verbatim")
{
	STATIC_REQUIRE(sizeof(ipc_compositor_get_segment_metrics_reply) <= IPC_BUF_SIZE);
	STATIC_REQUIRE(sizeof(ipc_compositor_set_view_routing_msg) <= IPC_BUF_SIZE);
	STATIC_REQUIRE(sizeof(ipc_compositor_segments_enable_msg) <= IPC_BUF_SIZE);

	// The table crosses by value (the generated proxy memcpys the struct):
	// a full two-segment table with eyes survives byte for byte.
	xrt_segment_metrics m;
	std::memset(&m, 0, sizeof(m));
	m.count = 2;
	for (uint32_t k = 0; k < 2; k++) {
		m.seg[k].screen_id = 0x1000 + k;
		m.seg[k].window_rect = {{(int)(k * 1200), 0}, {k == 0 ? 1200 : 720, 1080}};
		m.seg[k].screen_width_m = 0.3442f;
		m.seg[k].nominal_viewer_z_m = 0.65f;
		m.seg[k].is_primary = k == 0;
		m.seg[k].has_dp = true;
		m.seg[k].woven = true;
		m.seg[k].eyes.count = 2;
		m.seg[k].eyes.valid = true;
		m.seg[k].eyes.eyes[1].x = 0.032f;
		m.seg[k].have_eyes = true;
	}
	m.generation = 5;

	ipc_compositor_get_segment_metrics_reply reply;
	std::memset(&reply, 0, sizeof(reply));
	reply.result = XRT_SUCCESS;
	reply.metrics = m;
	ipc_compositor_get_segment_metrics_reply wire;
	std::memcpy(&wire, &reply, sizeof(wire));
	CHECK(std::memcmp(&wire.metrics, &m, sizeof(m)) == 0);

	const xrt_segment_view_routing r = two_screen_routing();
	ipc_compositor_set_view_routing_msg msg;
	std::memset(&msg, 0, sizeof(msg));
	msg.routing = r;
	ipc_compositor_set_view_routing_msg wire_msg;
	std::memcpy(&wire_msg, &msg, sizeof(wire_msg));
	CHECK(std::memcmp(&wire_msg.routing, &r, sizeof(r)) == 0);
}
#endif
