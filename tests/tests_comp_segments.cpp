// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief comp_segments (multi-screen M2): segment table math, the per-segment
 *        1:1 policy and the DP lifecycle hysteresis.
 */

#include "util/comp_segments.h"
#include "util/comp_segments_route.h"
#include "util/comp_segments_gl.h"

#include "catch_amalgamated.hpp"

#include <cmath>
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

namespace {

// The laptop panel (primary, 0x1 on the win box) at the origin and the DS1 to
// its right, both 3840x2160 1:1. A 1000x500 window at x places
// (3840 - x) px on the laptop.
const uint64_t LAPTOP = 0x1;
const uint64_t ACER = 0x2;
const uint64_t MS = 1000ull * 1000ull;

comp_segment_table
window_at(int32_t x, bool acer_has_factory = true)
{
	comp_segments_screen scr[2] = {
	    screen(LAPTOP, 0, 0, 3840, 2160, 3840, 2160, true),
	    screen(ACER, 3840, 0, 3840, 2160, 3840, 2160, false),
	};
	scr[1].has_dp_factory = acer_has_factory;
	comp_seg_rect win{x, 100, 1000, 500};
	comp_segment_table t{};
	comp_segments_compute(&win, nullptr, scr, 2, &t);
	return t;
}

//! Feed @p t every 16 ms for @p ms; return the first hand-off target (0 = none).
uint64_t
feed(comp_segments_owner &o, const comp_segment_table &t, uint64_t &now, uint64_t ms)
{
	uint64_t target = 0;
	for (uint64_t elapsed = 0; elapsed <= ms; elapsed += 16) {
		const uint64_t r = comp_segments_owner_update(&o, &t, now);
		if (r != 0 && target == 0) {
			target = r;
		}
		now += 16 * MS;
	}
	return target;
}

} // namespace

TEST_CASE("comp_segments: majority rule", "[comp_segments]")
{
	comp_segment_table t = window_at(3840 - 300); // 300 laptop / 700 Acer
	REQUIRE(t.count == 2);
	CHECK(t.seg[comp_segments_majority(&t)].screen_id == ACER);
	t = window_at(3840 - 700);
	CHECK(t.seg[comp_segments_majority(&t)].screen_id == LAPTOP);
	t = window_at(3840 - 500); // exact tie -> the primary
	CHECK(t.seg[comp_segments_majority(&t)].screen_id == LAPTOP);
	comp_segment_table empty{};
	CHECK(comp_segments_majority(&empty) == UINT32_MAX);
	CHECK(comp_segments_majority(nullptr) == UINT32_MAX);
}

TEST_CASE("comp_segments: HWND owner hysteresis", "[comp_segments]")
{
	comp_segments_owner o;
	comp_segments_owner_init(&o, LAPTOP, 0, 0);
	uint64_t now = 1000 * MS;

	SECTION("defaults: the primary owns it, 0.5 s hold, 20 % margin")
	{
		CHECK(o.owner_id == LAPTOP);
		CHECK(o.hold_ns == COMP_SEGMENTS_DEFAULT_HANDOFF_HOLD_NS);
		CHECK(o.hold_ns == 500 * MS);
		CHECK(o.margin_pct == COMP_SEGMENTS_DEFAULT_HANDOFF_MARGIN_PCT);
	}

	SECTION("a window on one screen never hands off")
	{
		CHECK(feed(o, window_at(100), now, 5000) == 0);
		CHECK(o.owner_id == LAPTOP);
		CHECK(o.candidate_id == 0);
	}

	SECTION("a majority inside the dead band never hands off")
	{
		// 45 % laptop / 55 % Acer: the Acer holds the majority, but not by 20 %.
		CHECK(feed(o, window_at(3840 - 450), now, 5000) == 0);
		// 41 / 59: still inside.
		CHECK(feed(o, window_at(3840 - 410), now, 5000) == 0);
	}

	SECTION("a clear majority hands off only after the hold")
	{
		const comp_segment_table t = window_at(3840 - 300); // 30 / 70
		CHECK(comp_segments_owner_update(&o, &t, now) == 0);
		CHECK(o.candidate_id == ACER);
		CHECK(comp_segments_owner_update(&o, &t, now + 499 * MS) == 0);
		CHECK(comp_segments_owner_update(&o, &t, now + 500 * MS) == ACER);
		comp_segments_owner_set_result(&o, ACER, true, ACER);
		CHECK(o.owner_id == ACER);
		// Owned: no further hand-off while it holds the majority.
		now += 600 * MS;
		CHECK(feed(o, t, now, 2000) == 0);
	}

	SECTION("exactly the margin (60 / 40) is a clear majority")
	{
		CHECK(feed(o, window_at(3840 - 400), now, 600) == ACER);
	}

	SECTION("a window entirely on the other screen hands off")
	{
		CHECK(feed(o, window_at(5000), now, 600) == ACER);
	}

	SECTION("a dip into the dead band restarts the clock")
	{
		const comp_segment_table clear = window_at(3840 - 200);
		const comp_segment_table band = window_at(3840 - 450);
		CHECK(comp_segments_owner_update(&o, &clear, now) == 0);
		CHECK(comp_segments_owner_update(&o, &clear, now + 400 * MS) == 0);
		CHECK(comp_segments_owner_update(&o, &band, now + 450 * MS) == 0);
		CHECK(comp_segments_owner_update(&o, &clear, now + 500 * MS) == 0);
		CHECK(comp_segments_owner_update(&o, &clear, now + 999 * MS) == 0);
		CHECK(comp_segments_owner_update(&o, &clear, now + 1000 * MS) == ACER);
	}

	SECTION("a drag back and forth across the seam never flaps")
	{
		// Sweep the window across the seam and back every 300 ms: each pass
		// spends < 0.5 s on either side of the dead band.
		uint64_t handoffs = 0;
		for (int pass = 0; pass < 20; pass++) {
			for (int step = 0; step < 18; step++) {
				const int32_t on_laptop = (pass % 2 == 0) ? 900 - step * 45 : 100 + step * 45;
				const comp_segment_table t = window_at(3840 - on_laptop);
				const uint64_t r = comp_segments_owner_update(&o, &t, now);
				if (r != 0) {
					handoffs++;
					comp_segments_owner_set_result(&o, r, true, r);
				}
				now += 16 * MS;
			}
		}
		CHECK(handoffs == 0);
		CHECK(o.owner_id == LAPTOP);
	}

	SECTION("after a hand-off, the way back needs the same margin and hold")
	{
		REQUIRE(feed(o, window_at(3840 - 300), now, 600) == ACER);
		comp_segments_owner_set_result(&o, ACER, true, ACER);
		// 55 % laptop: inside the band from the Acer's side.
		CHECK(feed(o, window_at(3840 - 550), now, 5000) == 0);
		// Back on the laptop: hand back after the hold.
		CHECK(feed(o, window_at(100), now, 600) == LAPTOP);
		comp_segments_owner_set_result(&o, LAPTOP, true, LAPTOP);
		CHECK(o.owner_id == LAPTOP);
	}

	SECTION("a failed target is not retried until the majority leaves it")
	{
		const comp_segment_table t = window_at(3840 - 300);
		REQUIRE(feed(o, t, now, 600) == ACER);
		comp_segments_owner_set_result(&o, ACER, false, LAPTOP); // rolled back
		CHECK(o.owner_id == LAPTOP);
		CHECK(feed(o, t, now, 5000) == 0);
		// The majority leaves the Acer, then comes back: one more try.
		CHECK(feed(o, window_at(100), now, 100) == 0);
		CHECK(feed(o, t, now, 600) == ACER);
	}

	SECTION("no owner after a failed rollback: the majority takes it")
	{
		REQUIRE(feed(o, window_at(3840 - 300), now, 600) == ACER);
		comp_segments_owner_set_result(&o, ACER, false, 0);
		CHECK(o.owner_id == 0);
		// The laptop becomes the majority: it takes the handle after the hold.
		CHECK(feed(o, window_at(3840 - 800), now, 600) == LAPTOP);
	}

	SECTION("a screen without a DP factory is never a target")
	{
		CHECK(feed(o, window_at(3840 - 100, false), now, 5000) == 0);
		CHECK(o.owner_id == LAPTOP);
	}

	SECTION("off every screen: nothing to follow")
	{
		comp_segment_table empty{};
		CHECK(comp_segments_owner_update(&o, &empty, now) == 0);
		CHECK(comp_segments_owner_update(&o, nullptr, now) == 0);
		CHECK(o.owner_id == LAPTOP);
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

TEST_CASE("tile rects: a split egress slot one frame behind the window still partitions its own tile",
          "[comp_segments]")
{
	// Under the #918 weave-on-scanout split the D3D11 segments crop the egress
	// slot being woven, which during a resize drag was painted for the
	// PREVIOUS window size (the R2 lag): the tile is the slot's own, the canvas
	// the live window. The crop must still cover that tile exactly, with
	// neighbours sharing the seam column, whichever way the window moved.
	// Live window: 1649 px wide, straddling a seam at window x = 825 (odd, so
	// the scaled seam is fractional).
	const comp_seg_rect canvas = {0, 0, 1649, 1788};
	const comp_seg_rect left = {0, 0, 825, 1788};
	const comp_seg_rect right = {825, 0, 824, 1788};
	// Slot tiles one frame behind a growing (800) and a shrinking (840)
	// window, plus the matched one (824/825 ~ 1649 / 2).
	const uint32_t tile_ws[] = {800, 824, 840};
	for (uint32_t tw : tile_ws) {
		CAPTURE(tw);
		comp_seg_rect a, b;
		REQUIRE(comp_segments_tile_rect(&left, &canvas, tw, 894, &a));
		REQUIRE(comp_segments_tile_rect(&right, &canvas, tw, 894, &b));
		CHECK(a.x == 0);
		CHECK(a.x + (int32_t)a.w == b.x); // no gap, no overlap
		CHECK(b.x + (int32_t)b.w == (int32_t)tw);
		CHECK(a.h == 894);
		CHECK(b.h == 894);
		// The seam sits where the live window puts it, scaled to the slot.
		const double seam = 825.0 * tw / 1649.0;
		CHECK(std::abs((double)b.x - seam) <= 0.5);
	}
}

namespace {

// The routing a frame was located (and so painted) against, as the state
// tracker hands it to the compositor: one view set per segment of @p t.
comp_segments_content
content_of(const comp_segment_table &t, const comp_seg_rect &canvas)
{
	xrt_segment_view_routing r{};
	r.count = t.count;
	for (uint32_t k = 0; k < t.count; k++) {
		r.screen_id[k] = t.seg[k].screen_id;
		r.rect[k].offset.w = t.seg[k].window_rect.x;
		r.rect[k].offset.h = t.seg[k].window_rect.y;
		r.rect[k].extent.w = (int)t.seg[k].window_rect.w;
		r.rect[k].extent.h = (int)t.seg[k].window_rect.h;
		r.first_view[k] = 2 * k;
		r.view_count[k] = 2;
	}
	r.canvas.offset.w = canvas.x;
	r.canvas.offset.h = canvas.y;
	r.canvas.extent.w = (int)canvas.w;
	r.canvas.extent.h = (int)canvas.h;
	comp_segments_content c;
	comp_segments_content_from_routing(&r, &c);
	return c;
}

} // namespace

TEST_CASE("source rects: a routed frame is cropped where it was painted, not at the live seam (#1883)",
          "[comp_segments]")
{
	// The win rig: the laptop panel (primary) and a DS1 to its right.
	comp_segments_screen scr[2] = {
	    screen(EDP, 0, 0, 1920, 1080, 1920, 1080, true),
	    screen(DS1, 1920, 0, 3840, 2160, 3840, 2160, false),
	};
	const comp_seg_rect canvas = {0, 0, 800, 600};
	const uint32_t tile_w = 400, tile_h = 600; // a 0.5 x 1 view scale

	// Frame N was located against the window at A: 420 px on the laptop,
	// 380 on the DS1, so its mosaic holds the laptop's views in tile columns
	// [0, 210) and the DS1's in [210, 400).
	const comp_seg_rect win_a = {1500, 100, 800, 600};
	comp_segment_table ta{};
	REQUIRE(comp_segments_compute(&win_a, nullptr, scr, 2, &ta) == 2);
	const comp_segments_content painted = content_of(ta, canvas);
	REQUIRE(painted.count == 2);

	// It is woven while the window is being dragged: the live window is at B,
	// 30 px further right, so the live seam is at window column 390.
	const comp_seg_rect win_b = {1530, 100, 800, 600};
	comp_segment_table tb{};
	REQUIRE(comp_segments_compute(&win_b, nullptr, scr, 2, &tb) == 2);
	REQUIRE(tb.seg[0].screen_id == EDP);
	REQUIRE(tb.seg[1].screen_id == DS1);
	REQUIRE(tb.seg[0].window_rect.w == 390);

	comp_seg_rect a0, a1, live0;
	REQUIRE(comp_segments_tile_rect(&ta.seg[0].window_rect, &canvas, tile_w, tile_h, &a0));
	REQUIRE(comp_segments_tile_rect(&ta.seg[1].window_rect, &canvas, tile_w, tile_h, &a1));
	REQUIRE(comp_segments_tile_rect(&tb.seg[0].window_rect, &canvas, tile_w, tile_h, &live0));
	// The defect: the live partition cuts the laptop's crop at column 195 —
	// 15 columns short, and the DS1's crop then starts with 15 columns of the
	// LAPTOP's views (the sliver across the seam).
	CHECK(a0.w == 210);
	CHECK(live0.w == 195);

	SECTION("routed: each segment's crop follows the painted partition A")
	{
		comp_seg_rect s0, s1;
		REQUIRE(comp_segments_source_rect(&tb.seg[0], &canvas, tile_w, tile_h, &painted, &s0));
		REQUIRE(comp_segments_source_rect(&tb.seg[1], &canvas, tile_w, tile_h, &painted, &s1));
		CHECK(s0.x == a0.x);
		CHECK(s0.w == a0.w);
		CHECK(s1.x == a1.x);
		CHECK(s1.w == a1.w);
		// Exactly the two halves of the mosaic: nobody weaves a neighbour's views.
		CHECK(s0.x == 0);
		CHECK(s0.x + (int32_t)s0.w == s1.x);
		CHECK(s1.x + (int32_t)s1.w == (int32_t)tile_w);
		CHECK(s0.h == tile_h);
		// The live table still places them: canvas + present origin come from
		// where the window IS (the DP stretches 210 tile columns over 390 px).
		CHECK(tb.seg[1].present_origin_x == 1530 - 1920);
	}

	SECTION("the same holds the other way, and at rest the two partitions agree")
	{
		comp_seg_rect s0;
		const comp_segments_content painted_b = content_of(tb, canvas);
		REQUIRE(comp_segments_source_rect(&ta.seg[0], &canvas, tile_w, tile_h, &painted_b, &s0));
		CHECK(s0.w == live0.w);
		REQUIRE(comp_segments_source_rect(&tb.seg[0], &canvas, tile_w, tile_h, &painted_b, &s0));
		CHECK(s0.w == live0.w);
	}

	SECTION("unrouted content (one view set) keeps the live partition")
	{
		comp_seg_rect s0;
		REQUIRE(comp_segments_source_rect(&tb.seg[0], &canvas, tile_w, tile_h, nullptr, &s0));
		CHECK(s0.w == live0.w);
		comp_segments_content none{};
		REQUIRE(comp_segments_source_rect(&tb.seg[0], &canvas, tile_w, tile_h, &none, &s0));
		CHECK(s0.w == live0.w);
	}

	SECTION("a screen the painted frame did not route (a segment appearing) maps live")
	{
		comp_segments_content only_ds1 = painted;
		only_ds1.count = 1;
		only_ds1.screen_id[0] = painted.screen_id[1];
		only_ds1.rect[0] = painted.rect[1];
		comp_seg_rect s0;
		REQUIRE(comp_segments_source_rect(&tb.seg[0], &canvas, tile_w, tile_h, &only_ds1, &s0));
		CHECK(s0.w == live0.w);
	}

	SECTION("a frame painted for another window size is mapped against ITS canvas")
	{
		// Painted at 1000 px wide (seam at 420 of 1000), woven against a live
		// 800 px canvas: the tile is the painted frame's.
		const comp_seg_rect big = {1500, 100, 1000, 600};
		const comp_seg_rect big_canvas = {0, 0, 1000, 600};
		comp_segment_table tbig{};
		REQUIRE(comp_segments_compute(&big, nullptr, scr, 2, &tbig) == 2);
		const comp_segments_content painted_big = content_of(tbig, big_canvas);
		comp_seg_rect s0;
		REQUIRE(comp_segments_source_rect(&tb.seg[0], &canvas, tile_w, tile_h, &painted_big, &s0));
		CHECK(s0.w == 168); // 420 * 400 / 1000
	}
}

TEST_CASE("content ring: a split slot finds the partition of the frame that filled it (#1883)", "[comp_segments]")
{
	comp_segments_content_ring ring{};
	comp_segments_content c{};
	comp_segments_content out{};

	CHECK_FALSE(comp_segments_content_ring_get(&ring, 1, &out));
	CHECK(out.count == 0);

	for (uint64_t seq = 1; seq <= 20; seq++) {
		c.count = 2;
		c.rect[0] = {0, 0, (uint32_t)(400 + seq), 600};
		comp_segments_content_ring_put(&ring, seq, &c);
	}
	// The egress ring is 3 deep: the slot being woven is a frame or two behind.
	REQUIRE(comp_segments_content_ring_get(&ring, 18, &out));
	CHECK(out.count == 2);
	CHECK(out.rect[0].w == 418);
	REQUIRE(comp_segments_content_ring_get(&ring, 20, &out));
	CHECK(out.rect[0].w == 420);
	// Overwritten / never recorded: unknown, reported unrouted.
	CHECK_FALSE(comp_segments_content_ring_get(&ring, 20 - COMP_SEGMENTS_CONTENT_RING, &out));
	CHECK(out.count == 0);
	CHECK_FALSE(comp_segments_content_ring_get(&ring, 0, &out));

	// An unrouted frame (NULL) records count 0, replacing what the cell held.
	comp_segments_content_ring_put(&ring, 21, nullptr);
	REQUIRE(comp_segments_content_ring_get(&ring, 21, &out));
	CHECK(out.count == 0);
	comp_segments_content_ring_put(&ring, 0, &c); // ignored
	CHECK_FALSE(comp_segments_content_ring_get(&ring, 0, &out));
}

TEST_CASE("GL segments: top-left rects flip into the bottom-left framebuffer", "[comp_segments][gl]")
{
	// The right half of a 1920x1080 window: same x, y counted from the bottom.
	const comp_seg_rect right = {960, 0, 960, 1080};
	comp_seg_rect g = comp_segments_gl_flip(&right, 1080);
	CHECK(g.x == 960);
	CHECK(g.y == 0);
	CHECK(g.w == 960);
	CHECK(g.h == 1080);
	// A segment that does not span the window's height (a monitor shorter
	// than the window): top-left 100 px down -> GL y measured from the bottom.
	const comp_seg_rect low = {0, 100, 500, 300};
	g = comp_segments_gl_flip(&low, 1080);
	CHECK(g.y == 1080 - 100 - 300);
	CHECK(g.h == 300);
	// Flipping twice is the identity.
	const comp_seg_rect back = comp_segments_gl_flip(&g, 1080);
	CHECK(back.x == low.x);
	CHECK(back.y == low.y);
}

TEST_CASE("GL segments: the crop reads a segment from every tile in GL row order", "[comp_segments][gl]")
{
	// A 2x2 quad atlas of 400x300 tiles; a segment occupying the right
	// quarter of the window maps to x 300..400 of every tile, full height.
	const comp_seg_rect canvas = {0, 0, 1600, 1200};
	const comp_seg_rect seg = {1200, 0, 400, 1200};
	comp_seg_rect t;
	REQUIRE(comp_segments_tile_rect(&seg, &canvas, 400, 300, &t));
	CHECK(t.x == 300);
	CHECK(t.y == 0);
	CHECK(t.w == 100);
	CHECK(t.h == 300);

	// View 0 (row 0, the TOP row as displayed) lives in GL's TOP row of the
	// atlas — y = (rows-1-0)*300 = 300 — and view 2 (row 1) at y = 0 (#1625).
	comp_seg_rect a0 = comp_segments_gl_atlas_rect(&t, 0, 0, 2, 400, 300);
	comp_seg_rect a2 = comp_segments_gl_atlas_rect(&t, 0, 1, 2, 400, 300);
	comp_seg_rect a1 = comp_segments_gl_atlas_rect(&t, 1, 0, 2, 400, 300);
	CHECK(a0.x == 300);
	CHECK(a0.y == 300);
	CHECK(a2.x == 300);
	CHECK(a2.y == 0);
	CHECK(a1.x == 700);
	CHECK(a1.y == 300);
	CHECK(a0.w == 100);
	CHECK(a0.h == 300);

	// The crop holds the same grid of segment-sized tiles, same row order, so
	// a DP reads it exactly like a whole-canvas atlas.
	comp_seg_rect c0 = comp_segments_gl_crop_rect(0, 0, 2, t.w, t.h);
	comp_seg_rect c2 = comp_segments_gl_crop_rect(0, 1, 2, t.w, t.h);
	comp_seg_rect c1 = comp_segments_gl_crop_rect(1, 0, 2, t.w, t.h);
	CHECK(c0.x == 0);
	CHECK(c0.y == 300);
	CHECK(c2.y == 0);
	CHECK(c1.x == 100);
	CHECK(c1.y == 300);
	CHECK(c0.w == t.w);
	CHECK(c0.h == t.h);
}

TEST_CASE("GL segments: a partial-height segment lands at the right rows inside its tile", "[comp_segments][gl]")
{
	// Window 1000x800 over a 1x2 stereo atlas at the same scale; the segment
	// is the bottom 200 rows of the window (a shorter monitor below a seam).
	const comp_seg_rect canvas = {0, 0, 1000, 800};
	const comp_seg_rect seg = {0, 600, 1000, 200};
	comp_seg_rect t;
	REQUIRE(comp_segments_tile_rect(&seg, &canvas, 500, 800, &t));
	CHECK(t.y == 600);
	CHECK(t.h == 200);
	// Bottom-left: the bottom 200 rows of the tile are GL rows 0..200.
	const comp_seg_rect a = comp_segments_gl_atlas_rect(&t, 1, 0, 1, 500, 800);
	CHECK(a.x == 500);
	CHECK(a.y == 0);
	CHECK(a.h == 200);
	// The routed viewport inside a tile (M3) uses the same flip within the tile.
	const comp_seg_rect v = comp_segments_gl_flip(&t, 800);
	CHECK(v.y == a.y);
}
