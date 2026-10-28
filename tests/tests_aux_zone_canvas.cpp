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

#include <cmath>

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

TEST_CASE("window orientation: a never-filled (zero) quaternion is the identity, not a rotation")
{
	// The Metal and GL compositors never write window_orientation, so it stays
	// zero-initialised. Read as "differs from (0,0,0,1)" it became a rotated
	// canvas whose zero quaternion zeroed every pose it was multiplied into
	// (the rig plane of RIG_LOCAL hands: joint orientations came back 0,0,0,0).
	struct xrt_window_metrics wm = {};
	struct xrt_quat q = {9, 9, 9, 9};
	CHECK_FALSE(u_canvas_window_orientation(&wm, &q));
	CHECK(q.x == 0.0f);
	CHECK(q.y == 0.0f);
	CHECK(q.z == 0.0f);
	CHECK(q.w == 1.0f);

	wm.window_orientation = xrt_quat{0, 0, 0, 1};
	CHECK_FALSE(u_canvas_window_orientation(&wm, &q));

	// A real rotation (30 deg about Y, scaled by 2) is detected and normalised.
	const float s = std::sin(0.2617994f), c = std::cos(0.2617994f);
	wm.window_orientation = xrt_quat{0, 2.0f * s, 0, 2.0f * c};
	CHECK(u_canvas_window_orientation(&wm, &q));
	CHECK(q.y == Catch::Approx(s).margin(1e-6));
	CHECK(q.w == Catch::Approx(c).margin(1e-6));
}

/*
 * #2305: u_canvas_rebase_display_origin — the window-relative Kooima after a
 * live arrangement change. Numbers from the Windows hardware run (Acer DS1,
 * 3840x2160, 0.3442 x 0.1936 m, a 3802x1911 window): the panel moved from
 * (-3840, 0) to (0, 1600) with the window on it, the Leia DP kept reporting
 * the session-start origin, and eye_offset read (0.3442, -0.1400) m instead of
 * (0.0000, 0.0035) m.
 */
namespace {

struct xrt_window_metrics
ds1_metrics(int32_t disp_left, int32_t disp_top, int32_t win_left, int32_t win_top)
{
	// Built the way the in-process compositors build them (get_window_metrics).
	struct xrt_window_metrics wm = {};
	wm.valid = true;
	wm.display_pixel_width = 3840;
	wm.display_pixel_height = 2160;
	wm.display_width_m = 0.3442f;
	wm.display_height_m = 0.1936f;
	wm.display_screen_left = disp_left;
	wm.display_screen_top = disp_top;
	wm.window_pixel_width = 3802;
	wm.window_pixel_height = 1911;
	wm.window_screen_left = win_left;
	wm.window_screen_top = win_top;
	const float px = wm.display_width_m / 3840.0f;
	const float py = wm.display_height_m / 2160.0f;
	wm.window_width_m = 3802.0f * px;
	wm.window_height_m = 1911.0f * py;
	const float cx = (float)(win_left - disp_left) + 3802.0f / 2.0f;
	const float cy = (float)(win_top - disp_top) + 1911.0f / 2.0f;
	wm.window_center_offset_x_m = (cx - 1920.0f) * px;
	wm.window_center_offset_y_m = -((cy - 1080.0f) * py);
	return wm;
}

} // namespace

TEST_CASE("rebase_display_origin: a DP stuck on the session-start origin follows the panel (W8b)")
{
	// The window moved with the panel: (-3821, 86) on (-3840, 0) -> (19, 1686) on (0, 1600).
	struct xrt_window_metrics stale = ds1_metrics(-3840, 0, 19, 1686);
	CHECK(stale.window_center_offset_x_m == Catch::Approx(0.3442f).margin(1e-4));
	CHECK(stale.window_center_offset_y_m == Catch::Approx(-0.1400f).margin(1e-4));

	const struct xrt_window_metrics truth = ds1_metrics(0, 1600, 19, 1686);
	REQUIRE(u_canvas_rebase_display_origin(&stale, -3840, 0, 0, 1600));
	CHECK(stale.window_center_offset_x_m == Catch::Approx(truth.window_center_offset_x_m).margin(1e-5));
	CHECK(stale.window_center_offset_y_m == Catch::Approx(truth.window_center_offset_y_m).margin(1e-5));
	CHECK(stale.window_center_offset_x_m == Catch::Approx(0.0f).margin(1e-4));
	CHECK(stale.window_center_offset_y_m == Catch::Approx(0.0035f).margin(1e-4));
	CHECK(stale.display_screen_left == 0);
	CHECK(stale.display_screen_top == 1600);
	// The window itself is untouched.
	CHECK(stale.window_screen_left == 19);
	CHECK(stale.window_width_m == Catch::Approx(truth.window_width_m));
}

TEST_CASE("rebase_display_origin: a zone canvas composed after the rebase is measured from the new origin")
{
	struct xrt_window_metrics stale = ds1_metrics(-3840, 0, 19, 1686);
	REQUIRE(u_canvas_rebase_display_origin(&stale, -3840, 0, 0, 1600));
	struct xrt_window_metrics truth = ds1_metrics(0, 1600, 19, 1686);
	struct u_canvas_rect zone_rect = {true, 100, 200, 640, 480};
	u_canvas_apply_to_metrics(&stale, &zone_rect);
	u_canvas_apply_to_metrics(&truth, &zone_rect);
	CHECK(stale.window_center_offset_x_m == Catch::Approx(truth.window_center_offset_x_m).margin(1e-5));
	CHECK(stale.window_center_offset_y_m == Catch::Approx(truth.window_center_offset_y_m).margin(1e-5));
}

TEST_CASE("rebase_display_origin: the W8b run-2 residual (a 130 px start offset) is gone")
{
	// Started at (-3840, -130), moved back to (-3840, 0): eye_offset y read -0.0082 m, should be 0.0035 m.
	struct xrt_window_metrics stale = ds1_metrics(-3840, -130, -3821, 86);
	CHECK(stale.window_center_offset_y_m == Catch::Approx(-0.0082f).margin(1e-4));
	REQUIRE(u_canvas_rebase_display_origin(&stale, -3840, -130, -3840, 0));
	CHECK(stale.window_center_offset_x_m == Catch::Approx(0.0f).margin(1e-4));
	CHECK(stale.window_center_offset_y_m == Catch::Approx(0.0035f).margin(1e-4));
}

TEST_CASE("rebase_display_origin: metrics that do not carry the start origin are left alone")
{
	// A DP that follows the panel already reports the new origin.
	struct xrt_window_metrics live = ds1_metrics(0, 1600, 19, 1686);
	const struct xrt_window_metrics before = live;
	CHECK_FALSE(u_canvas_rebase_display_origin(&live, -3840, 0, 0, 1600));
	CHECK(live.window_center_offset_x_m == before.window_center_offset_x_m);
	CHECK(live.window_center_offset_y_m == before.window_center_offset_y_m);
	CHECK(live.display_screen_left == 0);

	// A DP whose origin never agreed with the resolver (another coordinate space).
	struct xrt_window_metrics other = ds1_metrics(-1280, 0, -1270, 43);
	const struct xrt_window_metrics other_before = other;
	CHECK_FALSE(u_canvas_rebase_display_origin(&other, -3840, 0, 0, 1600));
	CHECK(other.window_center_offset_x_m == other_before.window_center_offset_x_m);
	CHECK(other.display_screen_left == -1280);
}

TEST_CASE("rebase_display_origin: no move, or nothing to rebase, changes nothing")
{
	struct xrt_window_metrics wm = ds1_metrics(-3840, 0, -3821, 86);
	const struct xrt_window_metrics before = wm;
	CHECK_FALSE(u_canvas_rebase_display_origin(&wm, -3840, 0, -3840, 0));
	CHECK(wm.window_center_offset_x_m == before.window_center_offset_x_m);
	CHECK(wm.window_center_offset_y_m == before.window_center_offset_y_m);

	struct xrt_window_metrics invalid = ds1_metrics(-3840, 0, 19, 1686);
	invalid.valid = false;
	CHECK_FALSE(u_canvas_rebase_display_origin(&invalid, -3840, 0, 0, 1600));
	CHECK(invalid.display_screen_left == -3840);

	struct xrt_window_metrics no_pixels = ds1_metrics(-3840, 0, 19, 1686);
	no_pixels.display_pixel_width = 0;
	CHECK_FALSE(u_canvas_rebase_display_origin(&no_pixels, -3840, 0, 0, 1600));

	CHECK_FALSE(u_canvas_rebase_display_origin(NULL, -3840, 0, 0, 1600));
}
