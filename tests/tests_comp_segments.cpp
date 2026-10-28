// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief comp_segments (multi-screen M2): segment table math, the per-segment
 *        1:1 policy and the DP lifecycle hysteresis.
 */

#include "util/comp_segments.h"
#include "xrt/xrt_plugin.h"
#include "util/comp_segments_route.h"
#include "util/comp_segments_gl.h"

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstdio>
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

TEST_CASE("comp_segments: a DP carried across a screen-table change (display hotplug)", "[comp_segments][hotplug]")
{
	comp_segments_lifecycle lc;
	comp_segments_lifecycle_init(&lc, 2, 3);
	comp_segments_actions act{};
	const comp_segment_table split = straddle_table();
	const comp_segment_table single = primary_only_table();

	// A new table that still lists EDP the same way keeps its live DP: no
	// second CREATE (a vendor weaver is not rebuilt for an unrelated monitor).
	REQUIRE(comp_segments_lifecycle_adopt(&lc, EDP));
	CHECK(comp_segments_lifecycle_is_live(&lc, EDP));
	comp_segments_lifecycle_update(&lc, &split, &act);
	CHECK(count_action(act, COMP_SEG_ACTION_CREATE, EDP) == 0);
	comp_segments_lifecycle_update(&lc, &split, &act);
	CHECK(act.count == 0);

	// ...and it is retired by the ordinary rule once its segment is gone.
	comp_segments_lifecycle_update(&lc, &single, &act);
	comp_segments_lifecycle_update(&lc, &single, &act);
	CHECK(act.count == 0);
	comp_segments_lifecycle_update(&lc, &single, &act);
	CHECK(count_action(act, COMP_SEG_ACTION_DESTROY, EDP) == 1);
	CHECK_FALSE(comp_segments_lifecycle_is_live(&lc, EDP));

	// Out of slots: adopt fails (the caller releases that DP instead).
	comp_segments_lifecycle full;
	comp_segments_lifecycle_init(&full, 2, 3);
	for (uint64_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		REQUIRE(comp_segments_lifecycle_adopt(&full, 0x100 + i));
	}
	CHECK_FALSE(comp_segments_lifecycle_adopt(&full, 0x999));
	// Adopting a tracked id again reuses its slot.
	CHECK(comp_segments_lifecycle_adopt(&full, 0x100));
}

TEST_CASE("comp_segments: which screen-table change keeps a segment DP (display hotplug)", "[comp_segments][hotplug]")
{
	static int leia_iface = 0, leia_inst = 0, sim_inst = 0;
	xrt_screen_binding b{};
	b.struct_size = sizeof(b);
	b.monitor_id = 0x93ab6bb69aa4bc98ull;
	b.desktop_left = 1512;
	b.desktop_width = 1920;
	b.desktop_height = 1080;
	b.native_pixel_width = 3840;
	b.native_pixel_height = 2160;
	b.physical_width_mm = 598;
	b.physical_height_mm = 341;
	b.desktop_scale = 2.0f;
	std::snprintf(b.serial, sizeof(b.serial), "VD012450ET0116");
	std::snprintf(b.device_name, sizeof(b.device_name), "B0ED46B3-A72B-41CA-A98D-8E6BC987BA4D");

	const comp_segments_dp_key was = {b.monitor_id, &leia_iface, &leia_inst, 100, &b};

	// An identical republish keeps it (no retire, no second tracker acquire).
	xrt_screen_binding same = b;
	comp_segments_dp_key now = {b.monitor_id, &leia_iface, &leia_inst, 100, &same};
	CHECK(comp_segments_dp_key_keeps(&was, &now));

	// The EDID mm refined a sweep later (cg-ids 598x341 -> iokit-edid 600x340): kept.
	xrt_screen_binding refined = b;
	refined.physical_width_mm = 600;
	refined.physical_height_mm = 340;
	now.binding = &refined;
	CHECK(comp_segments_dp_key_keeps(&was, &now));

	// A claim-level change (PRESENT_NOT_READY -> claimed) rebuilds it.
	const comp_segments_dp_key not_ready = {b.monitor_id, &leia_iface, &leia_inst,
	                                        (uint32_t)XRT_DISPLAY_CLAIM_PRESENT_NOT_READY, &b};
	now = {b.monitor_id, &leia_iface, &leia_inst, 100, &same};
	CHECK_FALSE(comp_segments_dp_key_keeps(&not_ready, &now));

	// Re-bound: the same panel at new bounds rebuilds it (test D).
	xrt_screen_binding rebound = b;
	rebound.desktop_left = -1920;
	now.binding = &rebound;
	CHECK_FALSE(comp_segments_dp_key_keeps(&was, &now));

	// Another serial (another panel on that connector) or another claimant instance.
	xrt_screen_binding other_panel = b;
	std::snprintf(other_panel.serial, sizeof(other_panel.serial), "OTHER");
	now.binding = &other_panel;
	CHECK_FALSE(comp_segments_dp_key_keeps(&was, &now));
	now = {b.monitor_id, &leia_iface, &sim_inst, 100, &same};
	CHECK_FALSE(comp_segments_dp_key_keeps(&was, &now));
	now = {0x1234, &leia_iface, &leia_inst, 100, &same};
	CHECK_FALSE(comp_segments_dp_key_keeps(&was, &now));
	CHECK_FALSE(comp_segments_dp_key_keeps(&was, nullptr));
}

TEST_CASE("comp_segments: the session DP keeps its screen only while its plug-in owns it (display hotplug)",
          "[comp_segments][hotplug]")
{
	// The session DP: made by leia-sr for the Odyssey, serial "ODY-1", at
	// 1512,0 2560x1440 pt, CGDirectDisplayID 3.
	comp_segments_session_dp sd{};
	sd.valid = true;
	sd.monitor_id = DS1;
	std::strcpy(sd.plugin_id, "leia-sr");
	std::strcpy(sd.serial, "ODY-1");
	sd.desktop = {1512, 0, 2560, 1440};
	sd.platform_id = 3;

	auto cand = [](uint64_t id, const char *plugin, const char *serial, comp_seg_rect r, uint64_t pid) {
		comp_segments_session_candidate c{};
		c.monitor_id = id;
		c.plugin_id = plugin;
		c.serial = serial;
		c.desktop = r;
		c.platform_id = pid;
		return c;
	};
	const comp_seg_rect laptop = {0, 0, 1512, 982};
	const comp_seg_rect ody = {1512, 0, 2560, 1440};
	comp_segments_session_miss miss = COMP_SEG_SESSION_OWNS;

	// As created.
	comp_segments_session_candidate as_created[] = {
	    cand(EDP, "sim-display", "", laptop, 1),
	    cand(DS1, "leia-sr", "ODY-1", ody, 3),
	};
	CHECK(comp_segments_session_dp_screen(&sd, as_created, 2, &miss) == 1);
	CHECK(miss == COMP_SEG_SESSION_OWNS);

	// Unplugged.
	CHECK(comp_segments_session_dp_screen(&sd, as_created, 1, &miss) == -1);
	CHECK(miss == COMP_SEG_SESSION_GONE);

	// Replugged, panel not authenticated yet: sim-display claims it — re-claimed.
	comp_segments_session_candidate edid_only[] = {
	    cand(EDP, "sim-display", "", laptop, 1),
	    cand(DS1, "sim-display", "", ody, 3),
	};
	CHECK(comp_segments_session_dp_screen(&sd, edid_only, 2, &miss) == -1);
	CHECK(miss == COMP_SEG_SESSION_RECLAIMED);

	// Case D, kept: the SAME serial back at the SAME bounds under the SAME OS
	// handle — the session DP owns it again, its window-bound weaver is kept.
	CHECK(comp_segments_session_dp_screen(&sd, as_created, 2, &miss) == 1);
	CHECK(miss == COMP_SEG_SESSION_OWNS);

	// Case D, recreated: another serial, a renumbered CGDirectDisplayID, or
	// moved bounds — re-bound: the panel gets a fresh segment DP instead.
	comp_segments_session_candidate other_serial[] = {cand(DS1, "leia-sr", "ODY-2", ody, 3)};
	CHECK(comp_segments_session_dp_screen(&sd, other_serial, 1, &miss) == -1);
	CHECK(miss == COMP_SEG_SESSION_REBOUND);
	comp_segments_session_candidate renumbered[] = {cand(DS1, "leia-sr", "ODY-1", ody, 4)};
	CHECK(comp_segments_session_dp_screen(&sd, renumbered, 1, &miss) == -1);
	CHECK(miss == COMP_SEG_SESSION_REBOUND);
	comp_segments_session_candidate moved[] = {cand(DS1, "leia-sr", "ODY-1", {-2560, 0, 2560, 1440}, 3)};
	CHECK(comp_segments_session_dp_screen(&sd, moved, 1, &miss) == -1);
	CHECK(miss == COMP_SEG_SESSION_REBOUND);
	comp_segments_session_candidate remoded[] = {cand(DS1, "leia-sr", "ODY-1", {1512, 0, 1920, 1080}, 3)};
	CHECK(comp_segments_session_dp_screen(&sd, remoded, 1, &miss) == -1);
	CHECK(miss == COMP_SEG_SESSION_REBOUND);
	CHECK(std::strcmp(comp_segments_session_miss_str(COMP_SEG_SESSION_REBOUND), "re-bound") == 0);

	// Replugged under the session DP's own plug-in, which owns the panel but
	// cannot drive it yet (EDID_ONLY): not ready — the session DP must not
	// weave it (flat 2D instead), and gets it back once it is ready again.
	comp_segments_session_candidate not_ready[] = {cand(DS1, "leia-sr", "", ody, 3)};
	not_ready[0].not_ready = true;
	CHECK(comp_segments_session_dp_screen(&sd, not_ready, 1, &miss) == -1);
	CHECK(miss == COMP_SEG_SESSION_NOT_READY);
	CHECK(comp_segments_session_dp_screen(&sd, as_created, 2, &miss) == 1);

	// Not recorded yet: no answer.
	comp_segments_session_dp none{};
	CHECK(comp_segments_session_dp_screen(&none, as_created, 2, &miss) == -1);
	CHECK(miss == COMP_SEG_SESSION_NOT_RECORDED);
	CHECK(comp_segments_session_dp_screen(nullptr, as_created, 2, nullptr) == -1);
}

TEST_CASE("comp_segments: a window on one screen the session DP does not own still splits (display hotplug)",
          "[comp_segments][hotplug]")
{
	// One screen, re-claimed by a hotplugged vendor: not the primary. The
	// single-segment table must take the split path, so the screen is woven
	// by its own segment DP and never by the session DP.
	comp_segments_screen scr = screen(DS1, 0, 0, 3840, 2160, 3840, 2160, false);
	const comp_seg_rect win = {100, 100, 1280, 720};
	comp_segment_table t{};
	comp_segments_compute(&win, nullptr, &scr, 1, &t);
	REQUIRE(t.count == 1);
	CHECK_FALSE(t.seg[0].is_primary);
	CHECK(comp_segments_table_is_split(&t, 1280, 720));

	// The same screen as the session DP's own: the single-DP path, as ever.
	scr.is_primary = true;
	comp_segments_compute(&win, nullptr, &scr, 1, &t);
	CHECK_FALSE(comp_segments_table_is_split(&t, 1280, 720));
}

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

TEST_CASE("comp_segments: hand-off replaces the old owner only where something must weave", "[comp_segments]")
{
	// The shared rule of every backend's hand-off (D3D11, Vulkan, the Vulkan
	// #918 split): before the DP holding the window is destroyed, does its
	// screen need a windowless replacement?
	const comp_segment_table spanning = window_at(3840 - 300); // both screens
	const comp_segment_table on_laptop = window_at(100);       // the laptop only
	REQUIRE(spanning.count == 2);
	REQUIRE(on_laptop.count == 1);

	// The primary always keeps a DP: it is the session's own.
	CHECK(comp_segments_owner_needs_windowless_replacement(&spanning, LAPTOP, true, true));
	CHECK(comp_segments_owner_needs_windowless_replacement(&on_laptop, LAPTOP, true, false));
	CHECK(comp_segments_owner_needs_windowless_replacement(nullptr, LAPTOP, true, false));

	// Another screen keeps one only while the window still covers it...
	CHECK(comp_segments_owner_needs_windowless_replacement(&spanning, ACER, false, true));
	CHECK_FALSE(comp_segments_owner_needs_windowless_replacement(&on_laptop, ACER, false, true));
	// ...and only when its plug-in can make a per-screen DP (else flat 2D).
	CHECK_FALSE(comp_segments_owner_needs_windowless_replacement(&spanning, ACER, false, false));

	// An empty table (the hand-back at teardown / a screen-list rebuild):
	// only the primary is replaced.
	comp_segment_table empty{};
	CHECK_FALSE(comp_segments_owner_needs_windowless_replacement(&empty, ACER, false, true));
	CHECK(comp_segments_owner_needs_windowless_replacement(&empty, LAPTOP, true, true));

	// Nobody holds the window (a failed hand-off): nothing to replace.
	CHECK_FALSE(comp_segments_owner_needs_windowless_replacement(&spanning, 0, false, true));
	CHECK_FALSE(comp_segments_owner_needs_windowless_replacement(&spanning, 0, true, true));
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

TEST_CASE("comp_segments: window_is_split — the zero-copy gate (#2218)", "[comp_segments]")
{
	comp_segments_screen scr[2] = {
	    screen(EDP, 0, 0, 3456, 2160, 3456, 2160, false),
	    screen(DS1, 3456, 0, 3840, 2160, 3840, 2160, true),
	};
	SECTION("wholly on the primary: not split — zero-copy stays available")
	{
		comp_seg_rect win{4000, 100, 1280, 720};
		CHECK_FALSE(comp_segments_window_is_split(&win, nullptr, scr, 2));
		comp_seg_rect canvas{0, 0, 1280, 720};
		CHECK_FALSE(comp_segments_window_is_split(&win, &canvas, scr, 2));
	}
	SECTION("wholly on a secondary screen: split — zero-copy must be withheld")
	{
		comp_seg_rect win{100, 100, 1280, 720};
		CHECK(comp_segments_window_is_split(&win, nullptr, scr, 2));
	}
	SECTION("straddling the seam: split")
	{
		comp_seg_rect win{3000, 200, 1280, 720};
		CHECK(comp_segments_window_is_split(&win, nullptr, scr, 2));
	}
	SECTION("degenerate inputs never split")
	{
		comp_seg_rect win{100, 100, 1280, 720};
		CHECK_FALSE(comp_segments_window_is_split(nullptr, nullptr, scr, 2));
		CHECK_FALSE(comp_segments_window_is_split(&win, nullptr, nullptr, 2));
		CHECK_FALSE(comp_segments_window_is_split(&win, nullptr, scr, 0));
	}
}

TEST_CASE("comp_segments: lens wish follows coverage (#2247)")
{
	// DS1 is the primary; eDP to its left.
	comp_segments_screen scr[2] = {
	    screen(EDP, 0, 0, 3456, 2160, 3456, 2160, false),
	    screen(DS1, 3456, 0, 3840, 2160, 3840, 2160, true),
	};
	comp_segment_table t{};

	SECTION("window wholly on the primary: only the primary is covered")
	{
		comp_seg_rect win{4000, 100, 1280, 720};
		REQUIRE(comp_segments_compute(&win, nullptr, scr, 2, &t) == 1);
		CHECK(comp_segments_screen_covered(&t, DS1));
		CHECK_FALSE(comp_segments_screen_covered(&t, EDP));
		CHECK(comp_segments_lens_wish(&t, DS1, true));
		CHECK_FALSE(comp_segments_lens_wish(&t, EDP, true));
	}

	SECTION("window wholly on the other screen: the primary it left wishes 2D")
	{
		comp_seg_rect win{100, 100, 1280, 720};
		REQUIRE(comp_segments_compute(&win, nullptr, scr, 2, &t) == 1);
		CHECK(comp_segments_screen_covered(&t, EDP));
		CHECK_FALSE(comp_segments_screen_covered(&t, DS1));
		CHECK(comp_segments_lens_wish(&t, EDP, true));
		CHECK_FALSE(comp_segments_lens_wish(&t, DS1, true));
	}

	SECTION("window straddling the seam: both screens covered, both follow the session")
	{
		comp_seg_rect win{3000, 200, 1280, 720};
		REQUIRE(comp_segments_compute(&win, nullptr, scr, 2, &t) == 2);
		CHECK(comp_segments_lens_wish(&t, EDP, true));
		CHECK(comp_segments_lens_wish(&t, DS1, true));
		CHECK_FALSE(comp_segments_lens_wish(&t, EDP, false));
		CHECK_FALSE(comp_segments_lens_wish(&t, DS1, false));
	}

	SECTION("a session in 2D wishes 2D everywhere, covered or not")
	{
		comp_seg_rect win{100, 100, 1280, 720};
		REQUIRE(comp_segments_compute(&win, nullptr, scr, 2, &t) == 1);
		CHECK_FALSE(comp_segments_lens_wish(&t, EDP, false));
		CHECK_FALSE(comp_segments_lens_wish(&t, DS1, false));
	}

	SECTION("no table (window on no screen) carries no coverage: the session's wish stands")
	{
		comp_seg_rect win{-5000, -5000, 100, 100};
		REQUIRE(comp_segments_compute(&win, nullptr, scr, 2, &t) == 0);
		CHECK(comp_segments_screen_covered(&t, EDP));
		CHECK(comp_segments_screen_covered(&t, DS1));
		CHECK(comp_segments_lens_wish(&t, DS1, true));
		CHECK(comp_segments_screen_covered(nullptr, DS1));
	}

	SECTION("an unknown screen is uncovered once a table exists")
	{
		comp_seg_rect win{4000, 100, 1280, 720};
		REQUIRE(comp_segments_compute(&win, nullptr, scr, 2, &t) == 1);
		CHECK_FALSE(comp_segments_screen_covered(&t, 0x9999));
	}
}

TEST_CASE("comp_segments: the lens dead band — 2D after the hand-off dwell, 3D at once (#2247)")
{
	comp_segments_screen scr[2] = {
	    screen(EDP, 0, 0, 3456, 2160, 3456, 2160, false),
	    screen(DS1, 3456, 0, 3840, 2160, 3840, 2160, true),
	};
	const uint64_t ms = 1000ull * 1000ull;
	comp_segments_cover_dwell d;
	comp_segments_cover_dwell_init(&d, 0);
	CHECK(d.hold_ns == COMP_SEGMENTS_DEFAULT_HANDOFF_HOLD_NS);

	comp_segment_table on_primary{}, on_edp{}, straddle{};
	comp_seg_rect w1{4000, 100, 1280, 720};
	comp_seg_rect w2{100, 100, 1280, 720};
	comp_seg_rect w3{3000, 200, 1280, 720};
	REQUIRE(comp_segments_compute(&w1, nullptr, scr, 2, &on_primary) == 1);
	REQUIRE(comp_segments_compute(&w2, nullptr, scr, 2, &on_edp) == 1);
	REQUIRE(comp_segments_compute(&w3, nullptr, scr, 2, &straddle) == 2);

	// Start straddling: both covered.
	CHECK(comp_segments_cover_dwell_update(&d, &straddle, scr, 2, 1000 * ms) == 0);
	CHECK(comp_segments_cover_dwell_covered(&d, 0));
	CHECK(comp_segments_cover_dwell_covered(&d, 1));

	SECTION("off the primary: covered until the dwell has elapsed, then 2D once")
	{
		CHECK(comp_segments_cover_dwell_update(&d, &on_edp, scr, 2, 1100 * ms) == 0);
		CHECK(comp_segments_cover_dwell_covered(&d, 1));
		CHECK(comp_segments_cover_dwell_update(&d, &on_edp, scr, 2, 1599 * ms) == 0);
		CHECK(comp_segments_cover_dwell_covered(&d, 1));
		CHECK(comp_segments_cover_dwell_update(&d, &on_edp, scr, 2, 1600 * ms) == (1u << 1));
		CHECK_FALSE(comp_segments_cover_dwell_covered(&d, 1));
		CHECK(comp_segments_cover_dwell_covered(&d, 0));
		// Steady: no further edges.
		CHECK(comp_segments_cover_dwell_update(&d, &on_edp, scr, 2, 5000 * ms) == 0);

		// Back: covered at once.
		CHECK(comp_segments_cover_dwell_update(&d, &straddle, scr, 2, 5001 * ms) == (1u << 1));
		CHECK(comp_segments_cover_dwell_covered(&d, 1));
	}

	SECTION("a drag back and forth across the seam never reaches 2D")
	{
		uint64_t t = 1000 * ms;
		for (int i = 0; i < 20; i++) {
			t += 300 * ms;
			CHECK(comp_segments_cover_dwell_update(&d, (i % 2) ? &straddle : &on_edp, scr, 2, t) == 0);
			CHECK(comp_segments_cover_dwell_covered(&d, 1));
		}
	}

	SECTION("a pending 2D is cancelled by a return and its clock restarts")
	{
		CHECK(comp_segments_cover_dwell_update(&d, &on_edp, scr, 2, 1100 * ms) == 0);
		CHECK(comp_segments_cover_dwell_update(&d, &straddle, scr, 2, 1500 * ms) == 0);
		CHECK(comp_segments_cover_dwell_update(&d, &on_edp, scr, 2, 1700 * ms) == 0);
		CHECK(comp_segments_cover_dwell_update(&d, &on_edp, scr, 2, 2100 * ms) == 0);
		CHECK(comp_segments_cover_dwell_covered(&d, 1));
		CHECK(comp_segments_cover_dwell_update(&d, &on_edp, scr, 2, 2200 * ms) == (1u << 1));
	}

	SECTION("both screens run their own dwell")
	{
		CHECK(comp_segments_cover_dwell_update(&d, &on_primary, scr, 2, 1100 * ms) == 0);
		CHECK(comp_segments_cover_dwell_update(&d, &on_primary, scr, 2, 1600 * ms) == (1u << 0));
		CHECK_FALSE(comp_segments_cover_dwell_covered(&d, 0));
		CHECK(comp_segments_cover_dwell_covered(&d, 1));
	}

	SECTION("an empty table carries no coverage: nothing goes 2D")
	{
		comp_segment_table empty{};
		CHECK(comp_segments_cover_dwell_update(&d, &empty, scr, 2, 1100 * ms) == 0);
		CHECK(comp_segments_cover_dwell_update(&d, &empty, scr, 2, 9000 * ms) == 0);
		CHECK(comp_segments_cover_dwell_covered(&d, 0));
		CHECK(comp_segments_cover_dwell_covered(&d, 1));
	}
}

TEST_CASE("comp_segments: mask_crop_rect — a segment's share of the zone mask (#2218 #4)", "[comp_segments]")
{
	comp_seg_rect out{};
	SECTION("mask at window resolution: the crop is the segment itself")
	{
		comp_seg_rect seg{300, 0, 980, 720};
		REQUIRE(comp_segments_mask_crop_rect(&seg, 1280, 720, 1280, 720, &out));
		CHECK(out.x == 300);
		CHECK(out.y == 0);
		CHECK(out.w == 980);
		CHECK(out.h == 720);
	}
	SECTION("mask rastered at half resolution: scaled, edges rounded outward")
	{
		comp_seg_rect seg{301, 0, 979, 720}; // 301/2 = 150.5 -> 150 .. (1280/2 = 640)
		REQUIRE(comp_segments_mask_crop_rect(&seg, 1280, 720, 640, 360, &out));
		CHECK(out.x == 150);
		CHECK(out.y == 0);
		CHECK(out.x + out.w == 640);
		CHECK(out.h == 360);
	}
	SECTION("a one-pixel sliver survives the scale")
	{
		comp_seg_rect seg{1279, 0, 1, 720};
		REQUIRE(comp_segments_mask_crop_rect(&seg, 1280, 720, 320, 180, &out));
		CHECK(out.w >= 1);
		CHECK(out.x + out.w <= 320);
	}
	SECTION("clamped to the mask; empty inputs refuse")
	{
		comp_seg_rect seg{1200, 700, 500, 500};
		REQUIRE(comp_segments_mask_crop_rect(&seg, 1280, 720, 1280, 720, &out));
		CHECK(out.x + out.w == 1280);
		CHECK(out.y + out.h == 720);
		comp_seg_rect empty{0, 0, 0, 720};
		CHECK_FALSE(comp_segments_mask_crop_rect(&empty, 1280, 720, 1280, 720, &out));
		CHECK_FALSE(comp_segments_mask_crop_rect(&seg, 0, 720, 1280, 720, &out));
		CHECK_FALSE(comp_segments_mask_crop_rect(&seg, 1280, 720, 0, 720, &out));
		CHECK_FALSE(comp_segments_mask_crop_rect(nullptr, 1280, 720, 1280, 720, &out));
	}
}

TEST_CASE("comp_segments: a window is paced at its majority screen's refresh rate (#2245)", "[comp_segments]")
{
	// The panel (primary, 60 Hz) at the origin, a 165 Hz monitor to its left.
	comp_segments_screen scr[2] = {
	    screen(EDP, 0, 0, 3840, 2160, 3840, 2160, true),
	    screen(DS1, -3840, 0, 3840, 2160, 3840, 2160, false),
	};
	const float hz[2] = {59.0f, 165.0f}; // the primary's re-read deliberately differs from today's value
	const float today = 60.0f;
	uint32_t idx = 0;

	SECTION("wholly on the primary: today's value, not a re-read")
	{
		const comp_seg_rect w = {100, 100, 1600, 900};
		CHECK(comp_segments_window_refresh_hz(&w, scr, hz, 2, today, &idx) == today);
		CHECK(idx == 0);
	}
	SECTION("wholly on the other screen: its rate")
	{
		const comp_seg_rect w = {-3000, 400, 2000, 1200};
		CHECK(comp_segments_window_refresh_hz(&w, scr, hz, 2, today, &idx) == 165.0f);
		CHECK(idx == 1);
	}
	SECTION("straddling: the majority screen's rate")
	{
		const comp_seg_rect mostly_left = {-1200, 0, 1600, 900}; // 1200 px left, 400 px right
		CHECK(comp_segments_window_refresh_hz(&mostly_left, scr, hz, 2, today, &idx) == 165.0f);
		CHECK(idx == 1);
		const comp_seg_rect mostly_right = {-400, 0, 1600, 900};
		CHECK(comp_segments_window_refresh_hz(&mostly_right, scr, hz, 2, today, &idx) == today);
		CHECK(idx == 0);
	}
	SECTION("an exact tie goes to the primary")
	{
		const comp_seg_rect w = {-800, 0, 1600, 900};
		CHECK(comp_segments_window_refresh_hz(&w, scr, hz, 2, today, &idx) == today);
		CHECK(idx == 0);
	}
	SECTION("unknown answers fall back to today's value")
	{
		const comp_seg_rect off = {20000, 20000, 100, 100};
		CHECK(comp_segments_window_refresh_hz(&off, scr, hz, 2, today, &idx) == today);
		CHECK(idx == UINT32_MAX);
		const comp_seg_rect left = {-3000, 400, 2000, 1200};
		const float unknown[2] = {59.0f, 0.0f};
		CHECK(comp_segments_window_refresh_hz(&left, scr, unknown, 2, today, &idx) == today);
		CHECK(idx == 1);
		CHECK(comp_segments_window_refresh_hz(&left, scr, nullptr, 2, today, nullptr) == today);
		CHECK(comp_segments_window_refresh_hz(nullptr, scr, hz, 2, today, nullptr) == today);
		CHECK(comp_segments_window_refresh_hz(&left, scr, hz, 0, today, nullptr) == today);
	}
}

TEST_CASE("comp_segments: a registry screen finds its monitor's refresh rate by its centre (#2245)", "[comp_segments]")
{
	comp_segments_screen mon[2] = {
	    screen(EDP, 0, 0, 3840, 2160, 3840, 2160, true),
	    screen(DS1, -3840, 0, 3840, 2160, 3840, 2160, false),
	};
	const float hz[2] = {60.0f, 165.0f};
	const comp_seg_rect left = {-3840, 0, 3840, 2160};
	const comp_seg_rect panel = {0, 0, 3840, 2160};
	const comp_seg_rect nowhere = {9000, 0, 100, 100};
	CHECK(comp_segments_screen_refresh_hz(&left, mon, hz, 2) == 165.0f);
	CHECK(comp_segments_screen_refresh_hz(&panel, mon, hz, 2) == 60.0f);
	CHECK(comp_segments_screen_refresh_hz(&nowhere, mon, hz, 2) == 0.0f);
	const float unknown[2] = {60.0f, -1.0f};
	CHECK(comp_segments_screen_refresh_hz(&left, mon, unknown, 2) == 0.0f);
	CHECK(comp_segments_screen_refresh_hz(&left, mon, nullptr, 2) == 0.0f);
}

TEST_CASE("comp_segments: a screen's own rate only on the chain's own adapter (#2245 gate)", "[comp_segments]")
{
	const uint64_t igpu = 0x0000000100001234ull;
	const uint64_t dgpu = 0x0000000100005678ull;
	bool other = true;
	// Same adapter: the screen's own rate.
	CHECK(comp_segments_gated_refresh_hz(165.0f, dgpu, dgpu, 60.0f, &other) == 165.0f);
	CHECK_FALSE(other);
	// Another adapter: the primary's, flagged for the caller's log.
	CHECK(comp_segments_gated_refresh_hz(165.0f, dgpu, igpu, 60.0f, &other) == 60.0f);
	CHECK(other);
	// Unknown adapter on either side: the primary's, not flagged.
	CHECK(comp_segments_gated_refresh_hz(165.0f, 0, igpu, 60.0f, &other) == 60.0f);
	CHECK_FALSE(other);
	CHECK(comp_segments_gated_refresh_hz(165.0f, dgpu, 0, 60.0f, &other) == 60.0f);
	CHECK_FALSE(other);
	// Unknown rate on the right adapter: the primary's.
	CHECK(comp_segments_gated_refresh_hz(0.0f, dgpu, dgpu, 60.0f, &other) == 60.0f);
	CHECK_FALSE(other);
	CHECK(comp_segments_gated_refresh_hz(165.0f, dgpu, dgpu, 60.0f, nullptr) == 165.0f);
}

/*
 * ADR-047 Amendment 8, home gone (#2305). The rig of 2026-10-27: the DS1
 * (the session DP's home) left of the laptop. The old rule handed the window
 * back to the home's DP before every rebuild; with the DS1 unplugged that was
 * a fresh DP for a panel that was not there, which never became ready.
 */
TEST_CASE("comp_segments: hand-back target before a rebuild (ADR-047 A8, #2305)", "[comp_segments]")
{
	const uint64_t LAPTOP = 0xfcf5;
	const uint64_t PANEL = 0xb84d;
	const uint64_t OTHER = 0x7777;
	comp_segments_screen scr[3] = {
	    screen(PANEL, -3840, 0, 3840, 2160, 3840, 2160, true), // the home (primary)
	    screen(LAPTOP, 0, 0, 2560, 1600, 2560, 1600, false),
	    screen(OTHER, 2560, 0, 1920, 1080, 1920, 1080, false),
	};
	std::strcpy(scr[0].plugin_id, "vendor"); // the home's plug-in; the others are sim-display's
	const uint32_t home = 0;
	bool ready[3] = {false, true, true};
	int target = 99;

	// Unplugged: the home's id is gone and 'vendor' wins no screen.
	comp_segments_next_list unplugged{};
	comp_segments_next_list_add(&unplugged, LAPTOP, "sim-display", false);
	comp_segments_next_list_add(&unplugged, OTHER, "sim-display", false);
	REQUIRE(unplugged.default_id == 0); // the home is gone: no default

	SECTION("the home is back (the next list's default): back home, as before")
	{
		comp_segments_next_list back = unplugged;
		comp_segments_next_list_add(&back, PANEL, "vendor", true);
		REQUIRE(back.default_id == PANEL);
		CHECK(comp_segments_handback_decide(scr, 3, home, 1, ready, nullptr, &back, &target) ==
		      COMP_SEGMENTS_HANDBACK_HOME);
		CHECK(target == -1);
	}

	SECTION("no next list (a teardown), or no segmentation: back home, as before")
	{
		CHECK(comp_segments_handback_decide(scr, 3, home, 1, ready, nullptr, nullptr, &target) ==
		      COMP_SEGMENTS_HANDBACK_HOME);
		CHECK(comp_segments_handback_decide(scr, 3, UINT32_MAX, 1, ready, nullptr, &unplugged, &target) ==
		      COMP_SEGMENTS_HANDBACK_HOME);
		CHECK(target == -1);
	}

	SECTION("home gone, the laptop holds the window and its DP is ready: it stays there")
	{
		CHECK(comp_segments_handback_decide(scr, 3, home, 1, ready, nullptr, &unplugged, &target) ==
		      COMP_SEGMENTS_HANDBACK_STAY);
		CHECK(target == 1);
	}

	SECTION("re-claimed by another plug-in (listed, no default): the panel is there, back home as before")
	{
		comp_segments_next_list reclaimed = unplugged;
		comp_segments_next_list_add(&reclaimed, PANEL, "other", false);
		CHECK(comp_segments_handback_decide(scr, 3, home, 1, ready, nullptr, &reclaimed, &target) ==
		      COMP_SEGMENTS_HANDBACK_HOME);
		CHECK(target == -1);
	}

	SECTION("the panel under a new (position-derived) id: its plug-in wins a screen, back home as before")
	{
		comp_segments_next_list moved = unplugged;
		comp_segments_next_list_add(&moved, 0xa714, "vendor", false);
		CHECK(comp_segments_handback_decide(scr, 3, home, 1, ready, nullptr, &moved, &target) ==
		      COMP_SEGMENTS_HANDBACK_HOME);
		CHECK(target == -1);
		// Mid-replug, Windows lists the panel alone at (0, 0) under a new id.
		comp_segments_next_list alone{};
		comp_segments_next_list_add(&alone, 0xa714, "vendor", false);
		CHECK(comp_segments_handback_decide(scr, 3, home, 1, ready, nullptr, &alone, &target) ==
		      COMP_SEGMENTS_HANDBACK_HOME);
	}

	SECTION("home gone while its DP still holds the window: to the ready screen the window covers most")
	{
		comp_segment_table last{};
		const comp_seg_rect win{2000, 100, 1200, 700}; // 560 px on the laptop, 640 on OTHER
		REQUIRE(comp_segments_compute(&win, nullptr, scr, 3, &last) == 2);
		CHECK(comp_segments_handback_decide(scr, 3, home, (int)home, ready, &last, &unplugged, &target) ==
		      COMP_SEGMENTS_HANDBACK_MOVE);
		CHECK(target == 2);
		// No share known: the first ready screen.
		CHECK(comp_segments_handback_decide(scr, 3, home, (int)home, ready, nullptr, &unplugged, &target) ==
		      COMP_SEGMENTS_HANDBACK_MOVE);
		CHECK(target == 1);
		// Nobody holds the window (a failed hand-off): the same choice.
		CHECK(comp_segments_handback_decide(scr, 3, home, -1, ready, &last, &unplugged, &target) ==
		      COMP_SEGMENTS_HANDBACK_MOVE);
		CHECK(target == 2);
	}

	SECTION("the holder's DP is not ready (no geometry): to another ready screen")
	{
		bool r[3] = {false, false, true};
		CHECK(comp_segments_handback_decide(scr, 3, home, 1, r, nullptr, &unplugged, &target) ==
		      COMP_SEGMENTS_HANDBACK_MOVE);
		CHECK(target == 2);
	}

	SECTION("the holder's screen left too: to a ready screen that is listed")
	{
		comp_segments_next_list only_other{};
		comp_segments_next_list_add(&only_other, OTHER, "sim-display", false);
		CHECK(comp_segments_handback_decide(scr, 3, home, 1, ready, nullptr, &only_other, &target) ==
		      COMP_SEGMENTS_HANDBACK_MOVE);
		CHECK(target == 2);
	}

	SECTION("no listed screen has a ready DP: the window stays with its holder, never a new home DP")
	{
		bool none[3] = {false, false, false};
		CHECK(comp_segments_handback_decide(scr, 3, home, 1, none, nullptr, &unplugged, &target) ==
		      COMP_SEGMENTS_HANDBACK_NO_READY);
		CHECK(target == 1);
		CHECK(comp_segments_handback_decide(scr, 3, home, (int)home, none, nullptr, &unplugged, &target) ==
		      COMP_SEGMENTS_HANDBACK_NO_READY);
		CHECK(target == (int)home);
		CHECK(comp_segments_handback_decide(scr, 3, home, -1, none, nullptr, &unplugged, &target) ==
		      COMP_SEGMENTS_HANDBACK_NO_READY);
		CHECK(target == -1);
		// The home's own readiness never counts.
		bool home_only[3] = {true, false, false};
		CHECK(comp_segments_handback_decide(scr, 3, home, (int)home, home_only, nullptr, &unplugged,
		                                    &target) == COMP_SEGMENTS_HANDBACK_NO_READY);
	}

	CHECK(std::strcmp(comp_segments_handback_str(COMP_SEGMENTS_HANDBACK_STAY), "stay") == 0);
}

TEST_CASE("comp_segments: the next list keeps ids, plug-ins and the default, capped (#2305)", "[comp_segments]")
{
	comp_segments_next_list nl{};
	for (uint64_t k = 1; k <= COMP_SEGMENTS_MAX_SCREENS + 3; k++) {
		comp_segments_next_list_add(&nl, k, k == 2 ? "vendor" : "sim-display", k == 2);
	}
	CHECK(nl.count == COMP_SEGMENTS_MAX_SCREENS);
	CHECK(nl.default_id == 2);
	CHECK(std::strcmp(nl.plugin_ids[1], "vendor") == 0);
	comp_segments_next_list_add(&nl, 99, nullptr, false); // full: ignored
	comp_segments_next_list_add(nullptr, 1, "x", true);   // no list: ignored
	CHECK(nl.count == COMP_SEGMENTS_MAX_SCREENS);
}

/*
 * #1575: a window that overhangs the panel is clipped to the panel before its
 * atlas crosses the output-device split's bridge, instead of being refused.
 */
namespace {

comp_segments_overhang_in
overhang_in(comp_seg_rect window, uint32_t cols, uint32_t rows, uint32_t view_w, uint32_t view_h)
{
	comp_segments_overhang_in in{};
	in.window = window;
	in.canvas = {0, 0, window.w, window.h};
	// The G16's 300 % panel left of the main display, and the bridge cap the
	// 2026-10-26 log reports (`cross-adapter ring 3840x2160`).
	in.panel = {-3840, 0, 3840, 2160};
	in.cols = cols;
	in.rows = rows;
	in.view_w = view_w;
	in.view_h = view_h;
	in.max_w = 3840;
	in.max_h = 2160;
	return in;
}

bool
contains(const comp_seg_rect &outer, const comp_seg_rect &inner)
{
	return inner.x >= outer.x && inner.y >= outer.y && (int64_t)inner.x + inner.w <= (int64_t)outer.x + outer.w &&
	       (int64_t)inner.y + inner.h <= (int64_t)outer.y + outer.h;
}

// What every CLIPPED answer must hold, whatever the geometry.
void
check_clip_invariants(const comp_segments_overhang_in &in, const comp_segments_overhang &o)
{
	// The box fits the bridge, and the cut lies inside its tile.
	CHECK(o.box_w == in.cols * o.tile.w);
	CHECK(o.box_h == in.rows * o.tile.h);
	CHECK(o.box_w <= in.max_w);
	CHECK(o.box_h <= in.max_h);
	CHECK(o.tile.x >= 0);
	CHECK(o.tile.y >= 0);
	CHECK(o.tile.x + o.tile.w <= in.view_w);
	CHECK(o.tile.y + o.tile.h <= in.view_h);
	// The cut is the bridge's per-tile share: a function of the tile size only.
	const uint32_t share_w = in.max_w / in.cols < in.view_w ? in.max_w / in.cols : in.view_w;
	const uint32_t share_h = in.max_h / in.rows < in.view_h ? in.max_h / in.rows : in.view_h;
	CHECK(o.tile.w == share_w);
	CHECK(o.tile.h == share_h);
	// The DP canvas is the cut mapped back, edge by edge (one scale), and lies
	// inside the canvas the atlas maps onto.
	comp_seg_rect back{};
	REQUIRE(comp_segments_canvas_from_tile(&o.tile, &in.canvas, in.view_w, in.view_h, &back));
	CHECK(back.x == o.canvas.x);
	CHECK(back.y == o.canvas.y);
	CHECK(back.w == o.canvas.w);
	CHECK(back.h == o.canvas.h);
	CHECK(contains(in.canvas, o.canvas));
	// The need covers the part on the panel, and the cut covers the need
	// whenever the share is wide enough for it.
	comp_seg_rect need_back{};
	REQUIRE(comp_segments_canvas_from_tile(&o.need, &in.canvas, in.view_w, in.view_h, &need_back));
	CHECK(contains(need_back, o.keep));
	if (o.need.w <= o.tile.w && o.need.h <= o.tile.h) {
		CHECK(contains(o.tile, o.need));
		CHECK(contains(o.canvas, o.keep));
	} else {
		// A need longer than the share keeps its start.
		if (o.need.w > o.tile.w) {
			CHECK(o.tile.x == o.need.x);
		}
		if (o.need.h > o.tile.h) {
			CHECK(o.tile.y == o.need.y);
		}
		// One tile px longer is outward rounding on a window that covers the
		// whole panel at an odd offset: the canvas then misses at most that
		// one tile px of the panel. More is a tile not at the panel's scale.
		if (o.need.w <= o.tile.w + 1 && o.need.h <= o.tile.h + 1) {
			const int32_t sx = (int32_t)((in.canvas.w + in.view_w - 1) / in.view_w);
			const int32_t sy = (int32_t)((in.canvas.h + in.view_h - 1) / in.view_h);
			const comp_seg_rect slack = {o.canvas.x - sx, o.canvas.y - sy, o.canvas.w + 2u * (uint32_t)sx,
			                             o.canvas.h + 2u * (uint32_t)sy};
			CHECK(contains(slack, o.keep));
		}
	}
}

} // namespace

TEST_CASE("overhang: the 2026-10-26 start case is clipped to the panel, not refused (#1575)",
          "[comp_segments][overhang]")
{
	// TargetDisplay moved a 1920x1080 Unity player from the 150 % main display
	// onto the 300 % 3840x2160 panel; Windows doubled it to a 3848x2169 client
	// at (-2873,597) (outer rect (-2891,512) 3884x2272, which the logged
	// window-relative Kooima offset (0.0870,-0.0539) m agrees with). The
	// renderer painted 2x1 tiles of 1924x1084: a 3848x1084 box on a 3840-wide
	// bridge, refused every frame (`COPY EXTENT MISMATCH ... 3848x1084 ...
	// destination is 3840x1084`).
	const auto in = overhang_in({-2873, 597, 3848, 2169}, 2, 1, 1924, 1084);
	comp_segments_overhang o{};
	REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);

	// Past the panel's right edge (onto the main display) and its bottom edge
	// (off every screen).
	CHECK(o.over_left == 0);
	CHECK(o.over_top == 0);
	CHECK(o.over_right == 975);
	CHECK(o.over_bottom == 606);

	// The part on the panel, and what each tile must carry for it.
	CHECK(o.keep.x == 0);
	CHECK(o.keep.y == 0);
	CHECK(o.keep.w == 2873);
	CHECK(o.keep.h == 1563);
	CHECK(o.need.x == 0);
	CHECK(o.need.y == 0);
	CHECK(o.need.w == 1437);
	CHECK(o.need.h == 782);

	// Each tile carries the bridge's share, 1920x1084, at its real offset;
	// the DP weaves it over the window pixels it maps to, unscaled.
	CHECK(o.tile.x == 0);
	CHECK(o.tile.y == 0);
	CHECK(o.tile.w == 1920);
	CHECK(o.tile.h == 1084);
	CHECK(o.canvas.x == 0);
	CHECK(o.canvas.y == 0);
	CHECK(o.canvas.w == 3840);
	CHECK(o.canvas.h == 2169);
	CHECK(o.box_w == 3840);
	CHECK(o.box_h == 1084);
	check_clip_invariants(in, o);
}

TEST_CASE("overhang: the DP canvas comes from the rounded tile rect, not from the panel cut (#1575)",
          "[comp_segments][overhang]")
{
	// The start case: 2873 window px on the panel need 1437 tile px, and 1437
	// tile px are 2874 window px. A canvas of 2873 would weave 1437 tiles px a
	// 1/2874 squeeze; the canvas from the tile rect puts every texel back
	// where the whole weave would.
	const comp_seg_rect canvas = {0, 0, 3848, 2169};
	const comp_seg_rect need = {0, 0, 1437, 782};
	comp_seg_rect c{};
	REQUIRE(comp_segments_canvas_from_tile(&need, &canvas, 1924, 1084, &c));
	CHECK(c.x == 0);
	CHECK(c.w == 2874);
	CHECK(c.h == 1565);

	// A whole tile maps to the whole canvas, offset included.
	const comp_seg_rect inner = {200, 100, 3900, 1800};
	const comp_seg_rect whole = {0, 0, 1950, 900};
	REQUIRE(comp_segments_canvas_from_tile(&whole, &inner, 1950, 900, &c));
	CHECK(c.x == 200);
	CHECK(c.y == 100);
	CHECK(c.w == 3900);
	CHECK(c.h == 1800);

	// Neighbouring cuts share their seam column, so a moving cut never opens a
	// gap or overlaps itself at a fractional scale (3847 px over 1923 texels).
	const comp_seg_rect odd = {0, 0, 3847, 2169};
	for (int32_t x = 0; x + 960 <= 1923; x += 37) {
		CAPTURE(x);
		const comp_seg_rect a = {x, 0, 480, 1084};
		const comp_seg_rect b = {x + 480, 0, 480, 1084};
		comp_seg_rect ca{}, cb{};
		REQUIRE(comp_segments_canvas_from_tile(&a, &odd, 1923, 1084, &ca));
		REQUIRE(comp_segments_canvas_from_tile(&b, &odd, 1923, 1084, &cb));
		CHECK(ca.x + (int32_t)ca.w == cb.x);
		// And each edge is within half a texel of its exact place.
		const double s = 3847.0 / 1923.0;
		CHECK(std::fabs(ca.x - x * s) <= s / 2 + 1e-9);
		CHECK(std::fabs(cb.x + (double)cb.w - (x + 960) * s) <= s / 2 + 1e-9);
	}

	// Degenerate.
	const comp_seg_rect empty = {0, 0, 0, 10};
	CHECK_FALSE(comp_segments_canvas_from_tile(&empty, &canvas, 1924, 1084, &c));
	CHECK_FALSE(comp_segments_canvas_from_tile(&need, &canvas, 0, 1084, &c));
}

TEST_CASE("overhang: a window inside the panel, or a box that fits, is carried whole (#1575)",
          "[comp_segments][overhang]")
{
	comp_segments_overhang o{};
	// The same player before the DPI move: 1920x1080 on the panel.
	auto in = overhang_in({-2873, 597, 1920, 1080}, 2, 1, 960, 540);
	CHECK(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_FITS);
	CHECK(o.box_w == 0);

	// A maximized window (3840x1949, the 10-26 run's later fit).
	in = overhang_in({-3840, 0, 3840, 1949}, 2, 1, 1920, 974);
	CHECK(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_FITS);

	// A normal-size window dragged half off the panel: the box fits, so it
	// crosses whole, exactly as before #1575.
	in = overhang_in({-4800, 400, 1920, 1080}, 2, 1, 960, 540);
	CHECK(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_FITS);

	// Taller than the panel but narrow enough: 2x1 at half scale fits.
	in = overhang_in({-3000, -100, 3000, 2400}, 2, 1, 1500, 1200);
	CHECK(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_FITS);
}

TEST_CASE("overhang: each edge clips at its real offset (#1575)", "[comp_segments][overhang]")
{
	comp_segments_overhang o{};

	SECTION("left")
	{
		// 3900 wide, 60 px past the panel's left edge: the cut starts 30 tile
		// px (60 window px) into each tile.
		const auto in = overhang_in({-3900, 80, 3900, 2000}, 2, 1, 1950, 1000);
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.over_left == 60);
		CHECK(o.over_right == 0);
		CHECK(o.tile.x == 30);
		CHECK(o.tile.w == 1920);
		CHECK(o.tile.y == 0);
		CHECK(o.tile.h == 1000);
		CHECK(o.canvas.x == 60);
		CHECK(o.canvas.w == 3840);
		CHECK(o.box_w == 3840);
		check_clip_invariants(in, o);
	}
	SECTION("right")
	{
		const auto in = overhang_in({-3840, 80, 3900, 2000}, 2, 1, 1950, 1000);
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.over_left == 0);
		CHECK(o.over_right == 60);
		CHECK(o.tile.x == 0);
		CHECK(o.tile.w == 1920);
		CHECK(o.canvas.x == 0);
		CHECK(o.canvas.w == 3840);
		check_clip_invariants(in, o);
	}
	SECTION("top")
	{
		// 2D (1x1, full scale): 2300 tall on a 2160-tall panel.
		const auto in = overhang_in({-3500, -140, 3000, 2300}, 1, 1, 3000, 2300);
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.over_top == 140);
		CHECK(o.over_bottom == 0);
		CHECK(o.tile.x == 0);
		CHECK(o.tile.w == 3000);
		CHECK(o.tile.y == 140);
		CHECK(o.tile.h == 2160);
		CHECK(o.canvas.y == 140);
		CHECK(o.canvas.h == 2160);
		CHECK(o.box_w == 3000);
		CHECK(o.box_h == 2160);
		check_clip_invariants(in, o);
	}
	SECTION("bottom")
	{
		const auto in = overhang_in({-3500, 0, 3000, 2300}, 1, 1, 3000, 2300);
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.over_top == 0);
		CHECK(o.over_bottom == 140);
		CHECK(o.tile.y == 0);
		CHECK(o.tile.h == 2160);
		CHECK(o.canvas.y == 0);
		CHECK(o.canvas.h == 2160);
		check_clip_invariants(in, o);
	}
	SECTION("all four")
	{
		// Larger than the panel both ways, centred on it. One row of tiles,
		// so the share is the whole tile height.
		const auto in = overhang_in({-3870, -20, 3900, 2200}, 2, 1, 1950, 1100);
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.over_left == 30);
		CHECK(o.over_right == 30);
		CHECK(o.over_top == 20);
		CHECK(o.over_bottom == 20);
		CHECK(o.keep.x == 30);
		CHECK(o.keep.y == 20);
		CHECK(o.keep.w == 3840);
		CHECK(o.keep.h == 2160);
		CHECK(o.tile.x == 15);
		CHECK(o.tile.w == 1920);
		CHECK(o.tile.y == 0);
		CHECK(o.tile.h == 1100);
		CHECK(o.canvas.x == 30);
		CHECK(o.canvas.w == 3840);
		CHECK(o.canvas.y == 0);
		CHECK(o.canvas.h == 2200);
		check_clip_invariants(in, o);
	}
}

TEST_CASE("overhang: a drag across the panel's left edge keeps one box for the whole episode (#1575)",
          "[comp_segments][overhang]")
{
	// A window larger than the panel, dragged from past the panel's right
	// edge to far past its left one (off the desktop), and diagonally. The
	// cut moves with the window; its size, and so the bridge box, the egress
	// ring and the DP input, never change. Every step covers the part on the
	// panel and keeps the left cut where the panel's edge is.
	const uint32_t box_w = 3840, box_h = 950;
	uint32_t steps = 0;
	for (int32_t x = -3780; x >= -6000; x -= 7) {
		for (int32_t y = -60; y <= 360; y += 140) {
			CAPTURE(x, y);
			const auto in = overhang_in({x, y, 3900, 1900}, 2, 1, 1950, 950);
			comp_segments_overhang o{};
			REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
			CHECK(o.box_w == box_w);
			CHECK(o.box_h == box_h);
			CHECK(o.tile.w == 1920);
			CHECK(o.tile.h == 950);
			// The part on the panel starts at the panel's left edge in window px
			// (or at the window's own edge while that is on the panel).
			const int32_t panel_x0 = -3840 - x;
			CHECK(o.keep.x == (panel_x0 > 0 ? panel_x0 : 0));
			CHECK(o.over_left == (uint32_t)(panel_x0 > 0 ? panel_x0 : 0));
			check_clip_invariants(in, o);
			steps++;
		}
	}
	CHECK(steps > 900);
}

TEST_CASE("overhang: multi-view grids and odd sizes (#1575)", "[comp_segments][overhang]")
{
	comp_segments_overhang o{};

	SECTION("2x2 tiles")
	{
		const auto in = overhang_in({-3870, -20, 3900, 2200}, 2, 2, 1950, 1100);
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.tile.x == 15);
		CHECK(o.tile.y == 10);
		CHECK(o.tile.w == 1920);
		CHECK(o.tile.h == 1080);
		CHECK(o.canvas.x == 30);
		CHECK(o.canvas.y == 20);
		CHECK(o.canvas.w == 3840);
		CHECK(o.canvas.h == 2160);
		CHECK(o.box_w == 3840);
		CHECK(o.box_h == 2160);
		check_clip_invariants(in, o);
	}
	SECTION("4x1 tiles")
	{
		// Quarter-width views: 4 x 975 = 3900 > 3840.
		const auto in = overhang_in({-3900, 0, 3900, 2000}, 4, 1, 975, 2000);
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.tile.x == 15);
		CHECK(o.tile.w == 960);
		CHECK(o.canvas.x == 60);
		CHECK(o.canvas.w == 3840);
		CHECK(o.box_w == 3840);
		check_clip_invariants(in, o);
	}
	SECTION("odd window, floor-sized tiles")
	{
		// 3847x2169 -> 1923x1084 tiles (the renderer floors), 3 px past the
		// left edge and 5 px above the top: the need rounds out to 1921 tile
		// px, one more than the share, so the share keeps the need's start
		// and still covers every panel pixel.
		const auto in = overhang_in({-3843, -5, 3847, 2169}, 2, 1, 1923, 1084);
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.keep.x == 3);
		CHECK(o.keep.y == 5);
		CHECK(o.keep.w == 3840);
		CHECK(o.keep.h == 2160);
		CHECK(o.need.x == 1);
		CHECK(o.need.w == 1921);
		CHECK(o.tile.x == 1);
		CHECK(o.tile.w == 1920);
		CHECK(o.tile.y == 0);
		CHECK(o.tile.h == 1084);
		CHECK(o.canvas.x == 2);
		CHECK(o.canvas.w == 3841);
		CHECK(contains(o.canvas, o.keep));
		check_clip_invariants(in, o);
	}
	SECTION("tiles not at the panel's scale keep their start and lose their far part")
	{
		// Full-window tiles side by side (8000 wide): the kept 3840 px of each
		// is twice the bridge's share. The cut starts at the panel's edge and
		// the DP canvas is the cut's own 1920 px.
		const auto in = overhang_in({-3920, 0, 4000, 1000}, 2, 1, 4000, 1000);
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.tile.x == 80);
		CHECK(o.tile.w == 1920);
		CHECK(o.canvas.x == 80);
		CHECK(o.canvas.w == 1920);
		CHECK(o.box_w == 3840);
		check_clip_invariants(in, o);
	}
	SECTION("a canvas inside the window")
	{
		// An output rect: the atlas maps onto (200,100) 3900x1800 of a
		// 4200-wide window; the canvas's right 60 px are past the panel.
		auto in = overhang_in({-4040, 0, 4200, 2000}, 2, 1, 1950, 900);
		in.canvas = {200, 100, 3900, 1800};
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.over_left == 0);
		CHECK(o.over_right == 60);
		CHECK(o.tile.x == 0);
		CHECK(o.tile.w == 1920);
		CHECK(o.canvas.x == 200);
		CHECK(o.canvas.y == 100);
		CHECK(o.canvas.w == 3840);
		CHECK(o.canvas.h == 1800);
		check_clip_invariants(in, o);
	}
}

TEST_CASE("overhang: what is not clipped (#1575)", "[comp_segments][overhang]")
{
	comp_segments_overhang o{};

	// Too big and wholly off the panel: nothing of it reaches the panel.
	auto in = overhang_in({100, 0, 3900, 1000}, 2, 1, 1950, 500);
	CHECK(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_OFF_PANEL);
	CHECK(o.over_right == 3900);
	CHECK(o.box_w == 0);

	// Touching the panel's edge is not on it (a zero-area intersection).
	in = overhang_in({0, 0, 3900, 1000}, 2, 1, 1950, 500);
	CHECK(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_OFF_PANEL);

	// Too big but wholly on the panel: tiles larger than the panel's own
	// (#1603's 1x2 box of full-panel tiles). Clipping to the panel cannot
	// cure that; the caller keeps its refusal.
	in = overhang_in({-2880, 0, 1920, 2160}, 1, 2, 1920, 2160);
	CHECK(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_ON_PANEL);
	CHECK(o.box_w == 0);

	// Degenerate inputs.
	in = overhang_in({-2873, 597, 3848, 2169}, 0, 1, 1924, 1084);
	CHECK(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_NO_INPUT);
	in = overhang_in({-2873, 597, 3848, 2169}, 2, 1, 1924, 1084);
	in.panel.w = 0;
	CHECK(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_NO_INPUT);
	in = overhang_in({-2873, 597, 3848, 2169}, 2, 1, 1924, 1084);
	in.max_w = 0;
	CHECK(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_NO_INPUT);
	CHECK(comp_segments_overhang_clip(nullptr, &o) == COMP_SEG_OVERHANG_NO_INPUT);
	CHECK(comp_segments_overhang_clip(&in, nullptr) == COMP_SEG_OVERHANG_NO_INPUT);
}

TEST_CASE("tile rects: a segment that starts outside the canvas maps the part inside it (#1575)",
          "[comp_segments][overhang]")
{
	// The weave cuts segments from the LIVE window, while a clipped slot's
	// canvas is the cut when the frame was submitted. During a drag the
	// panel's live segment can start a few px before it.
	const comp_seg_rect canvas = {60, 0, 3840, 2000}; // the clipped slot, window px
	const comp_seg_rect live = {55, 0, 3845, 2000};   // the panel's segment now
	comp_seg_rect t;
	REQUIRE(comp_segments_tile_rect(&live, &canvas, 1920, 1000, &t));
	CHECK(t.x == 0);
	CHECK(t.y == 0);
	CHECK(t.w == 1920);
	CHECK(t.h == 1000);

	// Above the canvas too.
	const comp_seg_rect above = {60, -8, 1920, 508};
	REQUIRE(comp_segments_tile_rect(&above, &canvas, 1920, 1000, &t));
	CHECK(t.y == 0);
	CHECK(t.h == 250);
	CHECK(t.x == 0);
	CHECK(t.w == 960);

	// A segment beside the clipped canvas (another screen's) has no part in it.
	const comp_seg_rect beside = {3900, 0, 975, 1003};
	CHECK_FALSE(comp_segments_tile_rect(&beside, &canvas, 1920, 1000, &t));
}

TEST_CASE("rect intersect (#1575)", "[comp_segments][overhang]")
{
	comp_seg_rect r{};
	const comp_seg_rect a = {0, 0, 3840, 2169};
	const comp_seg_rect b = {2873, 0, 975, 1003};
	REQUIRE(comp_segments_rect_intersect(&a, &b, &r));
	CHECK(r.x == 2873);
	CHECK(r.y == 0);
	CHECK(r.w == 967);
	CHECK(r.h == 1003);
	// Touching is not overlapping.
	const comp_seg_rect c = {3840, 0, 10, 10};
	CHECK_FALSE(comp_segments_rect_intersect(&a, &c, &r));
	CHECK(r.w == 0);
	// Negative origins.
	const comp_seg_rect d = {-100, -50, 200, 100};
	REQUIRE(comp_segments_rect_intersect(&a, &d, &r));
	CHECK(r.x == 0);
	CHECK(r.y == 0);
	CHECK(r.w == 100);
	CHECK(r.h == 50);
}

/*
 * #1575: the overhang episode's proof (its end line). A resize restarts it;
 * the DP input's baseline waits for the weave to catch up with the box.
 */
namespace {

// The counts and sizes the hub reads before each clipped frame's submit.
struct proof_rig
{
	comp_segments_overhang_proof p{};
	uint64_t rebuilds = 0;
	uint64_t dp_creates = 0;
	uint32_t dp_w = 0, dp_h = 0;
	uint32_t eg_w = 0, eg_h = 0;

	comp_segments_overhang_proof_event
	frame(uint32_t win_w, uint32_t win_h, uint32_t view_w, uint32_t view_h, uint32_t box_w, uint32_t box_h)
	{
		comp_segments_overhang_proof_in in{};
		in.window_w = win_w;
		in.window_h = win_h;
		in.cols = 2;
		in.rows = 1;
		in.view_w = view_w;
		in.view_h = view_h;
		in.box_w = box_w;
		in.box_h = box_h;
		in.egress_rebuilds = rebuilds;
		in.dp_creates = dp_creates;
		in.dp_settled = (eg_w == box_w && eg_h == box_h) || (dp_w == box_w && dp_h == box_h);
		return comp_segments_overhang_proof_step(&p, &in);
	}

	// The weave crops a slot of this box into a new DP input.
	void
	dp_made(uint32_t w, uint32_t h)
	{
		dp_creates++;
		dp_w = w;
		dp_h = h;
	}

	comp_segments_overhang_proof_result
	end()
	{
		comp_segments_overhang_proof_result r{};
		REQUIRE(comp_segments_overhang_proof_end(&p, rebuilds, dp_creates, &r));
		CHECK_FALSE(p.open);
		return r;
	}
};

} // namespace

TEST_CASE("overhang proof: the 2026-10-28 right drag — Windows' rescale tail is a resize, the drag proves "
          "0/0/0 (#1575)",
          "[comp_segments][overhang]")
{
	// G16, cube_handle_d3d12_win at 5220 px wide. Windows rescaled it as its
	// majority came back onto the panel: 2606x427 -> 5220x865 -> 5220x863.
	// The ring was worst-case (churn since 14:16:13.920, latched later), so
	// every slot went through the output-device crop and its DP input.
	proof_rig r;
	r.rebuilds = 10;
	r.eg_w = 3840;
	r.eg_h = 2160;
	r.dp_creates = 40;
	r.dp_w = 2606; // the DP input of the 2606x427 window's slots
	r.dp_h = 213;

	// 14:16:15.499: the first frame, tiles painted for 5220x865.
	CHECK(r.frame(5220, 863, 2610, 432, 3840, 432) == COMP_SEG_PROOF_STARTED);
	// 14:16:15.504: `scanout swapchain follows the window, 5220x865 -> 5220x863`.
	CHECK(r.frame(5220, 863, 2610, 431, 3840, 431) == COMP_SEG_PROOF_RESIZED);
	// 14:16:15.505: the weave is still on an older slot (2592x380's 2592x190 box).
	r.dp_made(2592, 190);
	CHECK(r.frame(5220, 863, 2610, 431, 3840, 431) == COMP_SEG_PROOF_FRAME);
	// 14:16:15.508: the first 3840x431 slot reaches the weave.
	r.dp_made(3840, 431);
	// The drag, 14:16:15.5 - 14:16:27.3: no size line at all.
	for (int i = 0; i < 667; i++) {
		CHECK(r.frame(5220, 863, 2610, 431, 3840, 431) == COMP_SEG_PROOF_FRAME);
	}
	const auto res = r.end();
	CHECK(res.frames == 670);
	CHECK(res.resizes == 1);
	CHECK(res.frames_at_size == 669);
	REQUIRE(res.have_counts);
	CHECK(res.box_w == 3840);
	CHECK(res.box_h == 431);
	CHECK(res.box_changes == 0);
	CHECK(res.egress_rebuilds == 0);
	CHECK(res.dp_creates == 0);
	// The baseline taken on the second submit read the same run as
	// "box changed 1 times, ... DP input re-created 2 times".
	CHECK(r.dp_creates - 40 == 2);
}

TEST_CASE("overhang proof: the episode's own first box reaching the weave is not a re-create (#1575)",
          "[comp_segments][overhang]")
{
	SECTION("latched worst-case ring: 2026-10-28 14:19:15, restore from maximize, then a pure drag")
	{
		proof_rig r;
		r.rebuilds = 10;
		r.eg_w = 3840;
		r.eg_h = 2160;
		r.dp_creates = 60;
		r.dp_w = 3840; // the maximized window's 3840x974 box
		r.dp_h = 974;
		CHECK(r.frame(5220, 863, 2610, 431, 3840, 431) == COMP_SEG_PROOF_STARTED);
		CHECK(r.frame(5220, 863, 2610, 431, 3840, 431) == COMP_SEG_PROOF_FRAME); // weave a frame behind
		r.dp_made(3840, 431);
		for (int i = 0; i < 2494; i++) {
			CHECK(r.frame(5220, 863, 2610, 431, 3840, 431) == COMP_SEG_PROOF_FRAME);
		}
		const auto res = r.end();
		CHECK(res.frames == 2496);
		CHECK(res.resizes == 0);
		REQUIRE(res.have_counts);
		CHECK(res.box_changes == 0);
		CHECK(res.egress_rebuilds == 0);
		CHECK(res.dp_creates == 0); // the run logged 1
	}
	SECTION("content-sized ring: no crop, no DP input, settled at once (2026-10-28 D2 drag)")
	{
		proof_rig r;
		r.rebuilds = 1;
		CHECK(r.frame(3848, 2169, 1924, 1084, 3840, 1084) == COMP_SEG_PROOF_STARTED);
		// The first frame's submit sizes the ring to the box: `rebuild #2`.
		r.rebuilds = 2;
		r.eg_w = 3840;
		r.eg_h = 1084;
		for (int i = 0; i < 8878; i++) {
			CHECK(r.frame(3848, 2169, 1924, 1084, 3840, 1084) == COMP_SEG_PROOF_FRAME);
		}
		const auto res = r.end();
		CHECK(res.frames == 8879);
		REQUIRE(res.have_counts);
		CHECK(res.box_changes == 0);
		CHECK(res.egress_rebuilds == 0);
		CHECK(res.dp_creates == 0);
	}
}

TEST_CASE("overhang proof: a drag that does reallocate is still caught (#1575)", "[comp_segments][overhang]")
{
	proof_rig r;
	r.eg_w = 3840;
	r.eg_h = 1084;
	CHECK(r.frame(3848, 2169, 1924, 1084, 3840, 1084) == COMP_SEG_PROOF_STARTED);
	for (int i = 0; i < 10; i++) {
		CHECK(r.frame(3848, 2169, 1924, 1084, 3840, 1084) == COMP_SEG_PROOF_FRAME);
	}
	// A cut that followed the window's position: the box changes at one
	// window size and one tile size, on every frame of the drag.
	for (uint32_t w = 3000; w < 3010; w++) {
		CHECK(r.frame(3848, 2169, 1924, 1084, w, 1084) == COMP_SEG_PROOF_FRAME);
		r.rebuilds++;
		r.dp_made(w, 1084);
	}
	const auto res = r.end();
	CHECK(res.resizes == 0);
	REQUIRE(res.have_counts);
	CHECK(res.box_changes == 10);
	CHECK(res.egress_rebuilds == 10);
	CHECK(res.dp_creates == 10);
}

TEST_CASE("overhang proof: an interactive resize opens one episode and the counts cover its last size (#1575)",
          "[comp_segments][overhang]")
{
	proof_rig r;
	r.eg_w = 3840;
	r.eg_h = 2160; // the churn's worst-case ring
	CHECK(r.frame(3898, 673, 1949, 336, 3840, 336) == COMP_SEG_PROOF_STARTED);
	uint32_t starts = 0, resizes = 0;
	for (uint32_t w = 3900; w <= 5220; w += 20) {
		const uint32_t h = 673 + (w - 3900) / 7;
		const auto ev = r.frame(w, h, w / 2, h / 2, 3840, h / 2);
		starts += ev == COMP_SEG_PROOF_STARTED;
		resizes += ev == COMP_SEG_PROOF_RESIZED;
		r.dp_made(3840, h / 2);
		r.rebuilds += (w % 200 == 0); // whatever the hysteresis does meanwhile
	}
	CHECK(starts == 0);
	CHECK(resizes == 67);
	const uint32_t last_h = (673 + (5220 - 3900) / 7) / 2;
	for (int i = 0; i < 300; i++) {
		CHECK(r.frame(5220, 673 + (5220 - 3900) / 7, 2610, last_h, 3840, last_h) == COMP_SEG_PROOF_FRAME);
	}
	// The R2 settle, 2 s after the last size change, is a real rebuild and is
	// counted; with a content-sized ring the weave crops nothing more.
	r.rebuilds++;
	r.eg_w = 3840;
	r.eg_h = last_h;
	for (int i = 0; i < 300; i++) {
		CHECK(r.frame(5220, 673 + (5220 - 3900) / 7, 2610, last_h, 3840, last_h) == COMP_SEG_PROOF_FRAME);
	}
	const auto res = r.end();
	CHECK(res.resizes == 67);
	CHECK(res.frames_at_size == 601);
	REQUIRE(res.have_counts);
	CHECK(res.box_changes == 0);
	CHECK(res.egress_rebuilds == 1);
	CHECK(res.dp_creates == 0);
}

TEST_CASE("overhang proof: the DP baseline waits a bounded time, and a short last size is not counted (#1575)",
          "[comp_segments][overhang]")
{
	SECTION("a DP input that never reaches the box")
	{
		proof_rig r;
		r.eg_w = 3840;
		r.eg_h = 2160;
		r.dp_w = 100;
		r.dp_h = 100;
		CHECK(r.frame(5220, 863, 2610, 431, 3840, 431) == COMP_SEG_PROOF_STARTED);
		for (uint32_t i = 0; i < COMP_SEG_OVERHANG_PROOF_DP_WAIT + 1; i++) {
			r.frame(5220, 863, 2610, 431, 3840, 431);
		}
		CHECK(r.p.dp_base_set);
		r.dp_made(200, 200); // after the baseline: counted
		r.frame(5220, 863, 2610, 431, 3840, 431);
		const auto res = r.end();
		REQUIRE(res.have_counts);
		CHECK(res.dp_creates == 1);
	}
	SECTION("the episode ends on the frame after its last resize")
	{
		proof_rig r;
		CHECK(r.frame(5220, 863, 2610, 432, 3840, 432) == COMP_SEG_PROOF_STARTED);
		CHECK(r.frame(5220, 863, 2610, 431, 3840, 431) == COMP_SEG_PROOF_RESIZED);
		const auto res = r.end();
		CHECK(res.frames == 2);
		CHECK(res.resizes == 1);
		CHECK(res.frames_at_size == 1);
		CHECK_FALSE(res.have_counts);
		CHECK(res.dp_creates == 0);
	}
	SECTION("no episode")
	{
		comp_segments_overhang_proof p{};
		comp_segments_overhang_proof_result res{};
		res.frames = 7;
		CHECK_FALSE(comp_segments_overhang_proof_end(&p, 1, 1, &res));
		CHECK(res.frames == 0);
	}
}

TEST_CASE("overhang: the 2026-10-28 D3 window — a drag that keeps its majority on the panel holds one box, and the "
          "bridge carries 3840 px of its 5220 (#1575)",
          "[comp_segments][overhang]")
{
	// 5220x863 with 2x1 tiles of 2610x431: the bridge's per-tile share is
	// 1920, so the cut is 3840 window px wide wherever the window is. The
	// window's far 1380 px from the panel's part are not carried: that is the
	// black the laptop showed. The panel's part is always covered.
	comp_segments_overhang_proof p{};
	uint32_t starts = 0;
	for (int32_t x = -5000; x <= -2900; x += 13) {
		CAPTURE(x);
		const auto in = overhang_in({x, 416, 5220, 863}, 2, 1, 2610, 431);
		comp_segments_overhang o{};
		REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
		CHECK(o.box_w == 3840);
		CHECK(o.box_h == 431);
		CHECK(o.canvas.w == 3840);
		check_clip_invariants(in, o);

		comp_segments_overhang_proof_in pin{};
		pin.window_w = 5220;
		pin.window_h = 863;
		pin.cols = 2;
		pin.rows = 1;
		pin.view_w = 2610;
		pin.view_h = 431;
		pin.box_w = o.box_w;
		pin.box_h = o.box_h;
		pin.dp_settled = true;
		starts += comp_segments_overhang_proof_step(&p, &pin) == COMP_SEG_PROOF_STARTED;
	}
	CHECK(starts == 1);
	comp_segments_overhang_proof_result res{};
	REQUIRE(comp_segments_overhang_proof_end(&p, 0, 0, &res));
	REQUIRE(res.have_counts);
	CHECK(res.resizes == 0);
	CHECK(res.box_changes == 0);

	// The episode's first frame in the log: `the window (5220x863 at -3476,416)
	// ... the DP weaves 3840x863 at 0,0` — on the laptop (x >= 0) only 364 px
	// of its 1744 px there are carried.
	const auto in = overhang_in({-3476, 416, 5220, 863}, 2, 1, 2610, 432);
	comp_segments_overhang o{};
	REQUIRE(comp_segments_overhang_clip(&in, &o) == COMP_SEG_OVERHANG_CLIPPED);
	CHECK(o.canvas.x == 0);
	CHECK(o.canvas.w == 3840);
	CHECK(-3476 + o.canvas.x + (int32_t)o.canvas.w == 364);
}
