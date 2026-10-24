// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A bound window with no size must never become a 0x0 atlas.
 *
 * The D3D11, D3D12 and Vulkan compositors sized their swapchain target and
 * atlas at create from the bound window's client rect, and scaled the atlas
 * by min(window / display). A minimized window reports an EMPTY client rect,
 * so it scaled to a 0x0 atlas, the atlas creation failed with E_INVALIDARG,
 * and xrCreateSession failed with it — the app never got a session. Field log,
 * verbatim:
 *
 *   Display pixel info: 3840x2160, base view dims: 1920x2160 per eye
 *   Scaled to window ratio 0.000: 0x0 per eye
 *   [ERROR] [create_resources] Failed to create atlas texture: 0x80070057
 *
 * Both halves of the fix are pure arithmetic, so they are pinned here: the
 * create size falls back to the pre-window default when the window has none,
 * and the scaled view is never 0 on either axis — while every window that DID
 * work keeps exactly the size it had.
 */

#include "catch_amalgamated.hpp"

#include "util/u_tiling.h"

#include <cmath>
#include <string>

namespace {

//! The inline create-path arithmetic the helper replaced, verbatim
//! (D3D11/D3D12/VK: 2x1 tiles; the D3D11 service: the mode's tiles).
void
legacy_scaled_view(uint32_t disp_w,
                   uint32_t disp_h,
                   uint32_t cols,
                   uint32_t rows,
                   uint32_t win_w,
                   uint32_t win_h,
                   uint32_t *out_w,
                   uint32_t *out_h)
{
	uint32_t base_vw = disp_w / cols;
	uint32_t base_vh = disp_h / rows;
	float ratio = fminf((float)win_w / (float)disp_w, (float)win_h / (float)disp_h);
	if (ratio > 1.0f) {
		ratio = 1.0f;
	}
	*out_w = (uint32_t)((float)base_vw * ratio);
	*out_h = (uint32_t)((float)base_vh * ratio);
}

} // namespace

TEST_CASE("create size: a usable client rect is taken as is", "[tiling][window_size]")
{
	uint32_t w = 0, h = 0;
	CHECK(u_tiling_pick_window_size(1920, 1080, false, 3840, 2160, &w, &h) == U_TILING_WINDOW_SIZE_CLIENT);
	CHECK(w == 1920);
	CHECK(h == 1080);

	// A 1x1 window is tiny, not empty: it is the window's size, and the
	// scaled-view floor below is what keeps it creatable.
	CHECK(u_tiling_pick_window_size(1, 1, false, 3840, 2160, &w, &h) == U_TILING_WINDOW_SIZE_CLIENT);
	CHECK(w == 1);
	CHECK(h == 1);
}

TEST_CASE("create size: a minimized window falls back to the pre-window default", "[tiling][window_size]")
{
	uint32_t w = 0, h = 0;
	// GetClientRect on an iconic window returns 0x0.
	CHECK(u_tiling_pick_window_size(0, 0, true, 3840, 2160, &w, &h) == U_TILING_WINDOW_SIZE_ICONIC);
	CHECK(w == 3840);
	CHECK(h == 2160);

	// Minimized wins even over a non-empty rect: its client size is not the
	// size the window will have.
	CHECK(u_tiling_pick_window_size(160, 28, true, 3840, 2160, &w, &h) == U_TILING_WINDOW_SIZE_ICONIC);
	CHECK(w == 3840);
	CHECK(h == 2160);
}

TEST_CASE("create size: an empty client rect falls back to the pre-window default", "[tiling][window_size]")
{
	uint32_t w = 0, h = 0;
	CHECK(u_tiling_pick_window_size(0, 0, false, 3840, 2160, &w, &h) == U_TILING_WINDOW_SIZE_EMPTY);
	CHECK(w == 3840);
	CHECK(h == 2160);

	CHECK(u_tiling_pick_window_size(1920, 0, false, 3840, 2160, &w, &h) == U_TILING_WINDOW_SIZE_EMPTY);
	CHECK(w == 3840);
	CHECK(h == 2160);

	CHECK(u_tiling_pick_window_size(0, 1080, false, 3840, 2160, &w, &h) == U_TILING_WINDOW_SIZE_EMPTY);
	CHECK(w == 3840);
	CHECK(h == 2160);

	// Defensive: right < left never happens for a client rect, but a negative
	// difference must not wrap to a 4-billion-pixel window either.
	CHECK(u_tiling_pick_window_size(-5, 1080, false, 1920, 1080, &w, &h) == U_TILING_WINDOW_SIZE_EMPTY);
	CHECK(w == 1920);
	CHECK(h == 1080);

	CHECK(std::string(u_tiling_window_size_source_str(U_TILING_WINDOW_SIZE_ICONIC)) == "window is minimized");
	CHECK(std::string(u_tiling_window_size_source_str(U_TILING_WINDOW_SIZE_EMPTY)) == "client rect is empty");
}

TEST_CASE("scaled view: the field case no longer produces a 0x0 atlas", "[tiling][window_size]")
{
	// What the field log shows: a 3840x2160 panel, a 0x0 client rect.
	uint32_t old_w = 99, old_h = 99;
	legacy_scaled_view(3840, 2160, 2, 1, 0, 0, &old_w, &old_h);
	CHECK(old_w == 0);
	CHECK(old_h == 0);

	// The create path now never hands the scaler a 0x0 window: the pick falls
	// back to the display, which is ratio 1 — the full per-eye size.
	uint32_t cw = 0, ch = 0;
	REQUIRE(u_tiling_pick_window_size(0, 0, true, 3840, 2160, &cw, &ch) == U_TILING_WINDOW_SIZE_ICONIC);
	uint32_t vw = 0, vh = 0;
	const float ratio = u_tiling_compute_window_scaled_view(3840, 2160, 2, 1, cw, ch, &vw, &vh);
	CHECK(ratio == 1.0f);
	CHECK(vw == 1920);
	CHECK(vh == 2160);

	// And the scaler itself refuses to divide an empty window into nothing.
	CHECK(u_tiling_compute_window_scaled_view(3840, 2160, 2, 1, 0, 0, &vw, &vh) == 1.0f);
	CHECK(vw == 1920);
	CHECK(vh == 2160);
}

TEST_CASE("scaled view: a tiny window is clamped to 1, never 0", "[tiling][window_size]")
{
	struct
	{
		uint32_t w, h;
	} tiny[] = {{1, 1}, {1, 2160}, {3840, 1}, {2, 1}, {1, 1000}};

	for (const auto &t : tiny) {
		uint32_t vw = 0, vh = 0;
		u_tiling_compute_window_scaled_view(3840, 2160, 2, 1, t.w, t.h, &vw, &vh);
		INFO("window " << t.w << "x" << t.h << " -> " << vw << "x" << vh);
		CHECK(vw >= 1);
		CHECK(vh >= 1);
	}

	// The old arithmetic made both 0 for a 1x1 window (ratio 1/3840).
	uint32_t old_w = 99, old_h = 99;
	legacy_scaled_view(3840, 2160, 2, 1, 1, 1, &old_w, &old_h);
	CHECK(old_w == 0);
	CHECK(old_h == 0);
}

TEST_CASE("scaled view: every window that worked keeps exactly its size", "[tiling][window_size]")
{
	struct
	{
		uint32_t disp_w, disp_h, cols, rows;
	} displays[] = {
	    {3840, 2160, 2, 1}, // the field panel, in-process compositors
	    {2560, 1600, 2, 1}, // a 16:10 panel
	    {1920, 1080, 2, 1},
	    {3840, 2160, 2, 2}, // the service's tile layout is the mode's
	    {3840, 2160, 3, 3},
	};

	uint64_t compared = 0;
	for (const auto &d : displays) {
		for (uint32_t w = 1; w <= d.disp_w + 640; w += 37) {
			for (uint32_t h = 1; h <= d.disp_h + 640; h += 41) {
				uint32_t old_w = 0, old_h = 0;
				legacy_scaled_view(d.disp_w, d.disp_h, d.cols, d.rows, w, h, &old_w, &old_h);
				if (old_w == 0 || old_h == 0) {
					continue; // the degenerate inputs this changes on purpose
				}
				uint32_t vw = 0, vh = 0;
				u_tiling_compute_window_scaled_view(d.disp_w, d.disp_h, d.cols, d.rows, w, h, &vw, &vh);
				if (vw != old_w || vh != old_h) {
					FAIL_CHECK("display " << d.disp_w << "x" << d.disp_h << " tiles " << d.cols
					                      << "x" << d.rows << " window " << w << "x" << h << ": "
					                      << vw << "x" << vh << " != legacy " << old_w << "x"
					                      << old_h);
				}
				compared++;
			}
		}
	}
	CHECK(compared > 10000);
}

TEST_CASE("scaled view: never upscales past the display", "[tiling][window_size]")
{
	uint32_t vw = 0, vh = 0;
	CHECK(u_tiling_compute_window_scaled_view(3840, 2160, 2, 1, 7680, 4320, &vw, &vh) == 1.0f);
	CHECK(vw == 1920);
	CHECK(vh == 2160);

	// Half the display on the limiting axis halves the views.
	CHECK(u_tiling_compute_window_scaled_view(3840, 2160, 2, 1, 1920, 2160, &vw, &vh) == 0.5f);
	CHECK(vw == 960);
	CHECK(vh == 1080);
}

TEST_CASE("scaled view: without display info it splits the window, never to 0", "[tiling][window_size]")
{
	uint32_t vw = 0, vh = 0;
	CHECK(u_tiling_compute_window_scaled_view(0, 0, 2, 1, 1920, 1080, &vw, &vh) == 1.0f);
	CHECK(vw == 960);
	CHECK(vh == 1080);

	// The old `preferred.width / 2` made a 1-px-wide window a 0-wide atlas.
	u_tiling_compute_window_scaled_view(0, 0, 2, 1, 1, 1, &vw, &vh);
	CHECK(vw == 1);
	CHECK(vh == 1);

	// Zero tiles are treated as one rather than dividing by zero.
	u_tiling_compute_window_scaled_view(3840, 2160, 0, 0, 3840, 2160, &vw, &vh);
	CHECK(vw == 3840);
	CHECK(vh == 2160);
}

/*
 * DisplayXR/displayxr-unity#418: the atlas ALLOCATION must hold the canvas
 * content in any orientation. NP02J (2560x1600 panel, 2x1 at view scale 0.75):
 * the landscape mode atlas is 3840x1200; rotated to portrait the canvas views
 * are 1200x1920, a 2400x1920 content region taller than that atlas. The
 * vk_native renderer clamped the view to the allocation, and the frame (DP
 * input and xrCaptureAtlasDXR alike) came out 2400x1600.
 */
TEST_CASE("atlas alloc: a portrait canvas grows the landscape mode atlas", "[tiling][window_size]")
{
	uint32_t aw = 0, ah = 0;

	// Portrait on a landscape mode atlas: height grows to the content, width
	// keeps the larger mode atlas (high-water, never shrinks).
	u_tiling_atlas_alloc_for_content(3840, 1200, 2, 1, 1200, 1920, &aw, &ah);
	CHECK(aw == 3840);
	CHECK(ah == 1920);

	// Landscape fullscreen: content == mode atlas, request unchanged.
	u_tiling_atlas_alloc_for_content(3840, 1200, 2, 1, 1920, 1200, &aw, &ah);
	CHECK(aw == 3840);
	CHECK(ah == 1200);

	// A smaller (freeform) window never shrinks the request below the mode atlas.
	u_tiling_atlas_alloc_for_content(3840, 1200, 2, 1, 600, 900, &aw, &ah);
	CHECK(aw == 3840);
	CHECK(ah == 1200);

	// 2x2 quad: both axes scale by the tile grid.
	u_tiling_atlas_alloc_for_content(2560, 1600, 2, 2, 800, 1280, &aw, &ah);
	CHECK(aw == 2560);
	CHECK(ah == 2560);
}
