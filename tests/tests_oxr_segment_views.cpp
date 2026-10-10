// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Multi-screen M3: per-segment views — the view-count contract, the
 *         majority segment, the one-frame placement, the per-segment Kooima
 *         inputs, the view ranges and the inactive-tail alias.
 *
 * The locate entry point needs a session, a head device and a segmenting
 * compositor; the geometry it composes is pure and pinned here, together with
 * the shared Kooima core it feeds (displayxr-common, via aux_math), so "two
 * screens give two different, correct frusta" is asserted end to end.
 */

#include "catch_amalgamated.hpp"

#include "oxr_segment_views.h"

#include "displayxr_math_xrt.h"

#include <cmath>
#include <cstring>

namespace {

//! Two screens side by side: A 1920x1080 at (0,0), 0.344 x 0.1935 m, nominal
//! z 0.6; B 2560x1440 at (1920,0), 0.60 x 0.3375 m, nominal z 0.8.
//! The window: 800x600 at desktop (1600,200) -> A holds its left 320 px,
//! B its right 480 px.
xrt_segment_metrics
two_screen_table()
{
	xrt_segment_metrics m;
	std::memset(&m, 0, sizeof(m));
	m.count = 2;
	m.window_screen_left = 1600;
	m.window_screen_top = 200;
	m.window_pixel_width = 800;
	m.window_pixel_height = 600;
	m.canvas = {{0, 0}, {800, 600}};

	xrt_segment_metric &a = m.seg[0];
	a.screen_id = 0xA;
	a.window_rect = {{0, 0}, {320, 600}};
	a.screen_rect = {{1600, 200}, {320, 600}};
	a.screen_desktop_left = 0;
	a.screen_desktop_top = 0;
	a.screen_desktop_width = 1920;
	a.screen_desktop_height = 1080;
	a.screen_width_m = 0.344f;
	a.screen_height_m = 0.1935f;
	a.nominal_viewer_z_m = 0.6f;
	a.is_primary = true;
	a.woven = true;

	xrt_segment_metric &b = m.seg[1];
	b.screen_id = 0xB;
	b.window_rect = {{320, 0}, {480, 600}};
	b.screen_rect = {{0, 200}, {480, 600}};
	b.screen_desktop_left = 1920;
	b.screen_desktop_top = 0;
	b.screen_desktop_width = 2560;
	b.screen_desktop_height = 1440;
	b.screen_width_m = 0.60f;
	b.screen_height_m = 0.3375f;
	b.nominal_viewer_z_m = 0.8f;
	b.woven = true;
	return m;
}

} // namespace

TEST_CASE("view-set capacity: only where a window can ever be split", "[oxr][segment_views]")
{
	// No segmenting compositor (macOS/Android, a GL / Vulkan Windows session, a service session): 1.
	CHECK(oxr_segment_views_set_capacity(0, false) == 1);
	CHECK(oxr_segment_views_set_capacity(3, false) == 1);
	// One DP-backed screen (or none): 1 — the pre-M3 count.
	CHECK(oxr_segment_views_set_capacity(0, true) == 1);
	CHECK(oxr_segment_views_set_capacity(1, true) == 1);
	// Two or more: up to XRT_MAX_SEGMENTS.
	CHECK(oxr_segment_views_set_capacity(2, true) == 2);
	CHECK(oxr_segment_views_set_capacity(5, true) == XRT_MAX_SEGMENTS);
}

TEST_CASE("Windows: a screen counts toward the capacity with a D3D11 OR a D3D12 factory", "[oxr][segment_views]")
{
	int d3d11_factory = 0;
	int d3d12_factory = 0;
	// Multi-screen M6: both in-process Windows compositors segment, so either
	// factory makes the screen DP-backed.
	CHECK(oxr_segment_views_win_entry_has_dp(&d3d11_factory, &d3d12_factory));
	CHECK(oxr_segment_views_win_entry_has_dp(&d3d11_factory, nullptr));
	CHECK(oxr_segment_views_win_entry_has_dp(nullptr, &d3d12_factory));
	// A D3D12-only plug-in (no D3D11 factory) used to count zero screens.
	CHECK_FALSE(oxr_segment_views_win_entry_has_dp(nullptr, nullptr));

	// Two D3D12-only screens: two view sets, as two D3D11 screens give.
	uint32_t with_factory = 0;
	const void *entries[2][2] = {{nullptr, &d3d12_factory}, {nullptr, &d3d12_factory}};
	for (const auto &e : entries) {
		if (oxr_segment_views_win_entry_has_dp(e[0], e[1])) {
			with_factory++;
		}
	}
	CHECK(oxr_segment_views_set_capacity(with_factory, true) == 2);
}

TEST_CASE("Windows: a GL factory also makes a screen count toward the capacity (M6, GL)", "[oxr][segment_views]")
{
	int gl_factory = 0;
	CHECK(oxr_segment_views_win_entry_has_gl_dp(&gl_factory));
	CHECK_FALSE(oxr_segment_views_win_entry_has_gl_dp(nullptr));

	// The oxr_system loop: a screen counts when ANY in-process Windows
	// compositor that segments has a factory for it. Two GL-only screens give
	// two view sets, as two D3D11 screens do; a screen with no factory at all
	// does not count.
	struct entry
	{
		const void *d3d11, *d3d12, *gl;
	};
	const entry entries[] = {
	    {nullptr, nullptr, &gl_factory}, {nullptr, nullptr, &gl_factory}, {nullptr, nullptr, nullptr}};
	uint32_t with_factory = 0;
	for (const auto &e : entries) {
		const bool has_dp =
		    oxr_segment_views_win_entry_has_dp(e.d3d11, e.d3d12) || oxr_segment_views_win_entry_has_gl_dp(e.gl);
		if (has_dp) {
			with_factory++;
		}
	}
	CHECK(with_factory == 2);
	CHECK(oxr_segment_views_set_capacity(with_factory, true) == 2);
}

TEST_CASE("PRIMARY_MULTIVIEW_DXR reports one view set per possible segment", "[oxr][segment_views]")
{
	// Capacity 1: exactly the pre-M3 count (the device max).
	CHECK(oxr_segment_views_multiview_count(0, 1) == 0);
	CHECK(oxr_segment_views_multiview_count(2, 1) == 2);
	CHECK(oxr_segment_views_multiview_count(4, 1) == 4);
	CHECK(oxr_segment_views_multiview_count(4, 0) == 4); // 0 treated as 1
	// Capacity 2 (ds1-linux): device max x 2.
	CHECK(oxr_segment_views_multiview_count(1, 2) == 2);
	CHECK(oxr_segment_views_multiview_count(2, 2) == 4); // Leia
	CHECK(oxr_segment_views_multiview_count(4, 2) == 8); // sim_display quad
	// Capped at XRT_MAX_VIEWS: a device that already fills it gets no second set.
	CHECK(oxr_segment_views_multiview_count(XRT_MAX_VIEWS, 2) == XRT_MAX_VIEWS);
}

TEST_CASE("the #1499 mode floor asks one segment's share", "[oxr][segment_views]")
{
	// A multiview session fills any mode the device has, on every segment —
	// exactly the pre-M3 verdict (device max).
	CHECK(oxr_segment_views_per_segment_capacity(8, true, 4) == 4);
	CHECK(oxr_segment_views_per_segment_capacity(4, true, 2) == 2);
	// Stereo never splits: 2.
	CHECK(oxr_segment_views_per_segment_capacity(2, false, 4) == 2);
	// A legacy/unsplit count that does not exceed the device is unchanged.
	CHECK(oxr_segment_views_per_segment_capacity(4, true, 4) == 4);
}

TEST_CASE("majority segment: largest area, ties to the primary", "[oxr][segment_views]")
{
	xrt_segment_metrics m = two_screen_table();
	CHECK(oxr_segment_views_majority(&m) == 1); // B holds 480 of 800 px

	m.seg[1].window_rect.extent.w = 320; // exact tie: the primary (A) wins
	CHECK(oxr_segment_views_majority(&m) == 0);
	m.seg[0].is_primary = false;
	m.seg[1].is_primary = true;
	CHECK(oxr_segment_views_majority(&m) == 1);

	xrt_segment_metrics empty;
	std::memset(&empty, 0, sizeof(empty));
	CHECK(oxr_segment_views_majority(&empty) == 0);
}

TEST_CASE("segments are placed continuously across the seam in the majority frame", "[oxr][segment_views]")
{
	const xrt_segment_metrics m = two_screen_table();
	oxr_segment_layout l;
	REQUIRE(oxr_segment_views_layout(&m, &l));
	REQUIRE(l.majority == 1);

	const float pa = 0.344f / 1920.0f;
	const float pb = 0.60f / 2560.0f;

	// The majority keeps its true position on its own screen.
	CHECK(l.seg[1].ref_cx == Catch::Approx(l.seg[1].own_cx));
	CHECK(l.seg[1].own_cx == Catch::Approx((240.0f - 1280.0f) * pb));
	CHECK(l.seg[1].own_cy == Catch::Approx(-(500.0f - 720.0f) * pb * (0.3375f / 1440.0f) / pb));

	// A's own position is on ITS screen, at its right edge.
	CHECK(l.seg[0].own_cx == Catch::Approx((1760.0f - 960.0f) * pa));
	CHECK(l.seg[0].w_m == Catch::Approx(320.0f * pa));
	CHECK(l.seg[1].w_m == Catch::Approx(480.0f * pb));

	// No gap, no overlap: A's right edge is B's left edge, in metres, even
	// though the two pitches differ.
	const float a_right = l.seg[0].ref_cx + l.seg[0].w_m * 0.5f;
	const float b_left = l.seg[1].ref_cx - l.seg[1].w_m * 0.5f;
	CHECK(a_right == Catch::Approx(b_left).margin(1e-6));
	// Same rows: A sits at B's height.
	CHECK(l.seg[0].ref_cy == Catch::Approx(l.seg[1].ref_cy).margin(1e-6));

	// The window centre is the centre of the union.
	CHECK(l.window_ref_cx == Catch::Approx((l.seg[0].ref_cx - l.seg[0].w_m * 0.5f + b_left + l.seg[1].w_m) * 0.5f));

	// A screen of unknown size cannot be placed.
	xrt_segment_metrics bad = m;
	bad.seg[0].screen_width_m = 0.0f;
	CHECK_FALSE(oxr_segment_views_layout(&bad, &l));
}

TEST_CASE("a segment's window metrics: its own screen, its canvas centre in the reference frame",
          "[oxr][segment_views]")
{
	const xrt_segment_metrics m = two_screen_table();
	oxr_segment_layout l;
	REQUIRE(oxr_segment_views_layout(&m, &l));

	for (uint32_t i = 0; i < 2; i++) {
		xrt_window_metrics wm;
		oxr_segment_views_window_metrics(&m, &l, i, &wm);
		REQUIRE(wm.valid);
		// The raw channel's canvasRectPx = window - display: the segment on its own screen.
		CHECK(wm.window_screen_left - wm.display_screen_left == m.seg[i].screen_rect.offset.w);
		CHECK(wm.window_screen_top - wm.display_screen_top == m.seg[i].screen_rect.offset.h);
		CHECK(wm.window_pixel_width == (uint32_t)m.seg[i].window_rect.extent.w);
		CHECK(wm.display_width_m == Catch::Approx(m.seg[i].screen_width_m));
		CHECK(wm.window_width_m == Catch::Approx(l.seg[i].w_m));
		CHECK(wm.window_center_offset_x_m == Catch::Approx(l.seg[i].ref_cx));
		CHECK(wm.window_center_offset_y_m == Catch::Approx(l.seg[i].ref_cy));
	}
}

TEST_CASE("two screens of different size and nominal viewer give two different, correct frusta", "[oxr][segment_views]")
{
	const xrt_segment_metrics m = two_screen_table();
	oxr_segment_layout l;
	REQUIRE(oxr_segment_views_layout(&m, &l));

	xrt_fov fov[2][2];
	for (uint32_t i = 0; i < 2; i++) {
		// What the locate does: the screen's nominal eyes (untracked), carried
		// into the reference frame, then taken relative to the canvas centre.
		float dx = 0, dy = 0;
		oxr_segment_views_own_to_ref(&l, i, &dx, &dy);
		xrt_window_metrics wm;
		oxr_segment_views_window_metrics(&m, &l, i, &wm);
		const float ipd = 0.063f;
		const float z = m.seg[i].nominal_viewer_z_m;
		xrt_vec3 raw[2] = {
		    {-ipd / 2 + dx - wm.window_center_offset_x_m, dy - wm.window_center_offset_y_m, z},
		    {ipd / 2 + dx - wm.window_center_offset_x_m, dy - wm.window_center_offset_y_m, z},
		};
		const dxr_screen scr = {wm.window_width_m, wm.window_height_m};
		dxr_display3d_tunables dt = dxr_display3d_default_tunables();
		dt.virtual_display_height = wm.window_height_m; // identity m2v
		const xrt_vec3 nominal = {0.0f, 0.0f, z};
		dxr_xrt_view out[2];
		dxr_xrt_display3d_compute_views(raw, 2, &nominal, &scr, &dt, nullptr, out);

		// Kooima, by hand: the eye relative to the SEGMENT as it sits on its
		// OWN screen (own screen centre -> segment centre), over the segment.
		for (uint32_t e = 0; e < 2; e++) {
			const float ex = (e == 0 ? -ipd / 2 : ipd / 2) - l.seg[i].own_cx;
			const float ey = -l.seg[i].own_cy;
			const float hw = l.seg[i].w_m * 0.5f;
			const float hh = l.seg[i].h_m * 0.5f;
			CHECK(std::tan(out[e].fov.angle_right) == Catch::Approx((hw - ex) / z).epsilon(1e-3));
			CHECK(std::tan(out[e].fov.angle_left) == Catch::Approx((-hw - ex) / z).epsilon(1e-3));
			CHECK(std::tan(out[e].fov.angle_up) == Catch::Approx((hh - ey) / z).epsilon(1e-3));
			CHECK(std::tan(out[e].fov.angle_down) == Catch::Approx((-hh - ey) / z).epsilon(1e-3));
			fov[i][e] = out[e].fov;
		}
	}
	// Different screens, different frusta.
	CHECK(std::fabs(fov[0][0].angle_left - fov[1][0].angle_left) > 0.05f);
	CHECK(std::fabs(fov[0][0].angle_up - fov[1][0].angle_up) > 0.01f);
	// A sits right of its screen centre: its frustum looks right (both edges > 0 tilt).
	CHECK(fov[0][0].angle_right > -fov[0][0].angle_left);
	// B sits left of its screen centre: the opposite skew.
	CHECK(fov[1][0].angle_right < -fov[1][0].angle_left);
}

TEST_CASE("a stereo session spanning screens keeps the whole window, framed from the majority screen",
          "[oxr][segment_views]")
{
	const xrt_segment_metrics m = two_screen_table();
	xrt_window_metrics wm;
	oxr_segment_views_whole_window_metrics(&m, 1, &wm);
	REQUIRE(wm.valid);
	const float pb = 0.60f / 2560.0f;
	CHECK(wm.window_pixel_width == 800);
	CHECK(wm.window_width_m == Catch::Approx(800.0f * pb));
	// Window centre at desktop x 2000 = B-relative 80; B centre 1280.
	CHECK(wm.window_center_offset_x_m == Catch::Approx((80.0f - 1280.0f) * pb));
	CHECK(wm.display_screen_left == 1920);
}

TEST_CASE("views are contiguous per segment and the tail aliases view 0", "[oxr][segment_views]")
{
	uint32_t first[XRT_MAX_SEGMENTS] = {};
	uint32_t count[XRT_MAX_SEGMENTS] = {};

	// sim anaglyph (2 views) on two screens, MULTIVIEW reports 8.
	REQUIRE(oxr_segment_views_assign(2, 2, 8, first, count) == 4);
	CHECK(first[0] == 0);
	CHECK(count[0] == 2);
	CHECK(first[1] == 2);
	CHECK(count[1] == 2);

	// Quad on two screens fills all 8.
	REQUIRE(oxr_segment_views_assign(2, 4, 8, first, count) == 8);
	CHECK(first[1] == 4);

	// One segment (a window wholly on a secondary screen).
	REQUIRE(oxr_segment_views_assign(1, 2, 8, first, count) == 2);

	// Does not fit / too many segments / nothing per segment: one view set.
	CHECK(oxr_segment_views_assign(2, 4, 4, first, count) == 0);
	CHECK(oxr_segment_views_assign(XRT_MAX_SEGMENTS + 1, 1, 8, first, count) == 0);
	CHECK(oxr_segment_views_assign(2, 0, 8, first, count) == 0);

	// ADR-041 alias: active views are themselves, the tail is view 0.
	CHECK(oxr_segment_views_alias_source(0, 4) == 0);
	CHECK(oxr_segment_views_alias_source(3, 4) == 3);
	CHECK(oxr_segment_views_alias_source(4, 4) == 0);
	CHECK(oxr_segment_views_alias_source(7, 4) == 0);
}

TEST_CASE("a segment takes its DP's eyes tracked or not, like the single-screen path", "[oxr][segment_views]")
{
	// Untracked but valid (sim_display): accepted — not replaced by the
	// registry nominal viewer, so the primary panel's views do not jump when
	// the window crosses the seam.
	CHECK(oxr_segment_views_accept_eyes(true, true, 4));
	CHECK(oxr_segment_views_accept_eyes(true, true, 2));
	CHECK_FALSE(oxr_segment_views_accept_eyes(false, true, 2));
	CHECK_FALSE(oxr_segment_views_accept_eyes(true, false, 2));
	CHECK_FALSE(oxr_segment_views_accept_eyes(true, true, 0));
}

TEST_CASE("Quad mode on two segments: every view gets a real frustum", "[oxr][segment_views]")
{
	// The nominal branch must fill one eye per active view: a 4-view mode
	// with only 2 eyes left views 2..3 with zero FOVs on every segment.
	xrt_eye_position eyes[XRT_MAX_VIEWS];
	REQUIRE(oxr_segment_views_nominal_eyes(0.063f, 0.6f, 2, 0.0f, 0.0f, eyes) == 2);
	CHECK(eyes[0].x == Catch::Approx(-0.0315f));
	CHECK(eyes[1].x == Catch::Approx(0.0315f));
	REQUIRE(oxr_segment_views_nominal_eyes(0.063f, 0.6f, 1, 0.0f, 0.0f, eyes) ==
	        2); // mono: the pair, centred later

	const xrt_segment_metrics m = two_screen_table();
	oxr_segment_layout l;
	REQUIRE(oxr_segment_views_layout(&m, &l));
	uint32_t first[XRT_MAX_SEGMENTS] = {};
	uint32_t count[XRT_MAX_SEGMENTS] = {};
	REQUIRE(oxr_segment_views_assign(2, 4, 8, first, count) == 8);

	for (uint32_t i = 0; i < 2; i++) {
		float dx = 0, dy = 0;
		oxr_segment_views_own_to_ref(&l, i, &dx, &dy);
		xrt_window_metrics wm;
		oxr_segment_views_window_metrics(&m, &l, i, &wm);
		const uint32_t n =
		    oxr_segment_views_nominal_eyes(0.063f, m.seg[i].nominal_viewer_z_m, count[i], dx, dy, eyes);
		REQUIRE(n == 4);
		xrt_vec3 raw[4];
		for (uint32_t e = 0; e < 4; e++) {
			raw[e] = {eyes[e].x - wm.window_center_offset_x_m, eyes[e].y - wm.window_center_offset_y_m,
			          eyes[e].z};
		}
		const dxr_screen scr = {wm.window_width_m, wm.window_height_m};
		dxr_display3d_tunables dt = dxr_display3d_default_tunables();
		dt.virtual_display_height = wm.window_height_m;
		const xrt_vec3 nominal = {0.0f, 0.0f, m.seg[i].nominal_viewer_z_m};
		dxr_xrt_view out[4];
		dxr_xrt_display3d_compute_views(raw, 4, &nominal, &scr, &dt, nullptr, out);
		for (uint32_t e = 0; e < 4; e++) {
			INFO("segment " << i << " view " << e);
			CHECK(out[e].fov.angle_right - out[e].fov.angle_left > 0.05f);
			CHECK(out[e].fov.angle_up - out[e].fov.angle_down > 0.05f);
		}
	}
}

TEST_CASE("the frame's routing survives a later non-splitting locate", "[oxr][segment_views]")
{
	xrt_segment_view_routing frame;
	std::memset(&frame, 0, sizeof(frame));

	// The projection locate split the views...
	xrt_segment_view_routing split;
	std::memset(&split, 0, sizeof(split));
	split.count = 2;
	split.first_view[1] = 2;
	split.view_count[0] = split.view_count[1] = 2;
	oxr_segment_views_route_record(&frame, &split);

	// ...then a zone-scoped / camera-rig locate keeps one view set: it does
	// not record anything, so xrEndFrame still routes per segment.
	xrt_segment_view_routing out;
	oxr_segment_views_route_take(&frame, &out);
	CHECK(out.count == 2);
	CHECK(out.first_view[1] == 2);

	// The record is per frame: the next frame starts empty.
	oxr_segment_views_route_take(&frame, &out);
	CHECK(out.count == 0);
}

TEST_CASE("a display rig gets ONE m2v across segments", "[oxr][segment_views]")
{
	const float V = 0.24f; // XrDisplayRigDXR::virtualDisplayHeight

	// Side by side, different pitch (the two-screen table): m2v_k = V *
	// scale_k / h_k must be equal, so the seam has no gap and no overlap.
	{
		const xrt_segment_metrics m = two_screen_table();
		oxr_segment_layout l;
		REQUIRE(oxr_segment_views_layout(&m, &l));
		const float m2v0 = V * oxr_segment_views_vdh_scale(&l, 0) / l.seg[0].h_m;
		const float m2v1 = V * oxr_segment_views_vdh_scale(&l, 1) / l.seg[1].h_m;
		CHECK(m2v0 == Catch::Approx(m2v1));
		CHECK(m2v0 == Catch::Approx(V / l.window_ref_h));
		// Seam continuity in VIRTUAL units with that shared m2v.
		const float a_right = m2v0 * (l.seg[0].ref_cx + l.seg[0].w_m * 0.5f - l.window_ref_cx);
		const float b_left = m2v1 * (l.seg[1].ref_cx - l.seg[1].w_m * 0.5f - l.window_ref_cx);
		CHECK(a_right == Catch::Approx(b_left).margin(1e-6));
	}

	// Stacked vertically, same panel: each half is half the window, not a
	// full virtual display (which magnified each half 2x).
	{
		xrt_segment_metrics m;
		std::memset(&m, 0, sizeof(m));
		m.count = 2;
		m.window_screen_left = 100;
		m.window_screen_top = 980;
		m.window_pixel_width = 800;
		m.window_pixel_height = 200;
		for (uint32_t i = 0; i < 2; i++) {
			xrt_segment_metric &s = m.seg[i];
			s.screen_id = 1 + i;
			s.window_rect = {{0, (int)(i * 100)}, {800, 100}};
			s.screen_rect = {{100, i == 0 ? 980 : 0}, {800, 100}};
			s.screen_desktop_left = 0;
			s.screen_desktop_top = (int32_t)(i * 1080);
			s.screen_desktop_width = 1920;
			s.screen_desktop_height = 1080;
			s.screen_width_m = 0.344f;
			s.screen_height_m = 0.1935f;
			s.nominal_viewer_z_m = 0.6f;
		}
		m.seg[0].is_primary = true;
		oxr_segment_layout l;
		REQUIRE(oxr_segment_views_layout(&m, &l));
		CHECK(oxr_segment_views_vdh_scale(&l, 0) == Catch::Approx(0.5f));
		CHECK(oxr_segment_views_vdh_scale(&l, 1) == Catch::Approx(0.5f));
		// Bottom of the top segment meets the top of the bottom one.
		CHECK(l.seg[0].ref_cy - l.seg[0].h_m * 0.5f == Catch::Approx(l.seg[1].ref_cy + l.seg[1].h_m * 0.5f));
	}
}

TEST_CASE("mixed vendors: each segment uses its OWN pitch and its OWN DP eye pair", "[oxr][segment_views]")
{
	// Segment 0 on screen A (the Leia-like primary, its DP's no-face default:
	// both eyes at one point); segment 1 on screen B (a sim_display segment
	// DP: an eye pair +-32 mm), at a different pixel pitch.
	xrt_segment_metrics m = two_screen_table();
	m.seg[0].have_eyes = true;
	m.seg[0].eyes.valid = true;
	m.seg[0].eyes.count = 2;
	m.seg[0].eyes.eyes[0] = {0.0f, 0.10f, 0.60f};
	m.seg[0].eyes.eyes[1] = {0.0f, 0.10f, 0.60f};
	m.seg[1].have_eyes = true;
	m.seg[1].eyes.valid = true;
	m.seg[1].eyes.count = 2;
	m.seg[1].eyes.eyes[0] = {-0.032f, 0.0f, 0.60f};
	m.seg[1].eyes.eyes[1] = {0.032f, 0.0f, 0.60f};

	oxr_segment_layout l;
	REQUIRE(oxr_segment_views_layout(&m, &l));

	// Its own pitch: B's segment is 480 px x 0.60/2560 m, not A's pitch.
	xrt_window_metrics wm0, wm1;
	oxr_segment_views_window_metrics(&m, &l, 0, &wm0);
	oxr_segment_views_window_metrics(&m, &l, 1, &wm1);
	CHECK(wm0.window_width_m == Catch::Approx(320.0f * 0.344f / 1920.0f));
	CHECK(wm1.window_width_m == Catch::Approx(480.0f * 0.60f / 2560.0f));
	CHECK(wm1.display_width_m == Catch::Approx(0.60f));

	// Its own eye pair, carried into the reference frame — never segment 0's.
	float dx = 0, dy = 0;
	oxr_segment_views_own_to_ref(&l, 1, &dx, &dy);
	xrt_eye_positions e1;
	REQUIRE(oxr_segment_views_segment_eyes(&m.seg[1], dx, dy, 2, &e1));
	CHECK(e1.eyes[1].x - e1.eyes[0].x == Catch::Approx(0.064f));
	CHECK(e1.eyes[0].x == Catch::Approx(-0.032f + dx));
	CHECK(e1.eyes[0].y == Catch::Approx(dy));
	// Relative to its own canvas the pair straddles the eye offset by +-32 mm.
	CHECK((e1.eyes[0].x - wm1.window_center_offset_x_m) + 0.032f == Catch::Approx(-l.seg[1].own_cx).margin(1e-5));

	// A DP that under-reports for the segment's views (one eye for a 2-view
	// mode: every view from the SAME eye, an anaglyph that looks flat) is not
	// used; the nominal viewer (a real pair) takes over.
	xrt_segment_metric under = m.seg[1];
	under.eyes.count = 1;
	CHECK_FALSE(oxr_segment_views_segment_eyes(&under, dx, dy, 2, &e1));
	CHECK(oxr_segment_views_segment_eyes(&under, dx, dy, 1, &e1)); // a 1-view mode is covered
}
