// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pin the zone-scoped locate's canvas resolution (#1458).
 *
 * The IPC rig locate (`ipc_server_handler.c`) rebases the window metrics to
 * the zone rect so a display-zone tile gets its own off-axis frustum. Before
 * #1458 that rebase was gated on the zone fitting inside the reported canvas
 * (in either orientation); a zone hanging past the bottom or right edge fell
 * back to the FULL-canvas frame, so a square tile half scrolled into view — or
 * any tile on an Android page whose layout viewport had drifted from its
 * visual one — was drawn with the whole panel's frustum, ~2.2x too wide.
 *
 * The rule now lives in @ref u_canvas_zone_canvas_dims: pick the orientation
 * the zone overhangs LEAST, never reject. These cases are the measured ones
 * from the phone that found it (1080x2400 portrait, DPR 3, 320-CSS-px tiles),
 * plus the fixed-orientation-DP case the transpose exists for (#528/#570).
 */

#include "util/u_canvas.h"

#include "catch_amalgamated.hpp"

namespace {

struct xrt_window_metrics
portrait_phone_metrics()
{
	// Square-pixel pitch from the native width, exactly as the IPC handler
	// rebuilds the baseline (the panel's rounded metres are not quite square:
	// 0.0710/1080 vs 0.1570/2400 differ by 0.5%, which is the handler's reason
	// for deriving the height from the pitch rather than trusting both metres).
	const float pitch = 0.0710f / 1080.0f;
	struct xrt_window_metrics wm = {};
	wm.valid = true;
	wm.display_pixel_width = 1080;
	wm.display_pixel_height = 2400;
	wm.display_width_m = 1080.0f * pitch;
	wm.display_height_m = 2400.0f * pitch;
	wm.window_pixel_width = 1080;
	wm.window_pixel_height = 2400;
	wm.window_width_m = wm.display_width_m;
	wm.window_height_m = wm.display_height_m;
	return wm;
}

struct u_canvas_rect
zone(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	struct u_canvas_rect z = {};
	z.valid = true;
	z.x = x;
	z.y = y;
	z.w = w;
	z.h = h;
	return z;
}

} // namespace

TEST_CASE("zone_canvas: a zone inside the reported canvas keeps the reported dims")
{
	// The product-page stage: 960 px square, fully on screen.
	const struct u_canvas_rect z = zone(60, 300, 960, 960);
	uint32_t w = 0, h = 0;
	const bool transposed = u_canvas_zone_canvas_dims(1080, 2400, &z, &w, &h);
	CHECK_FALSE(transposed);
	CHECK(w == 1080);
	CHECK(h == 2400);
}

TEST_CASE("zone_canvas: a zone that only fits the transposed canvas transposes (#528 fixed-orientation DP)")
{
	// The metrics source still says landscape while the app submits a
	// portrait-space zone that runs to y=2000.
	const struct u_canvas_rect z = zone(100, 1500, 800, 500);
	uint32_t w = 0, h = 0;
	const bool transposed = u_canvas_zone_canvas_dims(2400, 1080, &z, &w, &h);
	CHECK(transposed);
	CHECK(w == 1080);
	CHECK(h == 2400);
}

TEST_CASE("zone_canvas: a zone hanging past the bottom in BOTH orientations is kept, not rejected (#1458)")
{
	// The storefront hero as the phone's browser reported it: 960 px square at
	// y=1955 in a 2400-high window — 515 px past the bottom. Transposed it would
	// be 1835 px past, so the reported orientation wins and the zone is applied.
	const struct u_canvas_rect z = zone(60, 1955, 960, 960);
	uint32_t w = 0, h = 0;
	const bool transposed = u_canvas_zone_canvas_dims(1080, 2400, &z, &w, &h);
	CHECK_FALSE(transposed);
	CHECK(w == 1080);
	CHECK(h == 2400);

	// And the metrics the Kooima then runs on describe the ZONE, not the panel:
	// a square in metres. This is the line that was 0.452 (= 0.071 / 0.157).
	struct xrt_window_metrics wm = portrait_phone_metrics();
	u_canvas_apply_to_metrics(&wm, &z);
	CHECK(wm.window_pixel_width == 960);
	CHECK(wm.window_pixel_height == 960);
	CHECK(wm.window_width_m / wm.window_height_m == Catch::Approx(1.0f).epsilon(0.001f));
	// The zone centre sits below the display centre, so the vertical offset is
	// negative (y up in metres) and the horizontal one is ~0 (centred tile).
	CHECK(wm.window_center_offset_y_m < 0.0f);
	CHECK(wm.window_center_offset_x_m == Catch::Approx(0.0f).margin(0.001f));
}

TEST_CASE("zone_canvas: a top-clipped zone (negative y) keeps the reported dims")
{
	// Half scrolled off the top: y=-300, bottom at 660. Fits by the far edge,
	// and a negative origin is not evidence of the other orientation.
	const struct u_canvas_rect z = zone(60, -300, 960, 960);
	uint32_t w = 0, h = 0;
	const bool transposed = u_canvas_zone_canvas_dims(1080, 2400, &z, &w, &h);
	CHECK_FALSE(transposed);
	CHECK(w == 1080);
	CHECK(h == 2400);

	struct xrt_window_metrics wm = portrait_phone_metrics();
	u_canvas_apply_to_metrics(&wm, &z);
	CHECK(wm.window_width_m / wm.window_height_m == Catch::Approx(1.0f).epsilon(0.001f));
	CHECK(wm.window_center_offset_y_m > 0.0f); // above centre now
}

TEST_CASE("zone_canvas: a tie keeps the reported dims, so the choice is stable frame to frame")
{
	// Square canvas: transposing changes nothing, so it must not report as a
	// transpose either.
	const struct u_canvas_rect z = zone(0, 500, 400, 400);
	uint32_t w = 0, h = 0;
	CHECK_FALSE(u_canvas_zone_canvas_dims(800, 800, &z, &w, &h));
	CHECK(w == 800);
	CHECK(h == 800);
}

TEST_CASE("zone_canvas: an invalid or empty zone leaves the reported dims alone")
{
	struct u_canvas_rect z = zone(0, 0, 0, 100);
	uint32_t w = 0, h = 0;
	CHECK_FALSE(u_canvas_zone_canvas_dims(1080, 2400, &z, &w, &h));
	CHECK(w == 1080);
	CHECK(h == 2400);

	z = zone(0, 0, 100, 100);
	z.valid = false;
	CHECK_FALSE(u_canvas_zone_canvas_dims(2400, 1080, &z, &w, &h));
	CHECK(w == 2400);
	CHECK(h == 1080);

	// A zero-sized canvas has no orientation to pick; report what came in.
	z = zone(0, 0, 100, 100);
	CHECK_FALSE(u_canvas_zone_canvas_dims(0, 0, &z, &w, &h));
	CHECK(w == 0);
	CHECK(h == 0);
}
