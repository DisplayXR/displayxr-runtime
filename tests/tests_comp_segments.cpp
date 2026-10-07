// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief comp_segments (multi-screen M2): segment table math, the per-segment
 *        1:1 policy and the DP lifecycle hysteresis.
 */

#include "util/comp_segments.h"

#include "catch_amalgamated.hpp"

#include <cstring>

namespace {

// ds1-linux as M0/M1 report it: the laptop panel at the origin, the DS1 to
// its right. eDP-1 is scaled (desktop != device mode), HDMI-1 is 1:1.
comp_segments_screen
screen(uint64_t id, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t nw, uint32_t nh, bool primary)
{
	comp_segments_screen s{};
	s.id = id;
	s.desktop = {x, y, w, h};
	s.native_w = nw;
	s.native_h = nh;
	s.is_primary = primary;
	s.has_dp_factory = true;
	std::strcpy(s.plugin_id, "sim-display");
	return s;
}

const uint64_t EDP = 0x1001;
const uint64_t DS1 = 0x2002;

} // namespace

TEST_CASE("comp_segments: 1:1 classification")
{
	auto a = screen(EDP, 0, 0, 3456, 2160, 3456, 2160, true);
	CHECK(comp_segments_screen_1to1(&a) == COMP_SEG_1TO1_YES);
	auto b = screen(EDP, 0, 0, 4148, 2592, 3456, 2160, true);
	CHECK(comp_segments_screen_1to1(&b) == COMP_SEG_1TO1_NO);
	auto c = screen(EDP, 0, 0, 3456, 2160, 0, 0, true);
	CHECK(comp_segments_screen_1to1(&c) == COMP_SEG_1TO1_UNKNOWN);
	CHECK(comp_segments_screen_1to1(nullptr) == COMP_SEG_1TO1_UNKNOWN);
}

TEST_CASE("comp_segments: table")
{
	// DS1 is the primary (the active plug-in's panel); eDP to its left.
	comp_segments_screen scr[2] = {
	    screen(EDP, 0, 0, 3456, 2160, 3456, 2160, false),
	    screen(DS1, 3456, 0, 3840, 2160, 3840, 2160, true),
	};
	comp_segment_table t{};

	SECTION("window entirely on the primary -> one primary segment, not split")
	{
		comp_seg_rect win{4000, 100, 1280, 720};
		REQUIRE(comp_segments_compute(&win, nullptr, scr, 2, &t) == 1);
		CHECK(t.seg[0].is_primary);
		CHECK(t.seg[0].screen_id == DS1);
		CHECK(t.seg[0].window_rect.x == 0);
		CHECK(t.seg[0].window_rect.w == 1280);
		CHECK(t.seg[0].present_origin_x == 4000 - 3456);
		CHECK(t.seg[0].present_origin_y == 100);
		CHECK(t.seg[0].screen_rect.x == 4000 - 3456);
		CHECK_FALSE(comp_segments_table_is_split(&t, 1280, 720));
	}

	SECTION("window straddling the seam -> two segments, left to right, each with its own origin")
	{
		comp_seg_rect win{3000, 200, 1280, 720}; // 456 px on eDP, 824 on the DS1
		REQUIRE(comp_segments_compute(&win, nullptr, scr, 2, &t) == 2);
		CHECK(comp_segments_table_is_split(&t, 1280, 720));

		const comp_segment &l = t.seg[0];
		CHECK(l.screen_id == EDP);
		CHECK_FALSE(l.is_primary);
		CHECK(l.window_rect.x == 0);
		CHECK(l.window_rect.y == 0);
		CHECK(l.window_rect.w == 456);
		CHECK(l.window_rect.h == 720);
		CHECK(l.screen_rect.x == 3000);
		CHECK(l.screen_rect.y == 200);
		CHECK(l.present_origin_x == 3000);
		CHECK(l.present_origin_y == 200);

		const comp_segment &r = t.seg[1];
		CHECK(r.screen_id == DS1);
		CHECK(r.is_primary);
		CHECK(r.window_rect.x == 456);
		CHECK(r.window_rect.w == 824);
		CHECK(r.screen_rect.x == 0);
		// phase = present origin + canvas offset = the segment's screen position
		CHECK(r.present_origin_x + r.window_rect.x == r.screen_rect.x);
		CHECK(l.present_origin_x + l.window_rect.x == l.screen_rect.x);
		// the two segments tile the window exactly
		CHECK(l.window_rect.w + r.window_rect.w == win.w);
	}

	SECTION("window flush to the seam belongs to one screen only")
	{
		comp_seg_rect win{3456 - 1280, 0, 1280, 720};
		REQUIRE(comp_segments_compute(&win, nullptr, scr, 2, &t) == 1);
		CHECK(t.seg[0].screen_id == EDP);
		// entirely on a non-primary screen is still a split (the primary DP
		// does not weave it)
		CHECK(comp_segments_table_is_split(&t, 1280, 720));
	}

	SECTION("canvas sub-rect is honoured in window px")
	{
		comp_seg_rect win{3000, 0, 1280, 720};
		comp_seg_rect canvas{100, 50, 1000, 600}; // desktop 3100..4100
		REQUIRE(comp_segments_compute(&win, &canvas, scr, 2, &t) == 2);
		CHECK(t.seg[0].window_rect.x == 100);
		CHECK(t.seg[0].window_rect.y == 50);
		CHECK(t.seg[0].window_rect.w == 356);
		CHECK(t.seg[1].window_rect.x == 456);
		CHECK(t.seg[1].window_rect.w == 644);
		CHECK(t.seg[0].present_origin_x == 3000); // still the WINDOW's origin
	}

	SECTION("window on no screen -> empty, not split")
	{
		comp_seg_rect win{-5000, -5000, 100, 100};
		CHECK(comp_segments_compute(&win, nullptr, scr, 2, &t) == 0);
		CHECK_FALSE(comp_segments_table_is_split(&t, 100, 100));
	}

	SECTION("mirrored outputs give one segment per region, the primary winning")
	{
		comp_segments_screen m[3] = {
		    screen(0x77, 3456, 0, 3840, 2160, 3840, 2160, false), // mirror of the DS1, listed first
		    scr[0],
		    scr[1],
		};
		comp_seg_rect win{3000, 200, 1280, 720};
		REQUIRE(comp_segments_compute(&win, nullptr, m, 3, &t) == 2);
		CHECK(t.seg[0].screen_id == EDP);
		CHECK(t.seg[1].screen_id == DS1);
		CHECK(t.seg[1].is_primary);
		CHECK(t.seg[1].screen_index == 2);
	}

	SECTION("degenerate inputs")
	{
		comp_seg_rect win{0, 0, 0, 0};
		CHECK(comp_segments_compute(&win, nullptr, scr, 2, &t) == 0);
		comp_seg_rect ok{0, 0, 10, 10};
		CHECK(comp_segments_compute(&ok, nullptr, scr, 0, &t) == 0);
		CHECK(comp_segments_compute(&ok, nullptr, nullptr, 2, &t) == 0);
	}

	SECTION("equality + format")
	{
		comp_seg_rect win{3000, 200, 1280, 720};
		comp_segment_table a{}, b{};
		comp_segments_compute(&win, nullptr, scr, 2, &a);
		comp_segments_compute(&win, nullptr, scr, 2, &b);
		CHECK(comp_segments_table_equal(&a, &b));
		win.x += 1;
		comp_segments_compute(&win, nullptr, scr, 2, &b);
		CHECK_FALSE(comp_segments_table_equal(&a, &b));
		char buf[512];
		comp_segments_table_format(&a, buf, sizeof(buf));
		CHECK(std::strstr(buf, "2 segment(s)") != nullptr);
		char tiny[8];
		comp_segments_table_format(&a, tiny, sizeof(tiny));
		CHECK(std::strlen(tiny) < sizeof(tiny));
	}
}

TEST_CASE("comp_segments: uncovered canvas")
{
	comp_seg_rect out[COMP_SEGMENTS_MAX_UNCOVERED];
	comp_seg_rect canvas{0, 0, 1280, 720};

	SECTION("fully covered -> nothing")
	{
		comp_segment_table t{};
		t.count = 2;
		t.seg[0].window_rect = {0, 0, 456, 720};
		t.seg[1].window_rect = {456, 0, 824, 720};
		CHECK(comp_segments_uncovered(&t, &canvas, out, COMP_SEGMENTS_MAX_UNCOVERED) == 0);
	}

	SECTION("no segments -> the whole canvas")
	{
		comp_segment_table t{};
		REQUIRE(comp_segments_uncovered(&t, &canvas, out, COMP_SEGMENTS_MAX_UNCOVERED) == 1);
		CHECK(out[0].x == 0);
		CHECK(out[0].w == 1280);
		CHECK(out[0].h == 720);
	}

	SECTION("a window hanging below a short screen")
	{
		// left screen covers the full height, right screen only the top 500 px
		comp_segment_table t{};
		t.count = 2;
		t.seg[0].window_rect = {0, 0, 456, 720};
		t.seg[1].window_rect = {456, 0, 824, 500};
		REQUIRE(comp_segments_uncovered(&t, &canvas, out, COMP_SEGMENTS_MAX_UNCOVERED) == 1);
		CHECK(out[0].x == 456);
		CHECK(out[0].y == 500);
		CHECK(out[0].w == 824);
		CHECK(out[0].h == 220);
	}

	SECTION("a gap between two screens")
	{
		comp_segment_table t{};
		t.count = 2;
		t.seg[0].window_rect = {0, 0, 400, 720};
		t.seg[1].window_rect = {500, 0, 780, 720};
		REQUIRE(comp_segments_uncovered(&t, &canvas, out, COMP_SEGMENTS_MAX_UNCOVERED) == 1);
		CHECK(out[0].x == 400);
		CHECK(out[0].w == 100);
		CHECK(out[0].h == 720);
	}

	SECTION("area is conserved")
	{
		comp_segment_table t{};
		t.count = 2;
		t.seg[0].window_rect = {100, 50, 300, 200};
		t.seg[1].window_rect = {600, 300, 400, 400};
		const uint32_t n = comp_segments_uncovered(&t, &canvas, out, COMP_SEGMENTS_MAX_UNCOVERED);
		uint64_t area = 0;
		for (uint32_t i = 0; i < n; i++) {
			area += (uint64_t)out[i].w * out[i].h;
		}
		CHECK(area == 1280ull * 720 - 300ull * 200 - 400ull * 400);
	}
}

TEST_CASE("comp_segments: 1:1 policy")
{
	// No DP at all: always flat 2D.
	CHECK(comp_segments_decide(false, true, COMP_SEG_1TO1_YES) == COMP_SEG_RENDER_FLAT_2D);
	// A lenticular weave on a resampled screen: refused.
	CHECK(comp_segments_decide(true, false, COMP_SEG_1TO1_NO) == COMP_SEG_RENDER_FLAT_2D);
	// Anaglyph survives a resample: woven even on a scaled screen (ds1-linux).
	CHECK(comp_segments_decide(true, true, COMP_SEG_1TO1_NO) == COMP_SEG_RENDER_WEAVE);
	// 1:1 or unknown never degrades.
	CHECK(comp_segments_decide(true, false, COMP_SEG_1TO1_YES) == COMP_SEG_RENDER_WEAVE);
	CHECK(comp_segments_decide(true, false, COMP_SEG_1TO1_UNKNOWN) == COMP_SEG_RENDER_WEAVE);
}

namespace {

comp_segment_table
straddle_table()
{
	comp_segment_table t{};
	t.count = 2;
	t.seg[0].screen_id = EDP;
	t.seg[0].has_dp_factory = true;
	t.seg[0].window_rect = {0, 0, 456, 720};
	t.seg[1].screen_id = DS1;
	t.seg[1].is_primary = true;
	t.seg[1].has_dp_factory = true;
	t.seg[1].window_rect = {456, 0, 824, 720};
	return t;
}

comp_segment_table
primary_only_table()
{
	comp_segment_table t{};
	t.count = 1;
	t.seg[0].screen_id = DS1;
	t.seg[0].is_primary = true;
	t.seg[0].has_dp_factory = true;
	t.seg[0].window_rect = {0, 0, 1280, 720};
	return t;
}

int
count_action(const comp_segments_actions &a, comp_seg_action what, uint64_t id)
{
	int n = 0;
	for (uint32_t i = 0; i < a.count; i++) {
		if (a.items[i].action == what && a.items[i].screen_id == id) {
			n++;
		}
	}
	return n;
}

} // namespace

TEST_CASE("comp_segments: lifecycle hysteresis")
{
	comp_segments_lifecycle lc;
	comp_segments_lifecycle_init(&lc, 2, 3);
	comp_segments_actions act{};
	const comp_segment_table split = straddle_table();
	const comp_segment_table single = primary_only_table();

	SECTION("defaults")
	{
		comp_segments_lifecycle d;
		comp_segments_lifecycle_init(&d, 0, 0);
		CHECK(d.create_after == COMP_SEGMENTS_DEFAULT_CREATE_AFTER);
		CHECK(d.destroy_after == COMP_SEGMENTS_DEFAULT_DESTROY_AFTER);
	}

	SECTION("create after N updates, never for the primary")
	{
		comp_segments_lifecycle_update(&lc, &split, &act);
		CHECK(act.count == 0); // 1 update: not yet
		comp_segments_lifecycle_update(&lc, &split, &act);
		REQUIRE(act.count == 1);
		CHECK(count_action(act, COMP_SEG_ACTION_CREATE, EDP) == 1);
		CHECK(count_action(act, COMP_SEG_ACTION_CREATE, DS1) == 0);
		comp_segments_lifecycle_set_created(&lc, EDP, true);
		CHECK(comp_segments_lifecycle_is_live(&lc, EDP));
		comp_segments_lifecycle_update(&lc, &split, &act);
		CHECK(act.count == 0); // live: no second create
	}

	SECTION("a one-frame touch never creates")
	{
		comp_segments_lifecycle_update(&lc, &split, &act);
		comp_segments_lifecycle_update(&lc, &single, &act);
		comp_segments_lifecycle_update(&lc, &split, &act);
		CHECK(act.count == 0);
	}

	SECTION("destroy only after M empty updates; a return in between keeps the DP")
	{
		comp_segments_lifecycle_update(&lc, &split, &act);
		comp_segments_lifecycle_update(&lc, &split, &act);
		comp_segments_lifecycle_set_created(&lc, EDP, true);

		comp_segments_lifecycle_update(&lc, &single, &act);
		comp_segments_lifecycle_update(&lc, &single, &act);
		CHECK(act.count == 0);
		comp_segments_lifecycle_update(&lc, &split, &act); // back across the seam
		CHECK(act.count == 0);
		CHECK(comp_segments_lifecycle_is_live(&lc, EDP));

		comp_segments_lifecycle_update(&lc, &single, &act);
		comp_segments_lifecycle_update(&lc, &single, &act);
		CHECK(act.count == 0);
		comp_segments_lifecycle_update(&lc, &single, &act);
		REQUIRE(act.count == 1);
		CHECK(count_action(act, COMP_SEG_ACTION_DESTROY, EDP) == 1);
		CHECK_FALSE(comp_segments_lifecycle_is_live(&lc, EDP));
	}

	SECTION("a failed create is not retried until the segment goes away")
	{
		comp_segments_lifecycle_update(&lc, &split, &act);
		comp_segments_lifecycle_update(&lc, &split, &act);
		REQUIRE(count_action(act, COMP_SEG_ACTION_CREATE, EDP) == 1);
		comp_segments_lifecycle_set_created(&lc, EDP, false);
		for (int i = 0; i < 5; i++) {
			comp_segments_lifecycle_update(&lc, &split, &act);
			CHECK(act.count == 0);
		}
		comp_segments_lifecycle_update(&lc, &single, &act);
		comp_segments_lifecycle_update(&lc, &split, &act);
		comp_segments_lifecycle_update(&lc, &split, &act);
		CHECK(count_action(act, COMP_SEG_ACTION_CREATE, EDP) == 1);
	}

	SECTION("a screen without a DP factory is never created")
	{
		comp_segment_table t = split;
		t.seg[0].has_dp_factory = false;
		for (int i = 0; i < 5; i++) {
			comp_segments_lifecycle_update(&lc, &t, &act);
			CHECK(act.count == 0);
		}
	}

	SECTION("drain destroys every live DP")
	{
		comp_segments_lifecycle_update(&lc, &split, &act);
		comp_segments_lifecycle_update(&lc, &split, &act);
		comp_segments_lifecycle_set_created(&lc, EDP, true);
		comp_segments_lifecycle_drain(&lc, &act);
		REQUIRE(act.count == 1);
		CHECK(count_action(act, COMP_SEG_ACTION_DESTROY, EDP) == 1);
		CHECK_FALSE(comp_segments_lifecycle_is_live(&lc, EDP));
	}
}

TEST_CASE("comp_segments: deferred release")
{
	comp_segments_retire r{};
	uint32_t kinds[4];
	uint64_t items[4];

	REQUIRE(comp_segments_retire_push(&r, 1, 0xA));
	REQUIRE(comp_segments_retire_push(&r, 2, 0xB));
	REQUIRE(comp_segments_retire_push(&r, 3, 0xC));

	// A fill is in flight: nothing is handed back, nothing is lost.
	CHECK(comp_segments_retire_take(&r, false, kinds, items, 4) == 0);
	CHECK(r.count == 3);

	// Safe: oldest first, capped, the rest stays queued.
	REQUIRE(comp_segments_retire_take(&r, true, kinds, items, 2) == 2);
	CHECK(kinds[0] == 1);
	CHECK(items[0] == 0xA);
	CHECK(items[1] == 0xB);
	CHECK(r.count == 1);
	REQUIRE(comp_segments_retire_take(&r, true, kinds, items, 4) == 1);
	CHECK(items[0] == 0xC);
	CHECK(r.count == 0);

	// Full list refuses instead of overwriting.
	for (uint32_t i = 0; i < COMP_SEGMENTS_RETIRE_MAX; i++) {
		REQUIRE(comp_segments_retire_push(&r, 0, i));
	}
	CHECK_FALSE(comp_segments_retire_push(&r, 0, 999));
	CHECK_FALSE(comp_segments_retire_push(nullptr, 0, 1));
}

TEST_CASE("tile rects: adjacent segments share the seam column (M3 routing == M2 crop)", "[comp_segments]")
{
	// A 1600x900 canvas into an 800x900 tile (anaglyph 0.5x1 scale), split
	// at an odd column so the scaled seam is fractional.
	const comp_seg_rect canvas = {0, 0, 1600, 900};
	const comp_seg_rect left = {0, 0, 801, 900};
	const comp_seg_rect right = {801, 0, 799, 900};
	comp_seg_rect a, b;
	REQUIRE(comp_segments_tile_rect(&left, &canvas, 800, 900, &a));
	REQUIRE(comp_segments_tile_rect(&right, &canvas, 800, 900, &b));
	CHECK(a.x == 0);
	CHECK(a.x + (int32_t)a.w == b.x); // no gap, no overlap
	CHECK(b.x + (int32_t)b.w == 800);
	CHECK(a.h == 900);

	// Canvas offset inside the window.
	const comp_seg_rect canvas2 = {100, 50, 1600, 900};
	const comp_seg_rect seg2 = {100, 50, 800, 900};
	REQUIRE(comp_segments_tile_rect(&seg2, &canvas2, 800, 900, &a));
	CHECK(a.x == 0);
	CHECK(a.w == 400);

	// Outside the canvas / degenerate.
	const comp_seg_rect outside = {0, 0, 50, 900};
	CHECK_FALSE(comp_segments_tile_rect(&outside, &canvas2, 800, 900, &a));
	const comp_seg_rect zero = {0, 0, 0, 0};
	CHECK_FALSE(comp_segments_tile_rect(&left, &zero, 800, 900, &a));
}
