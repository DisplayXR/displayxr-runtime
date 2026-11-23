// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The in-process renderer atlas across a 2D <-> 3D switch.
 *
 * Field log (hosted D3D11 sessions on a hybrid-GPU laptop, runtime v2.70.0):
 *
 *  1. A LEGACY session on a display that starts in 2D (sim_display since
 *     ADR-065) is allocated for ONE compromise-scale view (1920x2160, tiles
 *     1x1). V moves the mode to 2x1; the legacy renderer keeps its view size
 *     and only takes the new grid, so the content becomes 3840x2160 inside a
 *     1920x2160 allocation, and the cross-adapter bridge refuses every frame:
 *     "the copy box is 3840x2160 but the source is 1920x2160".
 *
 * The decision the renderers take on every tile-layout change is
 * u_tiling_tile_layout_needs_atlas_grow — the very function
 * comp_d3d11_renderer_set_tile_layout, comp_d3d12_renderer_set_tile_layout and
 * the vk_native per-frame mode sync call — so it is pinned here on the host,
 * together with how far the grow then goes (u_tiling_grow_atlas_extent, the
 * D3D11 / D3D12 renderers' allocation rule).
 */

#include "catch_amalgamated.hpp"

#include "util/u_tiling.h"

namespace {

//! The renderer's allocation after a tile-layout change for a renderer with a
//! FIXED view size (a legacy session): the shipped decision, then the
//! renderer's own grow rule when it says so.
struct alloc
{
	uint32_t w, h;
};

alloc
legacy_alloc_after_tiles(alloc cur, uint32_t cols, uint32_t rows, uint32_t vw, uint32_t vh, alloc cap)
{
	if (!u_tiling_tile_layout_needs_atlas_grow(true, true, cols, rows, vw, vh, cur.w, cur.h)) {
		return cur;
	}
	return {u_tiling_grow_atlas_extent(cur.w, cols * vw, cap.w), u_tiling_grow_atlas_extent(cur.h, rows * vh, cap.h)};
}

} // namespace

TEST_CASE("holds: content inside the allocation fits, outside does not", "[tiling][atlas_mode_switch]")
{
	CHECK(u_tiling_atlas_alloc_holds(1, 1, 1920, 2160, 1920, 2160));
	CHECK(u_tiling_atlas_alloc_holds(2, 1, 1920, 2160, 3840, 2160));
	CHECK(u_tiling_atlas_alloc_holds(1, 1, 1920, 2160, 3840, 2160)); // 3D -> 2D shrinks the content
	CHECK_FALSE(u_tiling_atlas_alloc_holds(2, 1, 1920, 2160, 1920, 2160));
	CHECK_FALSE(u_tiling_atlas_alloc_holds(1, 2, 1920, 2160, 3840, 2160));
}

TEST_CASE("holds: an empty allocation holds nothing", "[tiling][atlas_mode_switch]")
{
	CHECK_FALSE(u_tiling_atlas_alloc_holds(1, 1, 64, 64, 0, 0));
	CHECK_FALSE(u_tiling_atlas_alloc_holds(1, 1, 64, 64, 64, 0));
}

TEST_CASE("holds: no overflow on large grids", "[tiling][atlas_mode_switch]")
{
	CHECK_FALSE(u_tiling_atlas_alloc_holds(0x10000u, 1, 0x10000u, 1, 0xFFFFFFFFu, 1));
}

TEST_CASE("grow decision: only a fixed-view-size renderer that owns its atlas, only when it no longer holds",
          "[tiling][atlas_mode_switch][legacy]")
{
	// The field case: 1x1 -> 2x1 at a fixed 1920x2160 view in a 1920x2160 allocation.
	CHECK(u_tiling_tile_layout_needs_atlas_grow(true, true, 2, 1, 1920, 2160, 1920, 2160));
	// An extension renderer resizes through its per-frame view sync instead.
	CHECK_FALSE(u_tiling_tile_layout_needs_atlas_grow(false, true, 2, 1, 1920, 2160, 1920, 2160));
	// No atlas yet, or one borrowed from a deposit ring the compositor sizes.
	CHECK_FALSE(u_tiling_tile_layout_needs_atlas_grow(true, false, 2, 1, 1920, 2160, 1920, 2160));
	// Still holds (3D -> 2D, or a session that started in 3D).
	CHECK_FALSE(u_tiling_tile_layout_needs_atlas_grow(true, true, 1, 1, 1920, 2160, 3840, 2160));
	CHECK_FALSE(u_tiling_tile_layout_needs_atlas_grow(true, true, 2, 1, 1920, 2160, 3840, 2160));
	// A 2-row kind outgrows the height alone.
	CHECK(u_tiling_tile_layout_needs_atlas_grow(true, true, 2, 2, 1920, 1080, 3840, 1080));
	// A degenerate grid is never a reason to reallocate.
	CHECK_FALSE(u_tiling_tile_layout_needs_atlas_grow(true, true, 0, 1, 1920, 2160, 1920, 2160));
	CHECK_FALSE(u_tiling_tile_layout_needs_atlas_grow(true, true, 2, 0, 1920, 2160, 1920, 2160));
}

TEST_CASE("legacy session started in 2D: V to the 2x1 mode grows the atlas to the 3D size",
          "[tiling][atlas_mode_switch][legacy]")
{
	// The field case: compromise scale 0.5x1.0 of a 3840x2160 panel = 1920x2160
	// per view; worst-case atlas 3840x2160 (the 2D mode).
	const alloc cap{3840, 2160};
	const uint32_t vw = 1920, vh = 2160;

	// Created in the 1x1 2D mode: one view wide.
	alloc a{1 * vw, vh};
	REQUIRE(a.w == 1920);

	// V: 2D -> 2x1. Before the fix the allocation stayed 1920 wide.
	REQUIRE_FALSE(u_tiling_atlas_alloc_holds(2, 1, vw, vh, a.w, a.h));
	a = legacy_alloc_after_tiles(a, 2, 1, vw, vh, cap);
	CHECK(a.w == 3840);
	CHECK(a.h == 2160);
	// ...exactly what a session that STARTED in the 2x1 mode is allocated.
	CHECK(a.w == 2 * vw);
	CHECK(a.w <= cap.w);
	CHECK(a.h <= cap.h);

	// V x3 more: back to 1x1, 2x1, 1x1 — never another grow.
	for (int i = 0; i < 3; i++) {
		const uint32_t cols = (i % 2 == 0) ? 1 : 2;
		alloc next = legacy_alloc_after_tiles(a, cols, 1, vw, vh, cap);
		CHECK(next.w == a.w);
		CHECK(next.h == a.h);
	}
}

TEST_CASE("legacy session started in 3D: the toggle never reallocates", "[tiling][atlas_mode_switch][legacy]")
{
	const alloc cap{3840, 2160};
	alloc a{2 * 1920, 2160};
	for (int i = 0; i < 4; i++) {
		const uint32_t cols = (i % 2 == 0) ? 1 : 2;
		alloc next = legacy_alloc_after_tiles(a, cols, 1, 1920, 2160, cap);
		CHECK(next.w == a.w);
		CHECK(next.h == a.h);
	}
}

TEST_CASE("legacy session: a 2-row kind grows the height, not past the cap", "[tiling][atlas_mode_switch][legacy]")
{
	// A 2x2 quad kind on a 3840x2160 panel: cap is still the 2D mode's extent.
	const alloc cap{3840, 2160};
	alloc a{1920, 1080}; // started 1x1 at a 0.5x0.5 compromise
	a = legacy_alloc_after_tiles(a, 2, 2, 1920, 1080, cap);
	CHECK(a.w == 3840);
	CHECK(a.h == 2160);
}

/*
 * 2. An EXTENSION hosted session on a display with {2D 1x1 at 1.0, 3D 2x1 at
 *    0.5x0.5} (3840x2160 panel). The V path ran between the compositor's
 *    tile-layout sync and its view-size sync, so for one frame mode 0's view
 *    size (3840x2160) met mode 1's 2x1 grid: "Renderer resized: atlas alloc
 *    3840x2160 -> 7680x2160 ... cap 3840x2160". The compositor now reads the
 *    active mode once per frame (not reachable without a GPU); what IS pinned
 *    here is the arithmetic both halves rest on: one mode's view size on the
 *    same mode's grid is always inside the worst-case cap, and a mixed pair is
 *    clamped back inside it instead of growing the atlas.
 */

namespace {

xrt_rendering_mode
mode(uint32_t views, uint32_t cols, uint32_t rows, uint32_t vw, uint32_t vh)
{
	xrt_rendering_mode m{};
	m.view_count = views;
	m.tile_columns = cols;
	m.tile_rows = rows;
	m.view_width_pixels = vw;
	m.view_height_pixels = vh;
	m.atlas_width_pixels = cols * vw;
	m.atlas_height_pixels = rows * vh;
	return m;
}

} // namespace

TEST_CASE("cap: one mode's view on its own grid never exceeds the worst case", "[tiling][atlas_mode_switch][cap]")
{
	const xrt_rendering_mode modes[2] = {mode(1, 1, 1, 3840, 2160), mode(2, 2, 1, 1920, 1080)};
	uint32_t cap_w = 0, cap_h = 0;
	u_tiling_compute_system_atlas(modes, 2, &cap_w, &cap_h);
	REQUIRE(cap_w == 3840);
	REQUIRE(cap_h == 2160);

	for (const xrt_rendering_mode &m : modes) {
		uint32_t vw = m.view_width_pixels, vh = m.view_height_pixels;
		CHECK_FALSE(u_tiling_clamp_view_to_atlas_cap(m.tile_columns, cap_w, &vw));
		CHECK_FALSE(u_tiling_clamp_view_to_atlas_cap(m.tile_rows, cap_h, &vh));
		CHECK(vw == m.view_width_pixels);
		CHECK(vh == m.view_height_pixels);
		CHECK(m.tile_columns * vw <= cap_w);
		CHECK(m.tile_rows * vh <= cap_h);
	}
}

TEST_CASE("cap: the torn V frame (mode 0 view, mode 1 grid) is clamped, not grown",
          "[tiling][atlas_mode_switch][cap]")
{
	const uint32_t cap_w = 3840, cap_h = 2160;
	uint32_t vw = 3840, vh = 2160; // mode 0's view
	const uint32_t cols = 2, rows = 1; // mode 1's grid
	REQUIRE(cols * vw == 7680);        // the field log's content
	CHECK(u_tiling_clamp_view_to_atlas_cap(cols, cap_w, &vw));
	CHECK_FALSE(u_tiling_clamp_view_to_atlas_cap(rows, cap_h, &vh));
	CHECK(vw == 1920);
	CHECK(cols * vw <= cap_w);
	// ...and an allocation already at the cap then holds it: no realloc.
	CHECK(u_tiling_atlas_alloc_holds(cols, rows, vw, vh, cap_w, cap_h));
}

TEST_CASE("cap: unknown cap or grid leaves the view alone; never 0", "[tiling][atlas_mode_switch][cap]")
{
	uint32_t v = 5000;
	CHECK_FALSE(u_tiling_clamp_view_to_atlas_cap(2, 0, &v));
	CHECK(v == 5000);
	CHECK_FALSE(u_tiling_clamp_view_to_atlas_cap(0, 3840, &v));
	CHECK(v == 5000);
	CHECK_FALSE(u_tiling_clamp_view_to_atlas_cap(2, 3840, nullptr));
	v = 100;
	CHECK(u_tiling_clamp_view_to_atlas_cap(8, 4, &v));
	CHECK(v == 1);
}

/*
 * 3. The cap the clamp above uses must follow the table. A display-info
 *    refresh (hotplug, a larger panel) recomputes the per-mode view sizes; a
 *    cap left at its create-time value would clamp a legitimately larger mode
 *    and silently reduce the session's content. u_tiling_follow_atlas_cap is
 *    what the D3D11 and D3D12 compositors call at the frame's single read.
 */
TEST_CASE("cap follow: a refreshed, larger table moves the cap and the larger mode is not clamped",
          "[tiling][atlas_mode_switch][cap]")
{
	// Created against a 3840x2160 panel.
	xrt_rendering_mode modes[2] = {mode(1, 1, 1, 3840, 2160), mode(2, 2, 1, 1920, 1080)};
	uint32_t cap_w = 0, cap_h = 0;
	REQUIRE(u_tiling_follow_atlas_cap(modes, 2, &cap_w, &cap_h));
	REQUIRE(cap_w == 3840);
	REQUIRE(cap_h == 2160);
	// Same table: nothing to do.
	CHECK_FALSE(u_tiling_follow_atlas_cap(modes, 2, &cap_w, &cap_h));

	// Refreshed for a 7680x4320 panel.
	modes[0] = mode(1, 1, 1, 7680, 4320);
	modes[1] = mode(2, 2, 1, 3840, 2160);

	// Against the stale cap the new 2D mode would be halved...
	uint32_t stale_vw = 7680;
	CHECK(u_tiling_clamp_view_to_atlas_cap(1, cap_w, &stale_vw));
	CHECK(stale_vw == 3840);

	// ...after following the table it is left alone, on both modes.
	CHECK(u_tiling_follow_atlas_cap(modes, 2, &cap_w, &cap_h));
	CHECK(cap_w == 7680);
	CHECK(cap_h == 4320);
	for (const xrt_rendering_mode &m : modes) {
		uint32_t vw = m.view_width_pixels, vh = m.view_height_pixels;
		CHECK_FALSE(u_tiling_clamp_view_to_atlas_cap(m.tile_columns, cap_w, &vw));
		CHECK_FALSE(u_tiling_clamp_view_to_atlas_cap(m.tile_rows, cap_h, &vh));
	}
	// The impossible mixed pair is still stopped against the new cap.
	uint32_t torn = 7680; // the 2D view on the 2x1 grid
	CHECK(u_tiling_clamp_view_to_atlas_cap(2, cap_w, &torn));
	CHECK(torn == 3840);
}

TEST_CASE("cap follow: a smaller table follows down; an empty table keeps the held cap",
          "[tiling][atlas_mode_switch][cap]")
{
	xrt_rendering_mode modes[2] = {mode(1, 1, 1, 1920, 1080), mode(2, 2, 1, 960, 540)};
	uint32_t cap_w = 3840, cap_h = 2160;
	CHECK(u_tiling_follow_atlas_cap(modes, 2, &cap_w, &cap_h));
	CHECK(cap_w == 1920);
	CHECK(cap_h == 1080);
	// ...and each mode's own view on its own grid still fits it exactly.
	for (const xrt_rendering_mode &m : modes) {
		CHECK(m.tile_columns * m.view_width_pixels <= cap_w);
		CHECK(m.tile_rows * m.view_height_pixels <= cap_h);
	}

	xrt_rendering_mode empty[1] = {};
	CHECK_FALSE(u_tiling_follow_atlas_cap(empty, 1, &cap_w, &cap_h));
	CHECK_FALSE(u_tiling_follow_atlas_cap(nullptr, 0, &cap_w, &cap_h));
	CHECK(cap_w == 1920);
	CHECK(cap_h == 1080);
}
