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

	CHECK(u_tiling_can_zero_copy(2, xs, ys, ws, hs, 1920, 1080, &m, U_TILING_ORIGIN_TOP_LEFT));

	SECTION("a wrong ACTIVE rect still fails — the gate did not go soft")
	{
		const int32_t bad_xs[] = {0, 900};
		CHECK_FALSE(u_tiling_can_zero_copy(2, bad_xs, ys, ws, hs, 1920, 1080, &m, U_TILING_ORIGIN_TOP_LEFT));
	}

	SECTION("a swapchain that is not the atlas still fails")
	{
		CHECK_FALSE(u_tiling_can_zero_copy(2, xs, ys, ws, hs, 3840, 1080, &m, U_TILING_ORIGIN_TOP_LEFT));
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
		CHECK(u_tiling_can_zero_copy(2, xs, ys, ws, hs, 1920, 1080, &m, U_TILING_ORIGIN_TOP_LEFT));
	}

	SECTION("a tail rect that differs from view 0 -> STILL zero-copy, the tail is unread")
	{
		// The runtime discards inactive views' content, so nothing about them
		// can make the passthrough wrong. Anything else would be a per-call-site
		// proxy in disguise.
		const int32_t odd_xs[] = {0, 640};
		const uint32_t odd_ws[] = {1920, 111};
		const uint32_t odd_hs[] = {1080, 7};
		CHECK(u_tiling_can_zero_copy(2, odd_xs, ys, odd_ws, odd_hs, 1920, 1080, &m, U_TILING_ORIGIN_TOP_LEFT));
	}

	SECTION("view 0 itself still has to be right")
	{
		const int32_t bad_xs[] = {8, 0};
		CHECK_FALSE(u_tiling_can_zero_copy(2, bad_xs, ys, ws, hs, 1920, 1080, &m, U_TILING_ORIGIN_TOP_LEFT));
	}

	SECTION("a four-view submission over a 1-view mode is fine too")
	{
		const int32_t xs4[] = {0, 0, 0, 0};
		const int32_t ys4[] = {0, 0, 0, 0};
		const uint32_t ws4[] = {1920, 1920, 1920, 1920};
		const uint32_t hs4[] = {1080, 1080, 1080, 1080};
		CHECK(u_tiling_can_zero_copy(4, xs4, ys4, ws4, hs4, 1920, 1080, &m, U_TILING_ORIGIN_TOP_LEFT));
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

	CHECK_FALSE(u_tiling_can_zero_copy(2, xs, ys, ws, hs, 1920, 1080, &quad, U_TILING_ORIGIN_TOP_LEFT));
}

/*
 * #1628 — the SUBMISSION side of #1625's "view 0 is the top-left tile as
 * displayed".
 *
 * The app's `subImage.imageRect.offset.y` is a bare number it computes with the
 * same expression it hands its own viewport call, so its meaning follows the
 * app's framebuffer origin. A GL app's `glViewport` Y counts UP from the bottom;
 * every other backend's counts DOWN from the top. Read in the wrong origin the
 * two agree NUMERICALLY on a symmetric tile grid while naming OPPOSITE rows —
 * and a zero-copy frame has no crop to re-place the content, so the display
 * processor is handed a vertically mirrored tile grid in silence.
 *
 * These cases are what make the origin argument load-bearing rather than
 * decorative: delete the BOTTOM_LEFT branch in u_tiling_view_matches_tile (or
 * make it call u_tiling_view_origin) and every `rows > 1` assertion below flips.
 */

namespace {

//! Cropped SBS: 1 column x 2 ROWS. The cheapest layout with a top/bottom to
//! get wrong, and one sim_display has shipped for a long time.
xrt_rendering_mode
stacked_mode()
{
	xrt_rendering_mode m{};
	m.view_count = 2;
	m.tile_columns = 1;
	m.tile_rows = 2;
	m.view_width_pixels = 960;
	m.view_height_pixels = 540;
	m.atlas_width_pixels = 960;
	m.atlas_height_pixels = 1080;
	return m;
}

//! 2x2 quad — two rows AND two columns, so it also pins that X never flips.
xrt_rendering_mode
quad_mode()
{
	xrt_rendering_mode m{};
	m.view_count = 4;
	m.tile_columns = 2;
	m.tile_rows = 2;
	m.view_width_pixels = 960;
	m.view_height_pixels = 540;
	m.atlas_width_pixels = 1920;
	m.atlas_height_pixels = 1080;
	return m;
}

} // namespace

TEST_CASE("zero-copy origin: a two-row submission is read in the CALLER's Y origin (#1628)",
          "[aux][u_tiling]")
{
	const xrt_rendering_mode m = stacked_mode();

	// What a D3D/Metal/Vulkan app writes: view 0 at the top, offset.y counting
	// DOWN, so view 0 -> 0 and view 1 -> 540.
	const int32_t xs[] = {0, 0};
	const int32_t ys_top[] = {0, 540};
	// What an OpenGL app writes for the SAME physical arrangement: view 0 is
	// still the top tile as displayed, but glViewport's Y counts UP, so view 0
	// is at 540 and view 1 at 0. Note this is the top-origin array REVERSED —
	// the numbers present are identical, only their assignment differs, which
	// is precisely why an origin-blind comparison could not tell them apart.
	const int32_t ys_bottom[] = {540, 0};
	const uint32_t ws[] = {960, 960};
	const uint32_t hs[] = {540, 540};

	SECTION("each origin accepts its own convention")
	{
		CHECK(u_tiling_can_zero_copy(2, xs, ys_top, ws, hs, 960, 1080, &m,
		                             U_TILING_ORIGIN_TOP_LEFT));
		CHECK(u_tiling_can_zero_copy(2, xs, ys_bottom, ws, hs, 960, 1080, &m,
		                             U_TILING_ORIGIN_BOTTOM_LEFT));
	}

	SECTION("and REJECTS the other one — this is the bug #1628 reported")
	{
		// Before the fix both of these passed, because both offset arrays are
		// the set {0, 540} and the predicate only ever compared against the
		// top-origin expectation.
		CHECK_FALSE(u_tiling_can_zero_copy(2, xs, ys_bottom, ws, hs, 960, 1080, &m,
		                                   U_TILING_ORIGIN_TOP_LEFT));
		CHECK_FALSE(u_tiling_can_zero_copy(2, xs, ys_top, ws, hs, 960, 1080, &m,
		                                   U_TILING_ORIGIN_BOTTOM_LEFT));
	}

	SECTION("per-view: bottom-origin view 0 is the LAST row, not the first")
	{
		CHECK(u_tiling_view_matches_tile(0, 0, 540, 960, 540, &m, U_TILING_ORIGIN_BOTTOM_LEFT));
		CHECK_FALSE(u_tiling_view_matches_tile(0, 0, 0, 960, 540, &m, U_TILING_ORIGIN_BOTTOM_LEFT));
		CHECK(u_tiling_view_matches_tile(0, 0, 0, 960, 540, &m, U_TILING_ORIGIN_TOP_LEFT));
		CHECK_FALSE(u_tiling_view_matches_tile(0, 0, 540, 960, 540, &m, U_TILING_ORIGIN_TOP_LEFT));
	}
}

TEST_CASE("zero-copy origin: 2x2 flips rows only, never columns (#1628)", "[aux][u_tiling]")
{
	const xrt_rendering_mode q = quad_mode();
	const uint32_t ws[] = {960, 960, 960, 960};
	const uint32_t hs[] = {540, 540, 540, 540};

	// Views 0,1 are the top row; 2,3 the bottom row. X is identical in both
	// origins — no graphics API flips X — so only the Y column below changes.
	const int32_t xs[] = {0, 960, 0, 960};
	const int32_t ys_top[] = {0, 0, 540, 540};
	const int32_t ys_bottom[] = {540, 540, 0, 0};

	CHECK(u_tiling_can_zero_copy(4, xs, ys_top, ws, hs, 1920, 1080, &q, U_TILING_ORIGIN_TOP_LEFT));
	CHECK(u_tiling_can_zero_copy(4, xs, ys_bottom, ws, hs, 1920, 1080, &q,
	                             U_TILING_ORIGIN_BOTTOM_LEFT));
	CHECK_FALSE(u_tiling_can_zero_copy(4, xs, ys_bottom, ws, hs, 1920, 1080, &q,
	                                   U_TILING_ORIGIN_TOP_LEFT));

	SECTION("a bottom-origin submission with the COLUMNS swapped still fails")
	{
		const int32_t xs_swapped[] = {960, 0, 960, 0};
		CHECK_FALSE(u_tiling_can_zero_copy(4, xs_swapped, ys_bottom, ws, hs, 1920, 1080, &q,
		                                   U_TILING_ORIGIN_BOTTOM_LEFT));
	}
}

TEST_CASE("zero-copy origin: the two origins are the SAME test at tile_rows == 1 (#1628)",
          "[aux][u_tiling]")
{
	// Every mode any shipped vendor plug-in publishes is single-row, which is
	// why #1628 was invisible and why this fix cannot regress them. Keep this
	// case: it is the blast-radius claim, asserted rather than asserted-about.
	const xrt_rendering_mode m = sbs_mode();
	const int32_t xs[] = {0, 960};
	const int32_t ys[] = {0, 0};
	const uint32_t ws[] = {960, 960};
	const uint32_t hs[] = {1080, 1080};

	CHECK(u_tiling_can_zero_copy(2, xs, ys, ws, hs, 1920, 1080, &m, U_TILING_ORIGIN_TOP_LEFT));
	CHECK(u_tiling_can_zero_copy(2, xs, ys, ws, hs, 1920, 1080, &m, U_TILING_ORIGIN_BOTTOM_LEFT));

	const xrt_rendering_mode mono = mono_mode();
	const int32_t xs1[] = {0, 0};
	const uint32_t ws1[] = {1920, 1920};
	const uint32_t hs1[] = {1080, 1080};
	CHECK(u_tiling_can_zero_copy(2, xs1, ys, ws1, hs1, 1920, 1080, &mono,
	                             U_TILING_ORIGIN_TOP_LEFT));
	CHECK(u_tiling_can_zero_copy(2, xs1, ys, ws1, hs1, 1920, 1080, &mono,
	                             U_TILING_ORIGIN_BOTTOM_LEFT));
}
