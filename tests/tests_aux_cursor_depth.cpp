// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for u_cursor_depth — depth-aware cursor placement
 *         (XR_DXR_cursor_depth, ADR-046).
 *
 * The views are built the way the runtime builds them: Kooima frusta from
 * each eye onto one physical canvas. The placement must then hold under any
 * rigid transform plus uniform scale of the whole scene (a camera rig is
 * exactly that), because it is solved from the views alone.
 */

#include "util/u_cursor_depth.h"

#include "catch_amalgamated.hpp"

#include <cmath>

namespace {

constexpr uint64_t MS = 1000000ULL;
constexpr float W = 0.30f; // canvas width, m
constexpr float H = 0.20f; // canvas height, m

struct Xform
{
	xrt_quat q{0, 0, 0, 1};
	xrt_vec3 t{0, 0, 0};
	float s = 1.0f;
};

xrt_vec3
rotate(xrt_quat q, xrt_vec3 v)
{
	// Reference implementation via the rotation matrix, independent of the unit under test.
	const float x = q.x, y = q.y, z = q.z, w = q.w;
	return {
	    (1 - 2 * (y * y + z * z)) * v.x + 2 * (x * y - z * w) * v.y + 2 * (x * z + y * w) * v.z,
	    2 * (x * y + z * w) * v.x + (1 - 2 * (x * x + z * z)) * v.y + 2 * (y * z - x * w) * v.z,
	    2 * (x * z - y * w) * v.x + 2 * (y * z + x * w) * v.y + (1 - 2 * (x * x + y * y)) * v.z,
	};
}

xrt_vec3
apply(const Xform &x, xrt_vec3 p)
{
	xrt_vec3 r = rotate(x.q, {p.x * x.s, p.y * x.s, p.z * x.s});
	return {r.x + x.t.x, r.y + x.t.y, r.z + x.t.z};
}

// Kooima view from a display-space eye onto the W x H canvas centred at the
// origin in the z = 0 plane, then carried into the locate space by x.
u_cursor_depth_view
kooima(xrt_vec3 eye, const Xform &x = Xform{})
{
	u_cursor_depth_view v{};
	const float d = eye.z;
	v.fov.angle_left = std::atan((-W / 2 - eye.x) / d);
	v.fov.angle_right = std::atan((W / 2 - eye.x) / d);
	v.fov.angle_up = std::atan((H / 2 - eye.y) / d);
	v.fov.angle_down = std::atan((-H / 2 - eye.y) / d);
	v.pose.orientation = x.q;
	v.pose.position = apply(x, eye);
	return v;
}

u_cursor_depth_tuning
tuning()
{
	u_cursor_depth_tuning t{};
	u_cursor_depth_tuning_defaults(&t);
	return t;
}

void
require_near(xrt_vec3 a, xrt_vec3 b, float eps = 1e-4f)
{
	CHECK(a.x == Catch::Approx(b.x).margin(eps));
	CHECK(a.y == Catch::Approx(b.y).margin(eps));
	CHECK(a.z == Catch::Approx(b.z).margin(eps));
}

} // namespace

TEST_CASE("cursor_depth: the canvas point under the cursor is where the view rays meet")
{
	const auto a = kooima({-0.032f, 0.0f, 0.6f});
	const auto b = kooima({0.032f, 0.0f, 0.6f});
	u_cursor_depth_geometry g{};
	REQUIRE(u_cursor_depth_geometry_solve(&a, &b, 0.25f, 0.75f, &g));
	require_near(g.canvas_point, {-W / 2 + 0.25f * W, H / 2 - 0.75f * H, 0.0f});
	require_near(g.eye, {0.0f, 0.0f, 0.6f});
	CHECK(g.eye_to_canvas == Catch::Approx(0.6f));
	CHECK(g.canvas_height == Catch::Approx(H));
}

TEST_CASE("cursor_depth: off-axis, rolled, unequal-depth eyes still find the canvas point")
{
	// Viewer up and to the right, head rolled, one eye nearer the glass.
	const auto a = kooima({0.05f, 0.03f, 0.55f});
	const auto b = kooima({0.11f, 0.045f, 0.57f});
	u_cursor_depth_geometry g{};
	REQUIRE(u_cursor_depth_geometry_solve(&a, &b, 0.9f, 0.1f, &g));
	require_near(g.canvas_point, {-W / 2 + 0.9f * W, H / 2 - 0.1f * H, 0.0f});
	CHECK(g.canvas_height == Catch::Approx(H));
}

TEST_CASE("cursor_depth: disparity is in eye-baseline units, 0 on the canvas, crossed in front")
{
	const auto a = kooima({-0.032f, 0.0f, 0.6f});
	const auto b = kooima({0.032f, 0.0f, 0.6f});
	u_cursor_depth_geometry g{};
	REQUIRE(u_cursor_depth_geometry_solve(&a, &b, 0.5f, 0.5f, &g));

	float d = 99.0f;
	xrt_vec3 on_canvas{0.0f, 0.0f, 0.0f};
	REQUIRE(u_cursor_depth_point_disparity(&g, &on_canvas, &d));
	CHECK(d == Catch::Approx(0.0f).margin(1e-5));

	// Halfway to the eye: the left eye sees it at +32 mm on screen, the right
	// at -32 mm: an on-screen disparity of -64 mm = -1 baseline.
	xrt_vec3 halfway{0.0f, 0.0f, 0.3f};
	REQUIRE(u_cursor_depth_point_disparity(&g, &halfway, &d));
	CHECK(d == Catch::Approx(-1.0f));

	// Disparity depends only on depth along the normal, not lateral offset.
	xrt_vec3 halfway_aside{0.05f, -0.02f, 0.3f};
	REQUIRE(u_cursor_depth_point_disparity(&g, &halfway_aside, &d));
	CHECK(d == Catch::Approx(-1.0f));

	xrt_vec3 behind{0.0f, 0.0f, -0.6f};
	REQUIRE(u_cursor_depth_point_disparity(&g, &behind, &d));
	CHECK(d == Catch::Approx(0.5f));

	xrt_vec3 behind_the_eye{0.0f, 0.0f, 0.9f};
	CHECK_FALSE(u_cursor_depth_point_disparity(&g, &behind_the_eye, &d));
}

TEST_CASE("cursor_depth: placement sits on the cyclopean ray and keeps its apparent size")
{
	const auto a = kooima({-0.032f, 0.0f, 0.6f});
	const auto b = kooima({0.032f, 0.0f, 0.6f});
	u_cursor_depth_geometry g{};
	REQUIRE(u_cursor_depth_geometry_solve(&a, &b, 0.25f, 0.75f, &g));

	xrt_vec3 pos{};
	float h = 0.0f;
	u_cursor_depth_place(&g, -1.0f, 0.05f, &pos, &h); // t = 0.5
	require_near(pos, {g.canvas_point.x * 0.5f, g.canvas_point.y * 0.5f, 0.3f});
	CHECK(h == Catch::Approx(0.05f * H * 0.5f));

	u_cursor_depth_place(&g, 0.0f, 0.0f, &pos, &h); // default height, on canvas
	require_near(pos, g.canvas_point);
	CHECK(h == Catch::Approx(U_CURSOR_DEPTH_DEFAULT_HEIGHT * H));

	// Round trip: the placed point has the disparity it was placed at.
	u_cursor_depth_place(&g, -0.37f, 0.05f, &pos, &h);
	float d = 0.0f;
	REQUIRE(u_cursor_depth_point_disparity(&g, &pos, &d));
	CHECK(d == Catch::Approx(-0.37f));
}

TEST_CASE("cursor_depth: invariant under any rigid transform + uniform scale (camera rigs)")
{
	Xform x;
	const float half = 0.5f * 0.7f; // ~40 degrees about a tilted axis
	const float n = std::sqrt(0.3f * 0.3f + 1.0f + 0.2f * 0.2f);
	x.q = {std::sin(half) * 0.3f / n, std::sin(half) * 1.0f / n, std::sin(half) * 0.2f / n, std::cos(half)};
	x.t = {3.0f, -1.5f, 7.0f};
	x.s = 12.5f; // virtual world 12.5x the physical display

	const auto a0 = kooima({-0.032f, 0.01f, 0.6f});
	const auto b0 = kooima({0.032f, 0.01f, 0.6f});
	const auto a1 = kooima({-0.032f, 0.01f, 0.6f}, x);
	const auto b1 = kooima({0.032f, 0.01f, 0.6f}, x);

	u_cursor_depth_geometry g0{}, g1{};
	REQUIRE(u_cursor_depth_geometry_solve(&a0, &b0, 0.3f, 0.6f, &g0));
	REQUIRE(u_cursor_depth_geometry_solve(&a1, &b1, 0.3f, 0.6f, &g1));
	require_near(g1.canvas_point, apply(x, g0.canvas_point), 1e-3f);
	CHECK(g1.canvas_height == Catch::Approx(g0.canvas_height * x.s));

	const xrt_vec3 p0{0.02f, -0.03f, 0.17f};
	const xrt_vec3 p1 = apply(x, p0);
	float d0 = 0.0f, d1 = 0.0f;
	REQUIRE(u_cursor_depth_point_disparity(&g0, &p0, &d0));
	REQUIRE(u_cursor_depth_point_disparity(&g1, &p1, &d1));
	CHECK(d1 == Catch::Approx(d0).margin(1e-4));

	xrt_vec3 c0{}, c1{};
	float h0 = 0.0f, h1 = 0.0f;
	u_cursor_depth_place(&g0, d0, 0.04f, &c0, &h0);
	u_cursor_depth_place(&g1, d1, 0.04f, &c1, &h1);
	require_near(c1, apply(x, c0), 1e-3f);
	CHECK(h1 == Catch::Approx(h0 * x.s).epsilon(1e-3));
}

TEST_CASE("cursor_depth: degenerate input is inactive, never garbage")
{
	const auto a = kooima({-0.032f, 0.0f, 0.6f});
	const auto b = kooima({0.032f, 0.0f, 0.6f});
	u_cursor_depth_geometry g{};

	// 2D mode: every view is the centroid view.
	const auto mono = kooima({0.0f, 0.0f, 0.6f});
	CHECK_FALSE(u_cursor_depth_geometry_solve(&mono, &mono, 0.5f, 0.5f, &g));

	CHECK_FALSE(u_cursor_depth_geometry_solve(&a, &b, -0.01f, 0.5f, &g));
	CHECK_FALSE(u_cursor_depth_geometry_solve(&a, &b, 0.5f, 1.01f, &g));
	CHECK_FALSE(u_cursor_depth_geometry_solve(&a, &b, NAN, 0.5f, &g));

	auto bad = b;
	bad.pose.position.x = NAN;
	CHECK_FALSE(u_cursor_depth_geometry_solve(&a, &bad, 0.5f, 0.5f, &g));
}

TEST_CASE("cursor_depth: target = content minus margin, clamped; nothing under it = the canvas")
{
	const auto t = tuning();
	CHECK(u_cursor_depth_target(&t, false, -0.4f) == 0.0f);
	CHECK(u_cursor_depth_target(&t, true, NAN) == 0.0f);
	CHECK(u_cursor_depth_target(&t, true, -0.2f) == Catch::Approx(-0.2f - t.margin));
	CHECK(u_cursor_depth_target(&t, true, 0.0f) == Catch::Approx(-t.margin));
	CHECK(u_cursor_depth_target(&t, true, -5.0f) == t.min_disparity);
	CHECK(u_cursor_depth_target(&t, true, 0.95f) == t.max_disparity);
	CHECK(t.max_disparity < 1.0f); // place() divides by 1 - d
}

TEST_CASE("cursor_depth: the filter rises fast and sinks slowly")
{
	const auto t = tuning();
	u_cursor_depth_filter f{};

	// First step snaps: no glide in from the canvas when the cursor appears.
	CHECK(u_cursor_depth_filter_step(&f, &t, -0.3f, 1000 * MS) == -0.3f);

	// Sink: one rise time constant later the cursor has barely moved back.
	float d = u_cursor_depth_filter_step(&f, &t, 0.0f, 1030 * MS);
	CHECK(d < -0.25f);

	// Rise from there: one rise tau covers ~63% of the gap.
	u_cursor_depth_filter f2{};
	u_cursor_depth_filter_step(&f2, &t, 0.0f, 1000 * MS);
	d = u_cursor_depth_filter_step(&f2, &t, -0.3f, 1030 * MS);
	CHECK(d == Catch::Approx(-0.3f * (1.0f - std::exp(-1.0f))).epsilon(1e-3));

	// A second locate in the same frame does not advance the filter.
	CHECK(u_cursor_depth_filter_step(&f2, &t, 0.5f, 1030 * MS) == d);

	// Converges.
	uint64_t now = 1030 * MS;
	for (int i = 0; i < 60; i++) {
		now += 16 * MS;
		d = u_cursor_depth_filter_step(&f2, &t, -0.3f, now);
	}
	CHECK(d == Catch::Approx(-0.3f).margin(1e-4));
}

TEST_CASE("cursor_depth: a long gap or a clock going backwards re-primes")
{
	const auto t = tuning();
	u_cursor_depth_filter f{};
	u_cursor_depth_filter_step(&f, &t, -0.3f, 1000 * MS);
	CHECK(u_cursor_depth_filter_step(&f, &t, 0.1f, 2000 * MS) == 0.1f);
	CHECK(u_cursor_depth_filter_step(&f, &t, -0.2f, 1500 * MS) == -0.2f);
}

TEST_CASE("cursor_depth: head motion does not move the cursor on the glass (screen-anchored, jitter-free)")
{
	// Project the placed sprite back onto the canvas (z = 0) from each eye. For a fixed cursor
	// UV and disparity, those two screen points must not depend on where the head is: the
	// sprite sits on the cyclopean ray, so each eye sees it at S -/+ (baseline/2)*d — tracking
	// motion or jitter cannot make it swim, only the content under it can change its depth.
	auto on_glass = [](xrt_vec3 eye, xrt_vec3 p) {
		const float k = eye.z / (eye.z - p.z);
		return xrt_vec3{eye.x + (p.x - eye.x) * k, eye.y + (p.y - eye.y) * k, 0.0f};
	};
	const float d = -0.4f;
	xrt_vec3 ref_l{}, ref_r{};
	const xrt_vec3 heads[] = {{0.0f, 0.0f, 0.6f}, {0.12f, -0.05f, 0.5f}, {-0.2f, 0.08f, 0.75f}};
	for (int i = 0; i < 3; i++) {
		const xrt_vec3 h = heads[i];
		const xrt_vec3 el{h.x - 0.032f, h.y, h.z}, er{h.x + 0.032f, h.y, h.z};
		const auto a = kooima(el);
		const auto b = kooima(er);
		u_cursor_depth_geometry g{};
		REQUIRE(u_cursor_depth_geometry_solve(&a, &b, 0.3f, 0.4f, &g));
		xrt_vec3 c{};
		float hgt = 0.0f;
		u_cursor_depth_place(&g, d, 0.03f, &c, &hgt);
		const xrt_vec3 sl = on_glass(el, c), sr = on_glass(er, c);
		// On-screen disparity is baseline * d, whatever the head pose.
		CHECK(sr.x - sl.x == Catch::Approx(0.064f * d).margin(1e-5));
		CHECK(sr.y == Catch::Approx(sl.y).margin(1e-5));
		if (i == 0) {
			ref_l = sl;
			ref_r = sr;
		} else {
			require_near(sl, ref_l, 1e-5f);
			require_near(sr, ref_r, 1e-5f);
		}
	}
}
