// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The in-process Metal compositor's segment geometry on a mixed-scale
 *         desktop (a 2x Retina panel next to a 1x external display): the
 *         table is cut in points, every segment's canvas and crop are in the
 *         DRAWABLE's backing px, and the view size of ADR-047 Amendment 10 is
 *         evaluated against the drawable, never the window's points (the
 *         #2791 bug class on Wayland).
 */

#include "catch_amalgamated.hpp"

#include "metal/comp_metal_segments_geom.h"
#include "util/u_screen_modes.h"
#include "xrt/xrt_display_status.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr uint64_t kRetina = 0x11;   // built-in panel, 2x, the session screen
constexpr uint64_t kExternal = 0x22; // external display, 1x, claimed by sim-display
constexpr uint64_t kMs = 1000ull * 1000ull;

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

//! sim_display's declared list (sim_display_modes.h order), its default 3D entry = Same as 2D.
u_screen_mode_table
sim_table(uint64_t id, const char *key, uint32_t px_w, uint32_t px_h)
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
	std::snprintf(t.key, sizeof(t.key), "%s", key);
	t.sized = true;
	t.list_source = U_SCREEN_MODES_LIST_PLUGIN;
	u_screen_modes_table_set_list(&t, list, 5, 1, px_w, px_h);
	return t;
}

/*!
 * The screen list as comp_metal_segments_set_screens builds it: CGDisplayBounds
 * in top-down POINTS, native backing px. The Retina panel 1512x982 pt (3024x1964
 * px) at the origin; the external 1920x1080 pt = px to its right.
 */
void
mixed_scale_screens(comp_segments_screen out[2])
{
	std::memset(out, 0, sizeof(comp_segments_screen) * 2);
	out[0].id = kRetina;
	out[0].desktop = {0, 0, 1512, 982};
	out[0].native_w = 3024;
	out[0].native_h = 1964;
	out[0].is_primary = true;
	out[1].id = kExternal;
	out[1].desktop = {1512, 0, 1920, 1080};
	out[1].native_w = 1920;
	out[1].native_h = 1080;
}

//! The window in points and its drawable (the window's backing scale is the
//! scale of the screen holding most of it).
comp_metal_seg_window
window(int32_t x_pt, int32_t y_pt, uint32_t w_pt, uint32_t h_pt, double backing_scale)
{
	comp_metal_seg_window w;
	std::memset(&w, 0, sizeof(w));
	w.frame_pt = {x_pt, y_pt, w_pt, h_pt};
	w.drawable_w = (uint32_t)(w_pt * backing_scale);
	w.drawable_h = (uint32_t)(h_pt * backing_scale);
	return w;
}

const comp_segment *
seg_on(const comp_segment_table &t, uint64_t screen)
{
	for (uint32_t k = 0; k < t.count; k++) {
		if (t.seg[k].screen_id == screen) {
			return &t.seg[k];
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("metal segments: a 2x + 1x straddle is cut in points and each canvas lands in drawable px")
{
	comp_segments_screen screens[2];
	mixed_scale_screens(screens);

	SECTION("majority on the Retina panel: the drawable is 2x")
	{
		// 600 pt on the Retina panel, 400 pt on the external display.
		const comp_metal_seg_window w = window(912, 100, 1000, 600, 2.0);
		comp_segment_table t;
		comp_metal_seg_cut(&w, screens, 2, &t);
		REQUIRE(t.count == 2);
		const comp_segment *r = seg_on(t, kRetina);
		const comp_segment *e = seg_on(t, kExternal);
		REQUIRE(r != nullptr);
		REQUIRE(e != nullptr);
		CHECK(r->window_rect.w == 600); // points
		CHECK(e->window_rect.w == 400);

		const comp_seg_rect rp = comp_metal_seg_window_rect_px(&w, &r->window_rect);
		const comp_seg_rect ep = comp_metal_seg_window_rect_px(&w, &e->window_rect);
		// The seam at 1200 drawable px: the 1x screen's part is 800 px of
		// the 2x drawable (it is resampled by the WindowServer), NOT 400 px
		// (its points x its own scale).
		CHECK(rp.x == 0);
		CHECK(rp.w == 1200);
		CHECK(ep.x == 1200);
		CHECK(ep.w == 800);
		CHECK(ep.h == 1200);
		CHECK(rp.x + (int32_t)rp.w == ep.x); // neighbours share the seam

		// Its crop out of a stereo atlas at view scale 0.5 x 1.0 (tile
		// 1000 x 1200): the external part's tile is 400 px wide at x = 600.
		const comp_seg_rect canvas = {0, 0, w.drawable_w, w.drawable_h};
		comp_seg_rect tile;
		comp_segments_segment_grid grid;
		REQUIRE(comp_metal_seg_crop_plan(&ep, &canvas, 1000, 1200, 2, 1, nullptr, &tile, &grid));
		CHECK(tile.x == 600);
		CHECK(tile.w == 400);
		CHECK(tile.h == 1200);
		CHECK(grid.cols == 2); // no per-screen entry: the session grid
		CHECK(grid.entry == -1);
	}

	SECTION("majority on the external display: the drawable is 1x")
	{
		// 400 pt on the Retina panel, 600 pt on the external display.
		const comp_metal_seg_window w = window(1112, 100, 1000, 600, 1.0);
		comp_segment_table t;
		comp_metal_seg_cut(&w, screens, 2, &t);
		REQUIRE(t.count == 2);
		const comp_seg_rect rp = comp_metal_seg_window_rect_px(&w, &seg_on(t, kRetina)->window_rect);
		const comp_seg_rect ep = comp_metal_seg_window_rect_px(&w, &seg_on(t, kExternal)->window_rect);
		// The 2x screen's part is 400 px of the 1x drawable, not 800.
		CHECK(rp.w == 400);
		CHECK(ep.x == 400);
		CHECK(ep.w == 600);
	}
}

TEST_CASE("metal segments: the view size follows the window's screen, in the drawable's backing px")
{
	u_screen_mode_table tables[2] = {sim_table(kRetina, "SIM-RETINA", 3024, 1964),
	                                 sim_table(kExternal, "SIM-EXTERNAL", 1920, 1080)};
	tables[0].list_source = U_SCREEN_MODES_LIST_HEAD;
	// The external screen's own choice: Anaglyph (`dp mode set anaglyph --screen ...`).
	REQUIRE(u_screen_modes_table_apply_choice(&tables[1], "anaglyph", XRT_STATUS_MODE_3D_SOURCE_USER));
	u_screen_modes_publish(tables, 2);

	comp_segments_screen screens[2];
	mixed_scale_screens(screens);
	// The head device (the Retina panel's sim-display, in Same as 2D).
	const xrt_rendering_mode head[2] = {
	    mode("2D", 1, 1.0f, 1.0f, 1, 1, false),
	    mode("Same as 2D", 1, 1.0f, 1.0f, 1, 1, false, XRT_RENDERING_MODE_FLAG_3D_PASSTHROUGH),
	};

	SECTION("on the session screen the head's table stands; wholly on the external, its own")
	{
		comp_segments_view_size vs;
		comp_segments_view_size_init(&vs);
		comp_segment_table t;

		const comp_metal_seg_window on_retina = window(100, 100, 1000, 600, 2.0);
		comp_metal_seg_cut(&on_retina, screens, 2, &t);
		CHECK_FALSE(comp_metal_seg_view_size_step(&vs, &on_retina, &t, screens, 2, head, 2, 2, 0, 0, 0));
		CHECK_FALSE(vs.eff.valid);

		const comp_metal_seg_window on_ext = window(1700, 100, 1000, 600, 1.0);
		comp_metal_seg_cut(&on_ext, screens, 2, &t);
		CHECK_FALSE(comp_metal_seg_view_size_step(&vs, &on_ext, &t, screens, 2, head, 2, 2, 0, 0, 100 * kMs));
		CHECK(comp_metal_seg_view_size_step(&vs, &on_ext, &t, screens, 2, head, 2, 2, 0, 0, 700 * kMs));
		REQUIRE(vs.eff.valid);
		CHECK(vs.eff.majority_screen_id == kExternal);
		CHECK(std::string(vs.eff.slot[1].mode_name) == "Anaglyph");
		CHECK(vs.eff.slot[1].view_count == 2);
		CHECK(vs.eff.slot[1].tile_columns == 2);
		CHECK(vs.eff.slot[1].view_width_pixels == 960); // the external's 1920 px x 0.5

		// Its segment DP draws entry 2 (Anaglyph) in a 2 x 1 grid; in 2D, entry 0.
		const comp_seg_rect canvas = {0, 0, on_ext.drawable_w, on_ext.drawable_h};
		const comp_seg_rect whole = canvas;
		comp_seg_rect tile;
		comp_segments_segment_grid grid;
		REQUIRE(comp_metal_seg_crop_plan(&whole, &canvas, 500, 300, 2, 1,
		                                 comp_segments_view_size_entry(&vs, 1, 1), &tile, &grid));
		CHECK(grid.entry == 2);
		CHECK(grid.cols == 2);
		CHECK(grid.views == 2);
		REQUIRE(comp_metal_seg_crop_plan(&whole, &canvas, 1000, 600, 1, 1,
		                                 comp_segments_view_size_entry(&vs, 1, 0), &tile, &grid));
		CHECK(grid.entry == 0);
		CHECK(grid.views == 1);
	}

	SECTION("the clamp to the instance's worst case compares the drawable's px, not the window's points")
	{
		comp_segments_view_size vs;
		comp_segments_view_size_init(&vs);
		comp_segment_table t;
		// Straddle with the majority on the Retina panel: a 2x drawable of
		// 2000 x 1200 px for a 1000 x 600 pt window.
		const uint32_t cap_w = 3000, cap_h = 4000;
		const comp_metal_seg_window on_retina = window(100, 100, 1000, 600, 2.0);
		comp_metal_seg_cut(&on_retina, screens, 2, &t);
		(void)comp_metal_seg_view_size_step(&vs, &on_retina, &t, screens, 2, head, 2, 2, cap_w, cap_h, 0);
		const comp_metal_seg_window w = window(912, 100, 1000, 600, 2.0);
		comp_metal_seg_cut(&w, screens, 2, &t);
		CHECK_FALSE(
		    comp_metal_seg_view_size_step(&vs, &w, &t, screens, 2, head, 2, 2, cap_w, cap_h, 100 * kMs));
		REQUIRE(comp_metal_seg_view_size_step(&vs, &w, &t, screens, 2, head, 2, 2, cap_w, cap_h, 700 * kMs));
		REQUIRE(vs.eff.valid);
		CHECK(vs.eff.screen_count == 2);
		CHECK(vs.eff.majority_screen_id == kRetina);
		// Slot 1: the layout of the entry with the most views (the external's
		// Anaglyph, 2 x 1) at the per-dimension max scale (Same as 2D's 1.0).
		CHECK(vs.eff.slot[1].tile_columns == 2);
		CHECK(vs.eff.slot[1].view_count == 2);
		// 2000 px x 1.0 x 2 tiles = 4000 > 3000: clamped to 0.75. Evaluated in
		// points (1000 pt x 1.0 x 2 = 2000) it would wrongly pass unclamped, and
		// the atlas the drawable needs would overflow the instance's worst case.
		CHECK(vs.eff.clamped[1]);
		CHECK(vs.eff.slot[1].view_scale_x == Catch::Approx(0.75f));
		CHECK(vs.eff.slot[1].view_scale_y == Catch::Approx(1.0f));
		CHECK(w.drawable_w * vs.eff.slot[1].view_scale_x * vs.eff.slot[1].tile_columns <= (float)cap_w);
	}

	u_screen_modes_clear();
}
