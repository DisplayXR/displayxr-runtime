// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1625: view 0 is the TOP-LEFT tile AS DISPLAYED, on every backend.
 *
 * The atlas tile row order was unspecified: `multiview-tiling.md` and
 * `u_tiling.h` both gave `Y = (view_index / tile_columns) * view_height`
 * without naming which end of the texture `y = 0` is. Read as texel offsets in
 * each API's own addressing, all five backends complied — and D3D11, D3D12,
 * Metal and Vulkan put view 0 in the geometrically top-left tile while OpenGL
 * put it bottom-left, because `glViewport`'s Y origin is the bottom and every
 * backend computed the same `tile_y * tile_h`.
 *
 * This is the cross-backend assertion the issue asked for, and it is a unit
 * test rather than a live capture because the whole disagreement lives in two
 * lines of integer arithmetic. Each backend family's tile rect is re-derived
 * here in *displayed* coordinates (row 0 = the top of the upright image), and
 * the test asserts every family lands the same view index in the same physical
 * band of the atlas.
 *
 * What it cannot pin: that each compositor actually calls the right helper.
 * Four of the five still recompute `i % cols` / `i / cols` inline, which is
 * how the GL copy diverged in the first place; those call sites are pinned by
 * code review and by the Metal-vs-GL atlas captures in the PR.
 */

#include "catch_amalgamated.hpp"

#include "util/u_tiling.h"

namespace {

//! A tile rect in DISPLAYED coordinates: row 0 is the top of the upright image.
struct displayed_tile
{
	uint32_t x;
	uint32_t y_top;
	uint32_t w;
	uint32_t h;

	bool
	operator==(const displayed_tile &o) const
	{
		return x == o.x && y_top == o.y_top && w == o.w && h == o.h;
	}
};

/*!
 * D3D11 / D3D12 / Metal / Vulkan: texel row 0 is the TOP, so the helper's Y is
 * already a displayed Y. Mirrors `D3D11_VIEWPORT::TopLeftY`,
 * `D3D12_VIEWPORT::TopLeftY`, `MTLViewport.originY` and `VkViewport.y`.
 */
displayed_tile
tile_top_down(uint32_t view_index, uint32_t cols, uint32_t view_w, uint32_t view_h)
{
	uint32_t x = 0, y = 0;
	u_tiling_view_origin(view_index, cols, view_w, view_h, &x, &y);
	return displayed_tile{x, y, view_w, view_h};
}

/*!
 * OpenGL: the helper returns the `glViewport` Y of the tile's LOWER edge in a
 * `rows * view_h` tall framebuffer whose origin is the bottom. Convert to a
 * displayed Y the same way the atlas capture does (`glReadPixels` + row
 * reversal).
 */
displayed_tile
tile_gl(uint32_t view_index, uint32_t cols, uint32_t rows, uint32_t view_w, uint32_t view_h)
{
	uint32_t x = 0, y_bottom = 0;
	u_tiling_view_origin_gl(view_index, cols, rows, view_w, view_h, &x, &y_bottom);

	const uint32_t atlas_h = rows * view_h;
	REQUIRE(y_bottom + view_h <= atlas_h);
	return displayed_tile{x, atlas_h - (y_bottom + view_h), view_w, view_h};
}

//! Every layout the tree can produce, plus a 3-row case nothing ships yet.
struct layout
{
	const char *name;
	uint32_t views;
	uint32_t cols;
	uint32_t rows;
};

constexpr layout k_layouts[] = {
    {"2D 1x1", 1, 1, 1},          //
    {"SBS 2x1", 2, 2, 1},         //
    {"Cropped SBS 1x2", 2, 1, 2}, //
    {"Quad 2x2", 4, 2, 2},        //
    {"3-row 2x3", 6, 2, 3},       //
};

} // namespace

TEST_CASE("tile order: the same view index lands in the same physical tile on every backend (#1625)", "[aux][u_tiling]")
{
	constexpr uint32_t view_w = 960;
	constexpr uint32_t view_h = 540;

	for (const layout &l : k_layouts) {
		INFO("layout " << l.name);
		for (uint32_t i = 0; i < l.views; i++) {
			INFO("view " << i);
			CHECK(tile_gl(i, l.cols, l.rows, view_w, view_h) == tile_top_down(i, l.cols, view_w, view_h));
		}
	}
}

TEST_CASE("tile order: view 0 is the top-left tile as displayed (#1625)", "[aux][u_tiling]")
{
	constexpr uint32_t view_w = 960;
	constexpr uint32_t view_h = 540;

	for (const layout &l : k_layouts) {
		INFO("layout " << l.name);

		const displayed_tile td = tile_top_down(0, l.cols, view_w, view_h);
		const displayed_tile gl = tile_gl(0, l.cols, l.rows, view_w, view_h);

		CHECK(td.x == 0u);
		CHECK(td.y_top == 0u);
		CHECK(gl.x == 0u);
		CHECK(gl.y_top == 0u);
	}
}

TEST_CASE("tile order: the GL helper is IDENTITY for every single-row layout (#1625)", "[aux][u_tiling]")
{
	// Every mode any shipped vendor plug-in publishes is tile_rows == 1
	// (leia_device.c publishes 1x1 and 2x1; the Android CNSDK DP rejects
	// anything but 2x1). So the paired GL change is a no-op for all of them,
	// and the blast radius is sim_display's Quad and Cropped SBS alone.
	constexpr uint32_t view_w = 800;
	constexpr uint32_t view_h = 600;

	for (uint32_t cols = 1; cols <= 4; cols++) {
		for (uint32_t i = 0; i < cols; i++) {
			uint32_t gx = 0, gy = 0, tx = 0, ty = 0;
			u_tiling_view_origin_gl(i, cols, 1, view_w, view_h, &gx, &gy);
			u_tiling_view_origin(i, cols, view_w, view_h, &tx, &ty);
			CHECK(gx == tx);
			CHECK(gy == ty);
			CHECK(gy == 0u);
		}
	}
}

TEST_CASE("tile order: a GL display processor's v-range matches its tile's displayed band (#1625)", "[aux][u_tiling]")
{
	// The DP-side half of the paired rule, exactly as the header states it:
	// tile row r occupies v in [1 - (r+1)/rows, 1 - r/rows], i.e. a GL shader
	// samples view i at v = (local_v + (rows - 1 - r)) / rows. This is what
	// `dxr_atlas_row()` in sim_display_processor_gl.c computes; if the
	// compositor half moves without it, these two stop agreeing.
	constexpr uint32_t view_w = 640;
	constexpr uint32_t view_h = 480;

	for (const layout &l : k_layouts) {
		INFO("layout " << l.name);
		const float rows_f = static_cast<float>(l.rows);
		const uint32_t atlas_h = l.rows * view_h;

		for (uint32_t i = 0; i < l.views; i++) {
			INFO("view " << i);
			const uint32_t r = i / l.cols;

			// What the spec/header promise the DP.
			const float v_lo = 1.0f - static_cast<float>(r + 1) / rows_f;
			const float v_hi = 1.0f - static_cast<float>(r) / rows_f;

			// The same band, taken from the shader's row index.
			const float shader_row = rows_f - 1.0f - static_cast<float>(r);
			CHECK(shader_row / rows_f == Catch::Approx(v_lo));
			CHECK((1.0f + shader_row) / rows_f == Catch::Approx(v_hi));

			// And the same band again, taken from where the compositor
			// put the tile: GL v is measured from the bottom, so the
			// displayed top edge is at 1 - v_hi of the atlas height.
			const displayed_tile t = tile_gl(i, l.cols, l.rows, view_w, view_h);
			CHECK(t.y_top == static_cast<uint32_t>((1.0f - v_hi) * static_cast<float>(atlas_h) + 0.5f));
			CHECK(t.y_top + t.h ==
			      static_cast<uint32_t>((1.0f - v_lo) * static_cast<float>(atlas_h) + 0.5f));
		}
	}
}
