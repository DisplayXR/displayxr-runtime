// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-047 Amendment 10: every screen's own mode table, the effective
 *         table of a window (wholly on one screen / straddling / rotated /
 *         clamped / an entry the session cannot fill), the hysteresis, the
 *         worst case over every screen, the hotplug fit, the per-segment grid
 *         a segment DP is handed, and the registry's choice re-read.
 */

#include "catch_amalgamated.hpp"

#include "util/u_screen_modes.h"
#include "util/comp_segments_view_size.h"
#include "xrt/xrt_display_status.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

xrt_rendering_mode
mode(const char *name, uint32_t views, float sx, float sy, uint32_t cols, uint32_t rows, bool hw3d, uint32_t flags = 0)
{
	xrt_rendering_mode m;
	std::memset(&m, 0, sizeof(m));
	std::snprintf(m.mode_name, sizeof(m.mode_name), "%s", name);
	m.view_count = views;
	m.view_scale_x = sx;
	m.view_scale_y = sy;
	m.tile_columns = cols;
	m.tile_rows = rows;
	m.hardware_display_3d = hw3d;
	m.mode_flags = flags;
	return m;
}

//! sim_display's shape: 2D, Same as 2D (default), Anaglyph, Squeezed SBS, Quad (start-up only).
u_screen_mode_table
sim_table(uint64_t id, uint32_t w, uint32_t h)
{
	xrt_rendering_mode list[5] = {
	    mode("2D", 1, 1.0f, 1.0f, 1, 1, false),
	    mode("Same as 2D", 1, 1.0f, 1.0f, 1, 1, false, XRT_RENDERING_MODE_FLAG_3D_PASSTHROUGH),
	    mode("Anaglyph", 2, 0.5f, 0.5f, 2, 1, true),
	    mode("Squeezed SBS", 2, 0.5f, 1.0f, 2, 1, true),
	    mode("Quad", 4, 0.5f, 0.5f, 2, 2, true, XRT_RENDERING_MODE_FLAG_START_UP_ONLY),
	};
	u_screen_mode_table t;
	std::memset(&t, 0, sizeof(t));
	t.screen_id = id;
	std::snprintf(t.key, sizeof(t.key), "SIM-%llu", (unsigned long long)id);
	t.sized = true;
	t.list_source = U_SCREEN_MODES_LIST_PLUGIN;
	u_screen_modes_table_set_list(&t, list, 5, 1, w, h);
	return t;
}

//! A lenticular panel's shape: 2D and one 2-view 3D mode at half width.
u_screen_mode_table
panel_table(uint64_t id, uint32_t w, uint32_t h)
{
	xrt_rendering_mode list[2] = {
	    mode("2D", 1, 1.0f, 1.0f, 1, 1, false),
	    mode("3D", 2, 0.5f, 1.0f, 2, 1, true, XRT_RENDERING_MODE_FLAG_HAS_TRACKING),
	};
	u_screen_mode_table t;
	std::memset(&t, 0, sizeof(t));
	t.screen_id = id;
	t.sized = true;
	t.list_source = U_SCREEN_MODES_LIST_HEAD;
	u_screen_modes_table_set_list(&t, list, 2, 1, w, h);
	return t;
}

u_screen_modes_cover
cover_of(const u_screen_mode_table &t)
{
	u_screen_modes_cover c;
	std::memset(&c, 0, sizeof(c));
	c.screen_id = t.screen_id;
	c.slot[0] = u_screen_modes_table_slot(&t, 0);
	c.slot[1] = u_screen_modes_table_slot(&t, 1);
	c.pixel_width = t.pixel_width;
	c.pixel_height = t.pixel_height;
	c.rotated = t.rotated;
	return c;
}

} // namespace

TEST_CASE("screen modes: a sim screen's table, slot 1 = the driver default, then the user's choice")
{
	u_screen_mode_table t = sim_table(7, 2880, 1800);
	CHECK(t.count == 5);
	CHECK(t.envelope_count == 4); // Quad is START_UP_ONLY, trailing
	CHECK(t.slot1 == 1);
	CHECK(t.source == XRT_STATUS_MODE_3D_SOURCE_DRIVER_DEFAULT);
	CHECK(std::string(u_screen_modes_table_slot(&t, 1)->mode_name) == "Same as 2D");
	CHECK(t.modes[2].view_width_pixels == 1440);
	CHECK(t.modes[2].atlas_width_pixels == 2880);

	// A choice by slug or by label, any case.
	CHECK(u_screen_modes_table_apply_choice(&t, "anaglyph", XRT_STATUS_MODE_3D_SOURCE_USER));
	CHECK(t.slot1 == 2);
	CHECK(t.source == XRT_STATUS_MODE_3D_SOURCE_USER);
	CHECK(std::string(t.setting) == "anaglyph");
	CHECK_FALSE(u_screen_modes_table_apply_choice(&t, "anaglyph", XRT_STATUS_MODE_3D_SOURCE_USER));
	CHECK(u_screen_modes_table_apply_choice(&t, "Squeezed SBS", XRT_STATUS_MODE_3D_SOURCE_MACHINE));
	CHECK(t.slot1 == 3);
	// A start-up-only entry, or an unknown name, is not honoured: the default.
	CHECK(u_screen_modes_table_apply_choice(&t, "quad", XRT_STATUS_MODE_3D_SOURCE_USER));
	CHECK(t.slot1 == 1);
	CHECK(t.setting[0] == '\0');
	(void)u_screen_modes_table_apply_choice(&t, "nonsense", XRT_STATUS_MODE_3D_SOURCE_USER);
	CHECK(t.slot1 == 1);
	CHECK(u_screen_modes_table_slot_index(&t, 0) == 0);
	CHECK(u_screen_modes_table_slot_index(&t, 1) == 1);
}

TEST_CASE("screen modes: status rows list every 3D entry with applies/reason")
{
	u_screen_mode_table t = sim_table(7, 2880, 1800);
	xrt_status_mode_3d st;
	u_screen_modes_fill_status(&t, true, &st);
	CHECK(st.valid);
	CHECK(std::string(st.active) == "same_as_2d");
	CHECK(st.choice_count == 4);
	CHECK(std::string(st.choices[0].name) == "same_as_2d");
	CHECK(st.choices[0].passthrough);
	CHECK(st.choices[3].applies == XRT_STATUS_MODE_3D_APPLIES_RESTART_APP); // Quad
	CHECK(st.choices[1].applies == XRT_STATUS_MODE_3D_APPLIES_NOW);

	// A screen that came after the instance and does not fit: restart_app everywhere.
	u_screen_modes_fill_status(&t, false, &st);
	CHECK(st.choices[1].applies == XRT_STATUS_MODE_3D_APPLIES_RESTART_APP);
}

TEST_CASE("screen modes: wholly on one screen, the table is that screen's")
{
	u_screen_mode_table laptop = sim_table(7, 2880, 1800);
	u_screen_modes_cover c = cover_of(laptop);
	xrt_effective_mode_table e;
	u_screen_modes_effective(&c, 1, 0, 2, 1440, 900, 3840, 2160, &e);
	REQUIRE(e.valid);
	CHECK(e.majority_screen_id == 7);
	CHECK(e.slot[1].view_count == 1);
	CHECK(e.slot[1].tile_columns == 1);
	CHECK(e.slot[1].view_scale_x == 1.0f);
	CHECK(std::string(e.slot[1].mode_name) == "Same as 2D");
	CHECK_FALSE(e.slot[1].hardware_display_3d);
	CHECK(e.slot[1].view_width_pixels == 2880);

	(void)u_screen_modes_table_apply_choice(&laptop, "anaglyph", XRT_STATUS_MODE_3D_SOURCE_USER);
	c = cover_of(laptop);
	u_screen_modes_effective(&c, 1, 0, 2, 1440, 900, 3840, 2160, &e);
	CHECK(e.slot[1].view_count == 2);
	CHECK(e.slot[1].tile_columns == 2);
	CHECK(e.slot[1].view_scale_x == 0.5f);
	CHECK(e.slot[1].view_width_pixels == 1440);
}

TEST_CASE("screen modes: a straddle takes the most views and the per-dimension max scale")
{
	u_screen_mode_table panel = panel_table(1, 3840, 2160);
	u_screen_mode_table laptop = sim_table(7, 2880, 1800);
	u_screen_modes_cover c[2] = {cover_of(panel), cover_of(laptop)};
	xrt_effective_mode_table e;

	// Majority on the laptop ("Same as 2D", one view): the panel's 2-view layout
	// wins on view count; the scale is the max of 0.5 x 1.0 and 1.0 x 1.0.
	u_screen_modes_effective(c, 2, 1, 2, 0, 0, 0, 0, &e);
	REQUIRE(e.valid);
	CHECK(e.majority_screen_id == 7);
	CHECK(e.layout_screen_id[1] == 1);
	CHECK(e.slot[1].view_count == 2);
	CHECK(e.slot[1].tile_columns == 2);
	CHECK(e.slot[1].view_scale_x == 1.0f);
	CHECK(e.slot[1].view_scale_y == 1.0f);
	CHECK(e.slot[1].hardware_display_3d);
	CHECK(e.slot[0].view_count == 1);
	CHECK(e.slot[0].view_scale_x == 1.0f);

	// The clamp: a 2-tile row at full window width does not fit a 3840-wide atlas
	// for a 2560-px window -> the x scale is cut to 3840 / (2 x 2560) = 0.75.
	u_screen_modes_effective(c, 2, 1, 2, 2560, 1000, 3840, 2160, &e);
	CHECK(e.clamped[1]);
	CHECK(e.slot[1].view_scale_x == Catch::Approx(0.75f));
	CHECK(e.slot[1].view_scale_y == 1.0f);
	CHECK_FALSE(e.clamped[0]);

	// A tie on view count goes to the majority.
	(void)u_screen_modes_table_apply_choice(&laptop, "anaglyph", XRT_STATUS_MODE_3D_SOURCE_USER);
	c[1] = cover_of(laptop);
	u_screen_modes_effective(c, 2, 1, 2, 0, 0, 0, 0, &e);
	CHECK(e.layout_screen_id[1] == 7);
	CHECK(std::string(e.slot[1].mode_name) == "Anaglyph");
	CHECK(e.slot[1].view_scale_x == 0.5f); // max(0.5, 0.5)
	CHECK(e.slot[1].view_scale_y == 1.0f); // max(1.0, 0.5)
}

TEST_CASE("screen modes: a rotated screen's scales are swapped into window axes")
{
	u_screen_mode_table portrait = sim_table(9, 3840, 2160);
	(void)u_screen_modes_table_apply_choice(&portrait, "squeezed_sbs", XRT_STATUS_MODE_3D_SOURCE_USER);
	portrait.rotated = true;
	u_screen_modes_cover c = cover_of(portrait);
	xrt_effective_mode_table e;
	u_screen_modes_effective(&c, 1, 0, 2, 0, 0, 0, 0, &e);
	CHECK(e.slot[1].view_scale_x == 1.0f); // the panel's y scale
	CHECK(e.slot[1].view_scale_y == 0.5f);
	CHECK(e.slot[1].tile_columns == 2); // the grid is the app's layout: not rotated
	CHECK(e.slot[1].view_width_pixels == 2160);
	CHECK(e.slot[1].view_height_pixels == 1920);
}

TEST_CASE("screen modes: an entry the session cannot fill shows its screen in 2D (decision 5)")
{
	xrt_rendering_mode list[2] = {mode("2D", 1, 1.0f, 1.0f, 1, 1, false), mode("Quad", 4, 0.5f, 0.5f, 2, 2, true)};
	u_screen_mode_table quad;
	std::memset(&quad, 0, sizeof(quad));
	quad.screen_id = 3;
	u_screen_modes_table_set_list(&quad, list, 2, 1, 1920, 1080);
	u_screen_modes_cover c = cover_of(quad);
	xrt_effective_mode_table e;
	u_screen_modes_effective(&c, 1, 0, 2, 0, 0, 0, 0, &e); // a 2-view session
	CHECK(e.slot[1].view_count == 1);
	CHECK(std::string(e.slot[1].mode_name) == "2D");
	u_screen_modes_effective(&c, 1, 0, 4, 0, 0, 0, 0, &e); // a 4-view session fills it
	CHECK(e.slot[1].view_count == 4);
	CHECK(e.slot[1].tile_rows == 2);
}

TEST_CASE("screen modes: the dwell holds a new covered set for 0.5 s; the majority has a 20 % dead band")
{
	const uint64_t ms = 1000ull * 1000ull;
	u_screen_modes_dwell d;
	u_screen_modes_dwell_init(&d, 0, 0);
	uint64_t ids1[1] = {1};
	uint32_t a1[1] = {100};
	CHECK(u_screen_modes_dwell_update(&d, ids1, a1, 1, 0)); // first placement: at once
	CHECK(d.stable_majority == 1);

	// Onto the laptop wholly: pending until 500 ms.
	uint64_t ids7[1] = {7};
	CHECK_FALSE(u_screen_modes_dwell_update(&d, ids7, a1, 1, 100 * ms));
	CHECK_FALSE(u_screen_modes_dwell_update(&d, ids7, a1, 1, 400 * ms));
	CHECK(u_screen_modes_dwell_update(&d, ids7, a1, 1, 600 * ms));
	CHECK(d.stable_majority == 7);
	CHECK(u_screen_modes_dwell_has(&d, 7));

	// Back and forth faster than the hold: nothing changes.
	CHECK_FALSE(u_screen_modes_dwell_update(&d, ids1, a1, 1, 700 * ms));
	CHECK_FALSE(u_screen_modes_dwell_update(&d, ids7, a1, 1, 800 * ms));
	CHECK_FALSE(u_screen_modes_dwell_update(&d, ids1, a1, 1, 900 * ms));
	CHECK_FALSE(u_screen_modes_dwell_update(&d, ids7, a1, 1, 1300 * ms));
	CHECK(d.stable_majority == 7);

	// Straddling 55/45 with the laptop the current majority: within the margin,
	// the laptop stays the majority (the set {1, 7} still changes, after 0.5 s).
	uint64_t both[2] = {1, 7};
	uint32_t a55[2] = {55, 45};
	CHECK_FALSE(u_screen_modes_dwell_update(&d, both, a55, 2, 2000 * ms));
	CHECK(u_screen_modes_dwell_update(&d, both, a55, 2, 2600 * ms));
	CHECK(d.stable_n == 2);
	CHECK(d.stable_majority == 7);
	// 70/30: beyond the margin; the panel takes the majority after the hold.
	uint32_t a70[2] = {70, 30};
	CHECK_FALSE(u_screen_modes_dwell_update(&d, both, a70, 2, 3000 * ms));
	CHECK(u_screen_modes_dwell_update(&d, both, a70, 2, 3600 * ms));
	CHECK(d.stable_majority == 1);
	// An empty set keeps the stable one.
	CHECK_FALSE(u_screen_modes_dwell_update(&d, nullptr, nullptr, 0, 4000 * ms));
	CHECK(d.stable_n == 2);
}

TEST_CASE("screen modes: the worst case is the union over every sized screen")
{
	u_screen_mode_table t[2] = {panel_table(1, 3840, 2160), sim_table(7, 2880, 1800)};
	uint32_t aw = 0, ah = 0, vw = 0, vh = 0;
	u_screen_modes_union_atlas(t, 2, &aw, &ah, &vw, &vh);
	CHECK(aw == 3840);
	CHECK(ah == 2160);
	CHECK(vw == 3840);
	CHECK(vh == 2160);

	// A super-ultrawide next to a 4K panel: 5120 x 2160.
	u_screen_mode_table w[2] = {panel_table(1, 3840, 2160), sim_table(8, 5120, 1440)};
	aw = ah = vw = vh = 0;
	u_screen_modes_union_atlas(w, 2, &aw, &ah, &vw, &vh);
	CHECK(aw == 5120);
	CHECK(ah == 2160);

	// A portrait 4K: 3840 tall from its 2D entry; and wider than the landscape
	// panel, because its Squeezed SBS entry (0.5 x 1.0 in panel axes) is two
	// FULL-width views side by side in window axes: 2 x 2160 = 4320.
	u_screen_mode_table p[2] = {panel_table(1, 3840, 2160), sim_table(9, 3840, 2160)};
	p[1].rotated = true;
	aw = ah = vw = vh = 0;
	u_screen_modes_union_atlas(p, 2, &aw, &ah, &vw, &vh);
	CHECK(aw == 4320);
	CHECK(ah == 3840);
	CHECK(vw == 3840); // the landscape panel's 2D view
	CHECK(vh == 3840); // the portrait's 2D view

	// Opted out: not counted.
	w[1].sized = false;
	aw = ah = vw = vh = 0;
	u_screen_modes_union_atlas(w, 2, &aw, &ah, &vw, &vh);
	CHECK(aw == 3840);

	// Hotplug: a 5120-wide screen does not fit a 3840 x 2160 instance.
	CHECK_FALSE(u_screen_modes_table_fits(&w[1], 3840, 2160));
	CHECK(u_screen_modes_table_fits(&t[1], 3840, 2160));
}

TEST_CASE("screen modes: the sizing opt-out list")
{
	CHECK(u_screen_modes_key_listed("SDC-4171-0", "SDC-4171-0"));
	CHECK(u_screen_modes_key_listed(" a ; sdc-4171-0 ,b", "SDC-4171-0"));
	CHECK_FALSE(u_screen_modes_key_listed("SDC-4171-00", "SDC-4171-0"));
	CHECK_FALSE(u_screen_modes_key_listed("", "x"));
	CHECK_FALSE(u_screen_modes_key_listed(nullptr, "x"));
	CHECK_FALSE(u_screen_modes_key_listed("x", ""));
}

TEST_CASE("screen modes: a segment DP's grid follows its screen's entry")
{
	u_screen_mode_table laptop = sim_table(7, 2880, 1800);
	// Same as 2D: one tile, entry 1.
	comp_segments_segment_grid g =
	    comp_segments_view_size_segment_grid(u_screen_modes_table_slot(&laptop, 1), 2, 1);
	CHECK(g.cols == 1);
	CHECK(g.views == 1);
	CHECK(g.entry == 1);
	// Anaglyph: its own 2 x 1 grid, entry 2.
	(void)u_screen_modes_table_apply_choice(&laptop, "anaglyph", XRT_STATUS_MODE_3D_SOURCE_USER);
	g = comp_segments_view_size_segment_grid(u_screen_modes_table_slot(&laptop, 1), 2, 1);
	CHECK(g.cols == 2);
	CHECK(g.rows == 1);
	CHECK(g.views == 2);
	CHECK(g.entry == 2);
	// The session renders one view (the window's table is Same as 2D elsewhere):
	// the 2-view entry cannot be filled - 2D, entry 0.
	g = comp_segments_view_size_segment_grid(u_screen_modes_table_slot(&laptop, 1), 1, 1);
	CHECK(g.views == 1);
	CHECK(g.entry == 0);
	// The app in 2D: the 2D entry.
	g = comp_segments_view_size_segment_grid(u_screen_modes_table_slot(&laptop, 0), 2, 1);
	CHECK(g.views == 1);
	CHECK(g.entry == 0);
	// No table: the session grid, nothing said.
	g = comp_segments_view_size_segment_grid(nullptr, 2, 1);
	CHECK(g.cols == 2);
	CHECK(g.entry == -1);
}

namespace {
const char *g_choice = nullptr;
bool
fake_resolver(const char *key, char *out, size_t cap, uint32_t *src)
{
	if (g_choice == nullptr || std::strcmp(key, "SIM-7") != 0) {
		return false;
	}
	std::snprintf(out, cap, "%s", g_choice);
	*src = XRT_STATUS_MODE_3D_SOURCE_USER;
	return true;
}
} // namespace

TEST_CASE("screen modes: the registry re-reads choices, bumps its generation, and feeds a manager")
{
	u_screen_mode_table t[2] = {panel_table(1, 3840, 2160), sim_table(7, 2880, 1800)};
	u_screen_modes_publish(t, 2);
	u_screen_modes_set_choice_resolver(fake_resolver);
	const uint64_t g0 = u_screen_modes_generation();

	g_choice = "anaglyph";
	CHECK(u_screen_modes_refresh_choices(1000, true));
	CHECK(u_screen_modes_generation() == g0 + 1);
	u_screen_mode_table got;
	REQUIRE(u_screen_modes_get(7, &got));
	CHECK(got.slot1 == 2);
	// The head's screen is the head device's to remap: untouched.
	REQUIRE(u_screen_modes_get(1, &got));
	CHECK(got.slot1 == 1);
	// Throttled to once a second unless forced.
	g_choice = "squeezed_sbs";
	CHECK_FALSE(u_screen_modes_refresh_choices(2000, false));
	CHECK(u_screen_modes_refresh_choices(2000ull * 1000ull * 1000ull, false));
	REQUIRE(u_screen_modes_get(7, &got));
	CHECK(got.slot1 == 3);

	// A manager: window wholly on the panel (primary) -> invalid (the head's
	// table); wholly on the laptop -> its table after the dwell.
	comp_segments_screen screens[2];
	std::memset(screens, 0, sizeof(screens));
	screens[0].id = 1;
	screens[0].is_primary = true;
	screens[1].id = 7;
	comp_segments_view_size vs;
	comp_segments_view_size_init(&vs);
	xrt_rendering_mode head[2] = {mode("2D", 1, 1.0f, 1.0f, 1, 1, false), mode("3D", 2, 0.5f, 1.0f, 2, 1, true)};
	comp_segment_table seg;
	std::memset(&seg, 0, sizeof(seg));
	seg.count = 1;
	seg.seg[0].screen_id = 1;
	seg.seg[0].screen_index = 0;
	seg.seg[0].is_primary = true;
	seg.seg[0].window_rect = {0, 0, 1000, 800};
	const uint64_t ms = 1000ull * 1000ull;
	CHECK_FALSE(comp_segments_view_size_update(&vs, &seg, screens, 2, head, 2, 2, 3840, 2160, 1000, 800, 0));
	CHECK_FALSE(vs.eff.valid);
	seg.seg[0].screen_id = 7;
	seg.seg[0].screen_index = 1;
	seg.seg[0].is_primary = false;
	CHECK_FALSE(comp_segments_view_size_update(&vs, &seg, screens, 2, head, 2, 2, 3840, 2160, 1000, 800, 100 * ms));
	CHECK(comp_segments_view_size_update(&vs, &seg, screens, 2, head, 2, 2, 3840, 2160, 1000, 800, 700 * ms));
	REQUIRE(vs.eff.valid);
	CHECK(std::string(vs.eff.slot[1].mode_name) == "Squeezed SBS");
	CHECK(vs.eff.slot[1].view_scale_y == 1.0f);
	// Its segment DP draws entry 3 in a 2 x 1 grid.
	const comp_segments_segment_grid g =
	    comp_segments_view_size_segment_grid(comp_segments_view_size_entry(&vs, 1, 1), 2, 1);
	CHECK(g.entry == 3);
	CHECK(g.cols == 2);

	u_screen_modes_set_choice_resolver(nullptr);
	u_screen_modes_clear();
}
