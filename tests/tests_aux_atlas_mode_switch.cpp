// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The in-process D3D11 renderer atlas across a 2D <-> 3D switch.
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
 * The decision the renderer now takes on every tile-layout change is header
 * arithmetic, so it is pinned here on the host: does the allocation still hold
 * the content, and how far does the grow go.
 */

#include "catch_amalgamated.hpp"

#include "util/u_tiling.h"

namespace {

//! The renderer's allocation after a tile-layout change, as
//! comp_d3d11_renderer_set_tile_layout + comp_d3d11_renderer_resize decide it
//! for a renderer with a FIXED view size (a legacy session).
struct alloc
{
	uint32_t w, h;
};

alloc
legacy_alloc_after_tiles(alloc cur, uint32_t cols, uint32_t rows, uint32_t vw, uint32_t vh, alloc cap)
{
	if (u_tiling_atlas_alloc_holds(cols, rows, vw, vh, cur.w, cur.h)) {
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
