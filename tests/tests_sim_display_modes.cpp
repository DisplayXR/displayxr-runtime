// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's rendering modes and the worst-case atlas they give.
 *
 * Every app's swapchain is sized to the worst case across the registered
 * modes (ADR-010, u_tiling_compute_system_atlas_oriented), and the 2D mode must
 * fill that worst case for u_tiling_can_zero_copy() to fire. Full side by side
 * (mode 6, two views at scale 1.0 x 1.0, 2x1 tiles) is twice the panel's width:
 * registered by default it made every app's swapchain twice as wide on every
 * platform. Pins that sim_display's DEFAULT list is the six modes it had before
 * (same names, scales and tiles) with a worst-case atlas of exactly W x H, and
 * that mode 6 appears only when asked for and the platform can take it.
 */

#include "catch_amalgamated.hpp"

#include "sim_display_modes.h"

#include "util/u_tiling.h"

#include <cstring>

namespace {

constexpr uint32_t kW = 1920;
constexpr uint32_t kH = 1080;

struct Row
{
	const char *name;
	uint32_t views;
	float sx, sy;
	bool hw3d;
	uint32_t cols, rows;
};

// The modes sim_display registered before full side by side existed.
constexpr Row kBase[6] = {
    {"2D", 1, 1.0f, 1.0f, false, 1, 1},         {"Anaglyph", 2, 0.5f, 0.5f, true, 2, 1},
    {"Cropped SBS", 2, 0.5f, 0.5f, true, 1, 2}, {"Squeezed SBS", 2, 0.5f, 1.0f, true, 2, 1},
    {"Quad", 4, 0.5f, 0.5f, true, 2, 2},        {"Interlaced", 2, 0.5f, 0.5f, true, 2, 1},
};

void
worst_case(const xrt_rendering_mode *m, uint32_t n, uint32_t *w, uint32_t *h)
{
	u_tiling_compute_system_atlas_oriented(m, n, kW, kH, w, h);
}

} // namespace

TEST_CASE("sim_display default modes: six, and the worst-case atlas is the panel", "[sim_display][u_tiling]")
{
	xrt_rendering_mode modes[XRT_MAX_RENDERING_MODES] = {};
	const uint32_t n = sim_display_fill_rendering_modes(modes, false);

	CHECK(n == 6);
	for (uint32_t i = 0; i < 6; i++) {
		CAPTURE(i);
		CHECK(modes[i].mode_index == i);
		CHECK(std::strcmp(modes[i].mode_name, kBase[i].name) == 0);
		CHECK(modes[i].view_count == kBase[i].views);
		CHECK(modes[i].view_scale_x == kBase[i].sx);
		CHECK(modes[i].view_scale_y == kBase[i].sy);
		CHECK(modes[i].hardware_display_3d == kBase[i].hw3d);
		CHECK(modes[i].tile_columns == kBase[i].cols);
		CHECK(modes[i].tile_rows == kBase[i].rows);
	}

	uint32_t w = 0, h = 0;
	worst_case(modes, n, &w, &h);
	CHECK(w == kW);
	CHECK(h == kH);
}

TEST_CASE("sim_display full side by side is asked for, and only where a stereo compositor takes it",
          "[sim_display][u_tiling]")
{
	xrt_rendering_mode modes[XRT_MAX_RENDERING_MODES] = {};
	const uint32_t n = sim_display_fill_rendering_modes(modes, true);
	uint32_t w = 0, h = 0;
	worst_case(modes, n, &w, &h);

	if (sim_display_full_sbs_available()) {
		REQUIRE(n == 7);
		CHECK(std::strcmp(modes[6].mode_name, "Full SBS") == 0);
		CHECK(modes[6].view_scale_x == 1.0f);
		CHECK(modes[6].tile_columns == 2);
		// The cost the default list must not pay: twice the panel's width.
		CHECK(w == 2 * kW);
		CHECK(h == kH);
	} else {
		CHECK(n == 6);
		CHECK(w == kW);
		CHECK(h == kH);
	}
}
