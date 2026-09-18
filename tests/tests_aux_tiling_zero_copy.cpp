// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-030 + ADR-041: u_tiling_can_zero_copy(), the SOLE zero-copy gate.
 *
 * ADR-041 makes the app's projection view count a fixed property of its view
 * configuration (R) while the mode's tile count (A) keeps changing underneath
 * it, so a conformant app submits R views and aliases the inactive tail
 * [A, R) onto view 0's subimage. The gate therefore has to ask "does this
 * submission COVER the mode's tiling", not "does it equal it" — and the
 * difference is not academic: Windows Leia's worst-case-filling mode is the
 * 1-view 2D one, which a PRIMARY_STEREO app now submits TWO views into. An
 * equality test would have silently retired the one shipping zero-copy case.
 *
 * Pure integer + rect arithmetic in a header, so it is pinned here rather than
 * on a GPU. The DOWNSTREAM half (each backend reading only the first
 * mode->view_count swapchain ids / rects) is not host-drivable and is pinned by
 * the code review in the PR.
 */

#include "catch_amalgamated.hpp"

#include "util/u_tiling.h"

#include <cstring>

namespace {

//! A 2-view side-by-side mode: 2x1 tiles of 960x1080 in a 1920x1080 atlas.
xrt_rendering_mode
sbs_mode()
{
	xrt_rendering_mode m{};
	m.view_count = 2;
	m.tile_columns = 2;
	m.tile_rows = 1;
	m.view_width_pixels = 960;
	m.view_height_pixels = 1080;
	m.atlas_width_pixels = 1920;
	m.atlas_height_pixels = 1080;
	return m;
}

//! The 2D mode: ONE view filling the whole 1920x1080 panel. This is the mode
//! that fills the worst-case envelope on Windows Leia, so it is the one that
//! can legitimately zero-copy — and under ADR-041 a PRIMARY_STEREO app arrives
//! here with TWO submitted views.
xrt_rendering_mode
mono_mode()
{
	xrt_rendering_mode m{};
	m.view_count = 1;
	m.tile_columns = 1;
	m.tile_rows = 1;
	m.view_width_pixels = 1920;
	m.view_height_pixels = 1080;
	m.atlas_width_pixels = 1920;
	m.atlas_height_pixels = 1080;
	return m;
}

} // namespace

TEST_CASE("zero-copy: an exact submission still passes (ADR-030)", "[aux][u_tiling]")
{
	const xrt_rendering_mode m = sbs_mode();
	const int32_t xs[] = {0, 960};
	const int32_t ys[] = {0, 0};
	const uint32_t ws[] = {960, 960};
	const uint32_t hs[] = {1080, 1080};

	CHECK(u_tiling_can_zero_copy(2, xs, ys, ws, hs, 1920, 1080, &m));

	SECTION("a wrong ACTIVE rect still fails — the gate did not go soft")
	{
		const int32_t bad_xs[] = {0, 900};
		CHECK_FALSE(u_tiling_can_zero_copy(2, bad_xs, ys, ws, hs, 1920, 1080, &m));
	}

	SECTION("a swapchain that is not the atlas still fails")
	{
		CHECK_FALSE(u_tiling_can_zero_copy(2, xs, ys, ws, hs, 3840, 1080, &m));
	}
}

TEST_CASE("zero-copy: an ALIASED tail is ignored, not disqualifying (ADR-041)", "[aux][u_tiling]")
{
	// THE Windows Leia 2D case: R = 2 (PRIMARY_STEREO), A = 1 (the mode), and
	// view 1 aliases view 0's subimage.
	const xrt_rendering_mode m = mono_mode();
	const int32_t xs[] = {0, 0};
	const int32_t ys[] = {0, 0};
	const uint32_t ws[] = {1920, 1920};
	const uint32_t hs[] = {1080, 1080};

	SECTION("submitted > mode count, tail aliasing view 0 -> still zero-copy")
	{
		CHECK(u_tiling_can_zero_copy(2, xs, ys, ws, hs, 1920, 1080, &m));
	}

	SECTION("a tail rect that differs from view 0 -> STILL zero-copy, the tail is unread")
	{
		// The runtime discards inactive views' content, so nothing about them
		// can make the passthrough wrong. Anything else would be a per-call-site
		// proxy in disguise.
		const int32_t odd_xs[] = {0, 640};
		const uint32_t odd_ws[] = {1920, 111};
		const uint32_t odd_hs[] = {1080, 7};
		CHECK(u_tiling_can_zero_copy(2, odd_xs, ys, odd_ws, odd_hs, 1920, 1080, &m));
	}

	SECTION("view 0 itself still has to be right")
	{
		const int32_t bad_xs[] = {8, 0};
		CHECK_FALSE(u_tiling_can_zero_copy(2, bad_xs, ys, ws, hs, 1920, 1080, &m));
	}

	SECTION("a four-view submission over a 1-view mode is fine too")
	{
		const int32_t xs4[] = {0, 0, 0, 0};
		const int32_t ys4[] = {0, 0, 0, 0};
		const uint32_t ws4[] = {1920, 1920, 1920, 1920};
		const uint32_t hs4[] = {1080, 1080, 1080, 1080};
		CHECK(u_tiling_can_zero_copy(4, xs4, ys4, ws4, hs4, 1920, 1080, &m));
	}
}

TEST_CASE("zero-copy: a submission that does NOT cover the mode is refused (ADR-041)", "[aux][u_tiling]")
{
	// R < A: PRIMARY_STEREO (2 views) while the panel is in a 2x2 quad mode.
	// There is no atlas to hand over, and reading 4 tiles out of a 2-view
	// submission would pick up slots the app never wrote.
	xrt_rendering_mode quad{};
	quad.view_count = 4;
	quad.tile_columns = 2;
	quad.tile_rows = 2;
	quad.view_width_pixels = 960;
	quad.view_height_pixels = 540;
	quad.atlas_width_pixels = 1920;
	quad.atlas_height_pixels = 1080;

	const int32_t xs[] = {0, 960};
	const int32_t ys[] = {0, 0};
	const uint32_t ws[] = {960, 960};
	const uint32_t hs[] = {540, 540};

	CHECK_FALSE(u_tiling_can_zero_copy(2, xs, ys, ws, hs, 1920, 1080, &quad));
}
