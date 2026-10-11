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
#include <cstdlib>
#include <cstring>
#include <vector>

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

xrt_vec3
on_glass(xrt_vec3 eye, xrt_vec3 p)
{
	// Where the line from eye through p crosses the canvas plane z = 0.
	const float k = eye.z / (eye.z - p.z);
	return xrt_vec3{eye.x + (p.x - eye.x) * k, eye.y + (p.y - eye.y) * k, 0.0f};
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
	CHECK(u_cursor_depth_target(&t, true, -5.0f) == t.min_disparity);
	CHECK(t.max_disparity < 1.0f); // place() divides by 1 - d
}

TEST_CASE("cursor_depth: never behind the glass (Amendment 2 A.1)")
{
	const auto t = tuning();
	CHECK(t.max_disparity == 0.0f);
	// Content ON the canvas: the cursor stays on it (it used to float margin in front).
	CHECK(u_cursor_depth_target(&t, true, 0.0f) == 0.0f);
	// Content behind it: the cursor stays on the canvas (it used to sink onto it).
	CHECK(u_cursor_depth_target(&t, true, 0.3f) == 0.0f);
	CHECK(u_cursor_depth_target(&t, true, 0.95f) == 0.0f);
	CHECK(u_cursor_depth_target(&t, true, INFINITY) == 0.0f);
	// Content just in front: margin in front of it.
	CHECK(u_cursor_depth_target(&t, true, -0.001f) == Catch::Approx(-0.001f - t.margin));

	// A tuning asking for room behind the glass cannot get it.
	auto loose = t;
	loose.max_disparity = 0.6f;
	CHECK(u_cursor_depth_target(&loose, true, 0.3f) == 0.0f);
	loose.margin = -0.2f; // a margin pushing the cursor BEHIND content in front
	CHECK(u_cursor_depth_target(&loose, true, -0.1f) <= 0.0f);
}

TEST_CASE("cursor_depth: property - the target is never > 0, for any content and geometry")
{
	// Random heads, cursor UVs and content points (in front of and behind the
	// canvas, through the real geometry + disparity path), random tunings.
	uint32_t seed = 0x1847u;
	auto rnd = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return (float)(seed >> 8) / (float)(1u << 24);
	};
	int behind = 0, in_front = 0;
	for (int i = 0; i < 4000; i++) {
		const xrt_vec3 head{(rnd() - 0.5f) * 0.4f, (rnd() - 0.5f) * 0.2f, 0.35f + rnd() * 0.6f};
		const auto a = kooima({head.x - 0.032f, head.y, head.z});
		const auto b = kooima({head.x + 0.032f, head.y, head.z});
		const float u = rnd(), v = rnd();
		u_cursor_depth_geometry g{};
		REQUIRE(u_cursor_depth_geometry_solve(&a, &b, u, v, &g));
		// Content anywhere from 90% of the way to the eye to 3 m behind the glass.
		const float z = -3.0f + rnd() * (3.0f + 0.9f * head.z);
		const xrt_vec3 p{(rnd() - 0.5f) * 0.6f, (rnd() - 0.5f) * 0.4f, z};
		float d = 0.0f;
		if (!u_cursor_depth_point_disparity(&g, &p, &d)) {
			continue;
		}
		auto t = tuning();
		if (i % 3 == 1) {
			t.max_disparity = rnd() * 0.8f; // misconfigured: room behind the glass
		}
		if (i % 5 == 2) {
			t.margin = (rnd() - 0.5f) * 0.1f; // including <= 0
		}
		const float target = u_cursor_depth_target(&t, true, d);
		CHECK(target <= 0.0f);
		if (d >= 0.0f) {
			behind++;
			CHECK(target == 0.0f);
		} else {
			in_front++;
			if (t.margin > 0.0f && t.max_disparity >= 0.0f) {
				// In front of the content, inside the comfort clamp.
				CHECK(target == Catch::Approx(std::fmax(d - t.margin, t.min_disparity)));
				if (d - t.margin > t.min_disparity) {
					CHECK(target < d);
				}
			}
		}
	}
	CHECK(behind > 500);
	CHECK(in_front > 500);
}

TEST_CASE("cursor_depth: draw threshold - on the plane it is the OS cursor (Amendment 2 A.3)")
{
	const auto t = tuning();
	CHECK(t.draw_threshold == Catch::Approx(-0.005f));
	CHECK_FALSE(u_cursor_depth_should_draw(&t, 0.0f));
	CHECK_FALSE(u_cursor_depth_should_draw(&t, -0.0049f));
	CHECK(u_cursor_depth_should_draw(&t, -0.0051f));
	CHECK(u_cursor_depth_should_draw(&t, -0.3f));
	CHECK_FALSE(u_cursor_depth_should_draw(&t, NAN));
	CHECK_FALSE(u_cursor_depth_should_draw(&t, 0.2f));
	// A threshold at or behind the glass falls back to the default.
	auto bad = t;
	bad.draw_threshold = 0.1f;
	CHECK_FALSE(u_cursor_depth_should_draw(&bad, 0.0f));
	CHECK_FALSE(u_cursor_depth_should_draw(&bad, -0.004f));
	CHECK(u_cursor_depth_should_draw(&bad, -0.006f));
}

TEST_CASE("cursor_depth: the drawn cursor hands back smoothly - the filter steps while hidden")
{
	// Content pops out, then goes away: the cursor sinks back over ~250 ms,
	// drawn until it crosses the threshold, then hidden; never a jump to 0.
	const auto t = tuning();
	u_cursor_depth_filter f{};
	uint64_t now = 1000 * MS;
	float d = u_cursor_depth_filter_step(&f, &t, u_cursor_depth_target(&t, true, -0.2f), now);
	CHECK(u_cursor_depth_should_draw(&t, d));
	float prev = d;
	bool hidden = false;
	for (int i = 0; i < 200; i++) {
		now += 16 * MS;
		d = u_cursor_depth_filter_step(&f, &t, u_cursor_depth_target(&t, true, 0.4f), now);
		CHECK(d >= prev);
		CHECK(d - prev < 0.05f);
		CHECK(d <= 0.0f);
		if (!u_cursor_depth_should_draw(&t, d)) {
			hidden = true;
		} else {
			CHECK_FALSE(hidden); // once handed back, stays handed back
		}
		prev = d;
	}
	CHECK(hidden);
	// It comes back fast when content pops out again.
	now += 16 * MS;
	d = u_cursor_depth_filter_step(&f, &t, u_cursor_depth_target(&t, true, -0.2f), now);
	now += 16 * MS;
	d = u_cursor_depth_filter_step(&f, &t, u_cursor_depth_target(&t, true, -0.2f), now);
	CHECK(u_cursor_depth_should_draw(&t, d));
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

TEST_CASE("cursor_depth: SCREEN anchor - head motion does not move the cursor on the glass")
{
	// The SCREEN-mode property (spec v1/v2 behaviour: u_cursor_depth_place and
	// U_CURSOR_DEPTH_ANCHOR_SCREEN). Project the placed sprite back onto the canvas (z = 0)
	// from each eye. For a fixed cursor UV and disparity, those two screen points do not
	// depend on where the head is: the sprite sits on the cyclopean ray, so each eye sees it
	// at S -/+ (baseline/2)*d. That is exactly why it has NO motion parallax - the conflict
	// HYBRID (the default since spec v3) resolves while the pointer is still.
	const float d = -0.4f;
	xrt_vec3 ref_l{}, ref_r{};
	const xrt_vec3 heads[] = {{0.0f, 0.0f, 0.6f}, {0.12f, -0.05f, 0.5f}, {-0.2f, 0.08f, 0.75f}};
	u_cursor_depth_anchor anchor{};
	for (int i = 0; i < 3; i++) {
		const xrt_vec3 h = heads[i];
		const xrt_vec3 el{h.x - 0.032f, h.y, h.z}, er{h.x + 0.032f, h.y, h.z};
		const auto a = kooima(el);
		const auto b = kooima(er);
		u_cursor_depth_geometry g{};
		REQUIRE(u_cursor_depth_geometry_solve(&a, &b, 0.3f, 0.4f, &g));
		xrt_vec3 c{}, c2{};
		float hgt = 0.0f, hgt2 = 0.0f;
		u_cursor_depth_place(&g, d, 0.03f, &c, &hgt);
		// The anchored entry point in SCREEN mode is the same placement, and ignores the anchor.
		u_cursor_depth_place_anchored(&g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_SCREEN, &anchor, 0.3f, 0.4f, false,
		                              (uint64_t)(i + 1) * 16 * MS, &c2, &hgt2);
		require_near(c2, c, 1e-6f);
		CHECK(hgt2 == hgt);
		CHECK_FALSE(anchor.has_anchor);
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


/*
 *
 * Spec v3: anchor modes.
 *
 */

namespace {

struct HeadGeom
{
	xrt_vec3 el, er;
	u_cursor_depth_geometry g;
};

HeadGeom
head(xrt_vec3 h, float u, float v)
{
	HeadGeom r{};
	r.el = {h.x - 0.032f, h.y, h.z};
	r.er = {h.x + 0.032f, h.y, h.z};
	const auto a = kooima(r.el);
	const auto b = kooima(r.er);
	REQUIRE(u_cursor_depth_geometry_solve(&a, &b, u, v, &r.g));
	return r;
}

} // namespace

TEST_CASE("cursor_depth: margin default is 0.015 (Amendment 2 A.2)")
{
	// Spec v3 briefly used 0.005; Amendment 2 set 0.03, halved to 0.015 on
	// 2026-10-11 after the Windows Leia panel check (0.03 floated visibly off
	// the content). ~1 mm of crossed on-screen disparity at a 63-65 mm IPD.
	CHECK(tuning().margin == Catch::Approx(0.015f));
	CHECK(U_CURSOR_DEPTH_DEFAULT_MARGIN == Catch::Approx(0.015f));

	// The behaviour, not just the constant: content at d = -0.2 puts the
	// default-tuned cursor at -0.215 (it was -0.23 with 0.03).
	const auto t = tuning();
	CHECK(u_cursor_depth_target(&t, true, -0.2f) == Catch::Approx(-0.215f).margin(1e-6f));
	CHECK(0.015f * 64.0f == Catch::Approx(0.96f)); // ~1 mm at a 64 mm IPD
}

TEST_CASE("cursor_depth: HYBRID - pointer moving = SCREEN placement (on the cyclopean ray)")
{
	const float d = -0.4f;
	u_cursor_depth_anchor anchor{};
	const float uvs[][2] = {{0.3f, 0.4f}, {0.31f, 0.4f}, {0.31f, 0.45f}, {0.7f, 0.2f}};
	const xrt_vec3 heads[] = {
	    {0.0f, 0.0f, 0.6f}, {0.12f, -0.05f, 0.5f}, {-0.2f, 0.08f, 0.75f}, {0.05f, 0.0f, 0.6f}};
	for (int i = 0; i < 4; i++) {
		const HeadGeom hg = head(heads[i], uvs[i][0], uvs[i][1]);
		xrt_vec3 c{}, cs{};
		float h = 0.0f, hs = 0.0f;
		u_cursor_depth_place_anchored(&hg.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, uvs[i][0],
		                              uvs[i][1], false, (uint64_t)(i + 1) * 16 * MS, &c, &h);
		u_cursor_depth_place(&hg.g, d, 0.03f, &cs, &hs);
		require_near(c, cs, 1e-6f);
		CHECK(h == hs);
		CHECK(anchor.has_anchor);
		// The stored anchor is this locate's line of sight.
		require_near(anchor.eye, hg.g.eye, 1e-6f);
		require_near(anchor.canvas_point, hg.g.canvas_point, 1e-6f);
		CHECK(anchor.eye_to_canvas == hg.g.eye_to_canvas);
	}
}

TEST_CASE("cursor_depth: HYBRID - pointer still, head moves = world-fixed (parallaxes like content)")
{
	const float d = -0.4f;
	const float u = 0.3f, v = 0.4f;
	u_cursor_depth_anchor anchor{};

	// Frame 1: the pointer arrives (moving) with the head off-axis.
	const HeadGeom h0 = head({0.1f, 0.03f, 0.6f}, u, v);
	xrt_vec3 c0{};
	float hgt = 0.0f;
	u_cursor_depth_place_anchored(&h0.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false, 16 * MS, &c0,
	                              &hgt);
	const xrt_vec3 img0 = on_glass(h0.el, c0);

	// Then the pointer is still and the head moves laterally (same distance):
	// the sprite stays put in locate space.
	const xrt_vec3 lateral[] = {{-0.1f, 0.0f, 0.6f}, {0.2f, -0.06f, 0.6f}, {0.0f, 0.0f, 0.6f}};
	uint64_t now = 16 * MS;
	for (const auto &hp : lateral) {
		now += 16 * MS;
		const HeadGeom hg = head(hp, u, v);
		xrt_vec3 c{};
		u_cursor_depth_place_anchored(&hg.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false, now,
		                              &c, &hgt);
		require_near(c, c0, 1e-5f);

		// ... so its image on the glass moves with the head like content at that depth,
		// while a SCREEN-anchored sprite's image would not move at all.
		xrt_vec3 cs{};
		float hs = 0.0f;
		u_cursor_depth_place(&hg.g, d, 0.03f, &cs, &hs);
		CHECK(std::fabs(on_glass(hg.el, c).x - img0.x) > 1e-3f);
		CHECK(on_glass(hg.el, cs).x == Catch::Approx(img0.x).margin(1e-5));
	}

	// Head moves toward the glass: the same disparity is a shallower depth. The
	// sprite slides along the anchor-time line of sight to that depth.
	now += 16 * MS;
	const HeadGeom hz = head({0.0f, 0.0f, 0.45f}, u, v);
	xrt_vec3 cz{};
	u_cursor_depth_place_anchored(&hz.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false, now, &cz,
	                              &hgt);
	require_near(on_glass(h0.g.eye, cz), h0.g.canvas_point, 5e-5f);
	float dz = 0.0f;
	REQUIRE(u_cursor_depth_point_disparity(&hz.g, &cz, &dz));
	CHECK(dz == Catch::Approx(d).margin(2e-4));

	// A re-prime (stale gap) re-anchors: back on the cyclopean ray.
	const HeadGeom hr = head({-0.15f, 0.0f, 0.6f}, u, v);
	xrt_vec3 cr{}, cs{};
	float hs = 0.0f;
	u_cursor_depth_place_anchored(&hr.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, true,
	                              now + 2000 * MS, &cr, &hgt);
	u_cursor_depth_place(&hr.g, d, 0.03f, &cs, &hs);
	require_near(cr, cs, 1e-6f);
	CHECK(std::fabs(cr.x - c0.x) > 1e-3f);
}

TEST_CASE("cursor_depth: HYBRID - continuous on the frame the pointer stops")
{
	u_cursor_depth_anchor anchor{};
	const HeadGeom prev = head({0.12f, -0.04f, 0.55f}, 0.6f, 0.33f);
	const HeadGeom hg = head({0.12f, -0.04f, 0.55f}, 0.62f, 0.33f);
	float hgt = 0.0f;

	// Moving: a frame at another UV, then the final moving frame.
	xrt_vec3 c_prev{}, c_moving{}, c_still{};
	u_cursor_depth_place_anchored(&prev.g, -0.25f, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, 0.6f, 0.33f, false,
	                              16 * MS, &c_prev, &hgt);
	u_cursor_depth_place_anchored(&hg.g, -0.3f, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, 0.62f, 0.33f, false,
	                              32 * MS, &c_moving, &hgt);
	// Stop frame: same UV, same t, same head.
	u_cursor_depth_place_anchored(&hg.g, -0.3f, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, 0.62f, 0.33f, false,
	                              48 * MS, &c_still, &hgt);
	require_near(c_still, c_moving, 1e-6f);

	// A UV change below the move threshold is "still" too (no re-anchor on noise).
	xrt_vec3 c_noise{};
	u_cursor_depth_place_anchored(&hg.g, -0.3f, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, 0.62f + 5e-5f, 0.33f,
	                              false, 64 * MS, &c_noise, &hgt);
	require_near(c_noise, c_moving, 1e-6f);
	CHECK(anchor.last_ns == 32 * MS);

	// Still, but the content under it changes depth (same head): the sprite slides
	// along the line of sight, i.e. it equals the SCREEN placement at the new depth.
	xrt_vec3 c_deeper{}, c_deeper_screen{};
	float hs = 0.0f;
	u_cursor_depth_place_anchored(&hg.g, -0.1f, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, 0.62f, 0.33f, false,
	                              80 * MS, &c_deeper, &hgt);
	u_cursor_depth_place(&hg.g, -0.1f, 0.03f, &c_deeper_screen, &hs);
	require_near(c_deeper, c_deeper_screen, 1e-5f);
	float dd = 0.0f;
	REQUIRE(u_cursor_depth_point_disparity(&hg.g, &c_deeper, &dd));
	CHECK(dd == Catch::Approx(-0.1f).margin(1e-5));
}

TEST_CASE("cursor_depth: HYBRID - content appears under a still pointer: stays on the click point, WORLD does not")
{
	// The case that motivated anchoring the LINE OF SIGHT rather than its foot: the
	// anchor is stored with the cursor on the glass (d0 = 0, e.g. before the first
	// hit test), then the content under the still pointer pops out. A viewer well
	// off-axis (eye 0.10 m above and 0.08 m left of the pointer's canvas point).
	const float u = 0.75f, v = 0.5f;
	const HeadGeom hg = head({0.0f, 0.1f, 0.6f}, u, v);
	u_cursor_depth_anchor anchor{};
	xrt_vec3 c{};
	float hgt = 0.0f;
	u_cursor_depth_place_anchored(&hg.g, 0.0f, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false, 16 * MS,
	                              &c, &hgt);
	require_near(c, hg.g.canvas_point, 1e-5f);

	uint64_t now = 16 * MS;
	const float ds[] = {-0.02f, -0.1f, -0.3f, 0.2f};
	for (float d : ds) {
		now += 16 * MS;
		u_cursor_depth_place_anchored(&hg.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false, now,
		                              &c, &hgt);
		// On the E0 -> S0 line: seen from E0 it projects onto S0, the click point...
		require_near(on_glass(anchor.eye, c), anchor.canvas_point, 5e-5f);
		// ... at the requested depth.
		float back = 0.0f;
		REQUIRE(u_cursor_depth_point_disparity(&hg.g, &c, &back));
		CHECK(back == Catch::Approx(d).margin(1e-4));

		// WORLD rises along the normal instead: off the click point for this viewer.
		xrt_vec3 cw{};
		u_cursor_depth_place_anchored(&hg.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_WORLD, nullptr, u, v, false, now,
		                              &cw, &hgt);
		const xrt_vec3 pw = on_glass(hg.g.eye, cw);
		CHECK(std::fabs(pw.y - hg.g.canvas_point.y) + std::fabs(pw.x - hg.g.canvas_point.x) > 1e-3f);
	}

	// And after the head moves laterally, the sprite is still world-fixed at fixed t.
	xrt_vec3 c_before = c;
	const HeadGeom moved_head = head({0.12f, 0.1f, 0.6f}, u, v);
	xrt_vec3 c_after{};
	u_cursor_depth_place_anchored(&moved_head.g, 0.2f, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false,
	                              now + 16 * MS, &c_after, &hgt);
	require_near(c_after, c_before, 1e-5f);
}

TEST_CASE("cursor_depth: WORLD - in front of the canvas point along the normal, at the requested disparity")
{
	const float d = -0.37f;
	const float t = 1.0f / (1.0f - d);

	// On-axis: E centred on S.
	{
		const HeadGeom hg = head({0.0f, 0.0f, 0.6f}, 0.5f, 0.5f);
		xrt_vec3 c{}, cs{};
		float h = 0.0f, hs = 0.0f;
		u_cursor_depth_place_anchored(&hg.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_WORLD, nullptr, 0.5f, 0.5f, false,
		                              16 * MS, &c, &h);
		const float lift = (1.0f - t) * hg.g.eye_to_canvas;
		require_near(c,
		             {hg.g.canvas_point.x - hg.g.forward.x * lift, hg.g.canvas_point.y - hg.g.forward.y * lift,
		              hg.g.canvas_point.z - hg.g.forward.z * lift},
		             1e-6f);
		CHECK(c.z > 0.0f); // in front of the glass (toward the viewer)
		float back = 0.0f;
		REQUIRE(u_cursor_depth_point_disparity(&hg.g, &c, &back));
		CHECK(back == Catch::Approx(d).margin(1e-5));
		// On-axis, WORLD and SCREEN coincide; the height is the same in every mode.
		u_cursor_depth_place(&hg.g, d, 0.03f, &cs, &hs);
		require_near(c, cs, 1e-6f);
		CHECK(h == hs);
	}

	// Off-axis: straight in front of S (same x, y), not on the line of sight.
	{
		const HeadGeom hg = head({0.15f, 0.05f, 0.6f}, 0.2f, 0.7f);
		xrt_vec3 c{}, cs{};
		float h = 0.0f, hs = 0.0f;
		u_cursor_depth_anchor untouched{};
		u_cursor_depth_place_anchored(&hg.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_WORLD, &untouched, 0.2f, 0.7f,
		                              false, 16 * MS, &c, &h);
		CHECK_FALSE(untouched.has_anchor);
		CHECK(c.x == Catch::Approx(hg.g.canvas_point.x).margin(1e-6));
		CHECK(c.y == Catch::Approx(hg.g.canvas_point.y).margin(1e-6));
		CHECK(c.z == Catch::Approx((1.0f - t) * hg.g.eye_to_canvas).margin(1e-6));
		float back = 0.0f;
		REQUIRE(u_cursor_depth_point_disparity(&hg.g, &c, &back));
		CHECK(back == Catch::Approx(d).margin(1e-5));
		u_cursor_depth_place(&hg.g, d, 0.03f, &cs, &hs);
		CHECK(std::fabs(cs.x - c.x) > 1e-3f);
	}
}

TEST_CASE("cursor_depth: filter_will_prime predicts exactly the steps that snap")
{
	const auto t = tuning();
	u_cursor_depth_filter f{};
	CHECK(u_cursor_depth_filter_will_prime(&f, &t, 1000 * MS));
	u_cursor_depth_filter_step(&f, &t, -0.3f, 1000 * MS);
	CHECK_FALSE(u_cursor_depth_filter_will_prime(&f, &t, 1016 * MS));
	CHECK_FALSE(u_cursor_depth_filter_will_prime(&f, &t, 1000 * MS)); // same frame
	CHECK(u_cursor_depth_filter_will_prime(&f, &t, 999 * MS));        // clock backwards
	CHECK(u_cursor_depth_filter_will_prime(&f, &t, 2000 * MS));       // stale gap
}


/*
 *
 * Phase 3a: the depth-layer source (spec v2).
 *
 */

namespace {

// Window depth an ordinary [0,1]-clip-depth perspective projection (D3D,
// Vulkan, Metal) writes for a point at view distance z.
float
window_depth_01(float n, float f, float z)
{
	return f / (f - n) * (1.0f - n / z);
}

// The same, through a GL [-1,1] NDC projection and the default glDepthRange.
float
window_depth_gl(float n, float f, float z)
{
	const float ndc = (f + n) / (f - n) - 2.0f * f * n / ((f - n) * z);
	return 0.5f * ndc + 0.5f;
}

// Project a locate-space point into a view: sub-image coords (origin top-left)
// and the view-space distance along -Z.
void
project(const u_cursor_depth_view &v, xrt_vec3 p, float &su, float &sv, float &z)
{
	const xrt_quat inv = {-v.pose.orientation.x, -v.pose.orientation.y, -v.pose.orientation.z,
	                      v.pose.orientation.w};
	const xrt_vec3 l = rotate(inv, {p.x - v.pose.position.x, p.y - v.pose.position.y, p.z - v.pose.position.z});
	z = -l.z;
	const float tx = l.x / z, ty = l.y / z;
	const float tl = std::tan(v.fov.angle_left), tr = std::tan(v.fov.angle_right);
	const float tu = std::tan(v.fov.angle_up), td = std::tan(v.fov.angle_down);
	su = (tx - tl) / (tr - tl);
	sv = (tu - ty) / (tu - td);
}

u_cursor_depth_patch_request
live_request()
{
	u_cursor_depth_patch_request r{};
	r.requested = true;
	r.u = 0.5f;
	r.v = 0.5f;
	r.radius_u = 0.02f;
	r.radius_v = 0.03f;
	return r;
}

} // namespace

TEST_CASE("cursor_depth source: a session that never asked does no compositor work (zero-cost gate)")
{
	// Every session's request starts zero-initialised; only a frame that
	// chained XrCursorDepthSourceDXR(SUBMITTED_DEPTH) sets `requested`.
	const u_cursor_depth_patch_request never{};
	CHECK_FALSE(u_cursor_depth_patch_should_sample(&never));
	CHECK_FALSE(u_cursor_depth_patch_should_sample(nullptr));

	u_cursor_depth_patch_request r = live_request();
	CHECK(u_cursor_depth_patch_should_sample(&r));

	r.requested = false; // a later frame without the request
	CHECK_FALSE(u_cursor_depth_patch_should_sample(&r));

	r = live_request();
	r.u = 1.2f; // cursor off the canvas
	CHECK_FALSE(u_cursor_depth_patch_should_sample(&r));
	r = live_request();
	r.v = std::nanf("");
	CHECK_FALSE(u_cursor_depth_patch_should_sample(&r));
	r = live_request();
	r.radius_u = 0.0f; // degenerate footprint
	CHECK_FALSE(u_cursor_depth_patch_should_sample(&r));
}

TEST_CASE("cursor_depth source: the patch is the footprint in the sub-image, clamped and capped")
{
	u_cursor_depth_patch_request r = live_request();
	int32_t x, y, w, h;
	// 756 x 822 tile at (756, 0): centre (1134, 411), half-size (15.12, 24.66).
	REQUIRE(u_cursor_depth_patch_rect(&r, 756, 0, 756, 822, &x, &y, &w, &h));
	CHECK(x == 1118);
	CHECK(y == 386);
	CHECK(x + w == 1150);
	CHECK(y + h == 436);

	// Cursor in the corner: clamped to the sub-image, never outside it.
	r.u = 0.0f;
	r.v = 1.0f;
	REQUIRE(u_cursor_depth_patch_rect(&r, 756, 0, 756, 822, &x, &y, &w, &h));
	CHECK(x == 756);
	CHECK(y + h == 822);
	CHECK(w > 0);
	CHECK(h > 0);

	// A huge footprint on a big tile is capped to the max patch side.
	r = live_request();
	r.radius_u = r.radius_v = 0.5f;
	REQUIRE(u_cursor_depth_patch_rect(&r, 0, 0, 3840, 2160, &x, &y, &w, &h));
	CHECK(w <= U_CURSOR_DEPTH_PATCH_MAX_DIM);
	CHECK(h <= U_CURSOR_DEPTH_PATCH_MAX_DIM);

	CHECK_FALSE(u_cursor_depth_patch_rect(&r, 0, 0, 0, 100, &x, &y, &w, &h));
}

TEST_CASE("cursor_depth source: the patch reduces to its nearest texel, either Z convention")
{
	// 3 x 2 patch inside rows of stride 4 (the 4th column is padding).
	const float nan = std::nanf("");
	const float texels[] = {0.9f, 0.7f, nan,   -5.0f, //
	                        0.8f, 0.6f, 0.95f, -5.0f};
	int32_t x = -1, y = -1;
	float raw = 0.0f;
	REQUIRE(u_cursor_depth_reduce_patch(texels, 3, 2, 4, false, &x, &y, &raw));
	CHECK(raw == 0.6f); // the padding's -5 is outside the patch
	CHECK(x == 1);
	CHECK(y == 1);
	REQUIRE(u_cursor_depth_reduce_patch(texels, 3, 2, 4, true, &x, &y, &raw));
	CHECK(raw == 0.95f);
	CHECK(x == 2);
	CHECK(y == 1);

	const float all_nan[] = {nan, nan};
	CHECK_FALSE(u_cursor_depth_reduce_patch(all_nan, 2, 1, 2, false, &x, &y, &raw));
	CHECK_FALSE(u_cursor_depth_reduce_patch(texels, 3, 2, 2, false, &x, &y, &raw)); // stride < w
}

TEST_CASE("cursor_depth source: window depth -> distance, ordinary / GL / reversed / infinite / remapped")
{
	const float n = 0.1f, f = 100.0f;
	float z = 0.0f;
	for (float zt : {0.15f, 0.5f, 0.65f, 3.0f, 40.0f}) {
		INFO("z = " << zt);
		const u_cursor_depth_layer_depth ord{0.0f, 1.0f, n, f};
		REQUIRE(u_cursor_depth_linear_depth(&ord, window_depth_01(n, f, zt), &z));
		CHECK(z == Catch::Approx(zt).epsilon(1e-3));

		// GL [-1,1] NDC lands on the same window hyperbola.
		REQUIRE(u_cursor_depth_linear_depth(&ord, window_depth_gl(n, f, zt), &z));
		CHECK(z == Catch::Approx(zt).epsilon(1e-3));

		// Reversed Z: nearZ > farZ - minDepth holds the far plane.
		const u_cursor_depth_layer_depth rev{0.0f, 1.0f, f, n};
		REQUIRE(u_cursor_depth_linear_depth(&rev, 1.0f - window_depth_01(n, f, zt), &z));
		CHECK(z == Catch::Approx(zt).epsilon(1e-3));

		// Infinite far plane: w = 1 - n/z.
		const u_cursor_depth_layer_depth inf{0.0f, 1.0f, n, INFINITY};
		REQUIRE(u_cursor_depth_linear_depth(&inf, 1.0f - n / zt, &z));
		CHECK(z == Catch::Approx(zt).epsilon(1e-3));

		// Reversed + infinite: w = n/z.
		const u_cursor_depth_layer_depth rinf{0.0f, 1.0f, INFINITY, n};
		REQUIRE(u_cursor_depth_linear_depth(&rinf, n / zt, &z));
		CHECK(z == Catch::Approx(zt).epsilon(1e-3));

		// minDepth/maxDepth remap: the swapchain holds [0.2, 0.8].
		const u_cursor_depth_layer_depth sub{0.2f, 0.8f, n, f};
		REQUIRE(u_cursor_depth_linear_depth(&sub, 0.2f + 0.6f * window_depth_01(n, f, zt), &z));
		CHECK(z == Catch::Approx(zt).epsilon(1e-3));
	}

	// The cleared background is not content, in either convention.
	const u_cursor_depth_layer_depth ord{0.0f, 1.0f, n, f};
	const u_cursor_depth_layer_depth rev{0.0f, 1.0f, f, n};
	CHECK_FALSE(u_cursor_depth_linear_depth(&ord, 1.0f, &z));
	CHECK_FALSE(u_cursor_depth_linear_depth(&rev, 0.0f, &z));
	const u_cursor_depth_layer_depth inf{0.0f, 1.0f, n, INFINITY};
	CHECK_FALSE(u_cursor_depth_linear_depth(&inf, 1.0f, &z));

	// Out of range and degenerate ranges never produce a distance.
	CHECK_FALSE(u_cursor_depth_linear_depth(&ord, 1.5f, &z));
	CHECK_FALSE(u_cursor_depth_linear_depth(&ord, -0.5f, &z));
	CHECK_FALSE(u_cursor_depth_linear_depth(&ord, std::nanf(""), &z));
	const u_cursor_depth_layer_depth flat{0.5f, 0.5f, n, f};
	CHECK_FALSE(u_cursor_depth_linear_depth(&flat, 0.5f, &z));
	const u_cursor_depth_layer_depth same{0.0f, 1.0f, n, n};
	CHECK_FALSE(u_cursor_depth_linear_depth(&same, 0.5f, &z));
	const u_cursor_depth_layer_depth neg{0.0f, 1.0f, -n, f};
	CHECK_FALSE(u_cursor_depth_linear_depth(&neg, 0.5f, &z));
}

TEST_CASE("cursor_depth source: a depth texel unprojects to the point it shows, in any rig")
{
	Xform x;
	x.q = {0.0f, std::sin(0.3f), 0.0f, std::cos(0.3f)}; // yaw 0.6 rad
	x.t = {1.0f, -2.0f, 0.5f};
	x.s = 3.0f;
	for (const Xform &xf : {Xform{}, x}) {
		const u_cursor_depth_view v = kooima({-0.032f, 0.01f, 0.5f}, xf);
		const float n = 0.01f * xf.s, f = 100.0f * xf.s;
		for (xrt_vec3 local :
		     {xrt_vec3{0.0f, 0.0f, 0.05f}, xrt_vec3{0.05f, -0.03f, 0.1f}, xrt_vec3{-0.1f, 0.06f, -0.08f}}) {
			const xrt_vec3 p = apply(xf, local);
			float su, sv, z;
			project(v, p, su, sv, z);
			xrt_vec3 got{};
			const u_cursor_depth_layer_depth d{0.0f, 1.0f, n, f};
			REQUIRE(u_cursor_depth_point_from_depth_sample(&v, &d, su, sv, window_depth_01(n, f, z), &got));
			require_near(got, p, 2e-4f * xf.s);
		}
	}
}

TEST_CASE("cursor_depth source: the depth-layer point gives the same disparity as the app's own point")
{
	// The whole point of Phase 3a: feeding the unprojected nearest texel into
	// the v1 path places the cursor exactly where the app's hit test would.
	const u_cursor_depth_view a = kooima({-0.032f, 0.0f, 0.5f});
	const u_cursor_depth_view b = kooima({0.032f, 0.0f, 0.5f});
	u_cursor_depth_geometry g{};
	REQUIRE(u_cursor_depth_geometry_solve(&a, &b, 0.5f, 0.5f, &g));

	const xrt_vec3 p{0.0f, 0.0f, 0.06f}; // 6 cm in front of the canvas
	float d_app = 0.0f;
	REQUIRE(u_cursor_depth_point_disparity(&g, &p, &d_app));

	for (const u_cursor_depth_view *v : {&a, &b}) {
		float su, sv, z;
		project(*v, p, su, sv, z);
		const u_cursor_depth_layer_depth d{0.0f, 1.0f, 0.01f, 100.0f};
		xrt_vec3 q{};
		REQUIRE(u_cursor_depth_point_from_depth_sample(v, &d, su, sv, window_depth_01(0.01f, 100.0f, z), &q));
		float d_layer = 0.0f;
		REQUIRE(u_cursor_depth_point_disparity(&g, &q, &d_layer));
		CHECK(d_layer == Catch::Approx(d_app).margin(1e-4));
	}
}


/*
 *
 * Spec v5: the runtime draws the cursor (ADR-046 Amendment 2 B).
 *
 */

TEST_CASE("cursor_draw: zero cost - a never-requested request never draws")
{
	// The compositor-side gate. Every session that never chained
	// XrCursorDepthDrawInfoDXR hands the compositor exactly this.
	const u_cursor_draw_request none{};
	CHECK_FALSE(u_cursor_draw_should_draw(&none));
	CHECK_FALSE(u_cursor_draw_should_draw(nullptr));

	u_cursor_draw_request req{};
	req.requested = true;
	req.view_count = 1; // 2D: one view, nothing to lift
	CHECK_FALSE(u_cursor_draw_should_draw(&req));
	req.view_count = 2;
	CHECK(u_cursor_draw_should_draw(&req));
	req.view_count = XRT_MAX_VIEWS + 1;
	CHECK_FALSE(u_cursor_draw_should_draw(&req));
	req.view_count = 2;
	req.view[1].axis_y[1] = NAN;
	CHECK_FALSE(u_cursor_draw_should_draw(&req));
	req.view[1].axis_y[1] = 0.0f;
	req.requested = false;
	CHECK_FALSE(u_cursor_draw_should_draw(&req));
}

TEST_CASE("cursor_draw: a canvas point projects to its canvas UV in every view, in any rig")
{
	Xform x;
	x.q = {0.0f, std::sin(0.3f), 0.0f, std::cos(0.3f)};
	x.t = {1.0f, -2.0f, 0.5f};
	x.s = 12.5f;
	for (const Xform &xf : {Xform{}, x}) {
		for (xrt_vec3 eye : {xrt_vec3{-0.032f, 0.0f, 0.6f}, xrt_vec3{0.08f, 0.03f, 0.55f}}) {
			const u_cursor_depth_view v = kooima(eye, xf);
			for (float u : {0.0f, 0.3f, 1.0f}) {
				for (float vv : {0.0f, 0.7f, 1.0f}) {
					const xrt_vec3 s = apply(xf, {-W / 2 + u * W, H / 2 - vv * H, 0.0f});
					float xy[2];
					REQUIRE(u_cursor_draw_project(&v, &s, xy));
					CHECK(xy[0] == Catch::Approx(u).margin(1e-4));
					CHECK(xy[1] == Catch::Approx(vv).margin(1e-4));
				}
			}
			// Behind the eye: no projection.
			const xrt_vec3 behind = apply(xf, {eye.x, eye.y, eye.z + 0.1f});
			float xy[2];
			CHECK_FALSE(u_cursor_draw_project(&v, &behind, xy));
		}
	}
}

TEST_CASE("cursor_draw: the sprite lands at the placement's disparity, constant size, hotspot on the point")
{
	const xrt_vec3 el{-0.032f, 0.0f, 0.6f};
	const xrt_vec3 er{0.032f, 0.0f, 0.6f};
	const u_cursor_depth_view a = kooima(el);
	const u_cursor_depth_view b = kooima(er);
	u_cursor_depth_geometry g{};
	REQUIRE(u_cursor_depth_geometry_solve(&a, &b, 0.5f, 0.5f, &g));

	for (float disparity : {0.0f, -0.1f, -0.4f}) {
		xrt_vec3 c{};
		float h = 0.0f;
		u_cursor_depth_place(&g, disparity, 0.05f, &c, &h);
		const float t = 1.0f / (1.0f - disparity);
		const float unit = g.canvas_height * t; // what the state tracker hands over

		u_cursor_draw_view va{}, vb{};
		REQUIRE(u_cursor_draw_view_from_placement(&a, &c, &g.orientation, unit, &va));
		REQUIRE(u_cursor_draw_view_from_placement(&b, &c, &g.orientation, unit, &vb));

		// Each eye sees the hotspot where its line through C crosses the glass.
		const xrt_vec3 gl = on_glass(el, c), gr = on_glass(er, c);
		CHECK(va.origin[0] == Catch::Approx((gl.x + W / 2) / W).margin(1e-4));
		CHECK(vb.origin[0] == Catch::Approx((gr.x + W / 2) / W).margin(1e-4));
		// On-screen disparity = baseline x d (0.064 m x d), in tile widths.
		CHECK((vb.origin[0] - va.origin[0]) * W == Catch::Approx(0.064f * disparity).margin(1e-5));
		CHECK(va.origin[1] == Catch::Approx(vb.origin[1]).margin(1e-5));

		// One unit (the t-scaled canvas height) spans exactly one tile height
		// on the glass at every depth: constant apparent size.
		CHECK(-va.axis_y[1] == Catch::Approx(1.0f).margin(1e-4));
		CHECK(va.axis_y[0] == Catch::Approx(0.0f).margin(1e-5));
		CHECK(va.axis_x[0] * W == Catch::Approx(H).margin(1e-5)); // square pixels: 1 unit wide = H metres
		CHECK(va.axis_x[1] == Catch::Approx(0.0f).margin(1e-5));

		// Hotspot top-left (an arrow's tip): corner 0 is the origin; the quad
		// hangs right and down from it.
		float xy[4][2];
		u_cursor_draw_corners(&va, 0.05f, 0.6f, 0.0f, 0.0f, xy);
		CHECK(xy[0][0] == Catch::Approx(va.origin[0]).margin(1e-6));
		CHECK(xy[0][1] == Catch::Approx(va.origin[1]).margin(1e-6));
		CHECK(xy[1][1] - xy[0][1] == Catch::Approx(0.05f).margin(1e-5));                  // bottom-left below
		CHECK((xy[2][0] - xy[0][0]) * W == Catch::Approx(0.05f * 0.6f * H).margin(1e-6)); // top-right
		// Centred hotspot: the quad's centre is the origin.
		u_cursor_draw_corners(&vb, 0.05f, 1.0f, 0.5f, 0.5f, xy);
		CHECK((xy[0][0] + xy[3][0]) * 0.5f == Catch::Approx(vb.origin[0]).margin(1e-6));
		CHECK((xy[0][1] + xy[3][1]) * 0.5f == Catch::Approx(vb.origin[1]).margin(1e-6));
	}
}

TEST_CASE("cursor_draw: the per-view map is the same under any rigid transform plus scale")
{
	Xform x;
	x.q = {0.1f, std::sin(0.4f), 0.05f, std::cos(0.4f)};
	const float n = std::sqrt(x.q.x * x.q.x + x.q.y * x.q.y + x.q.z * x.q.z + x.q.w * x.q.w);
	x.q = {x.q.x / n, x.q.y / n, x.q.z / n, x.q.w / n};
	x.t = {-3.0f, 1.0f, 2.0f};
	x.s = 12.5f;

	const xrt_vec3 el{0.02f, 0.03f, 0.55f}, er{0.085f, 0.035f, 0.56f};
	u_cursor_draw_view ref[2]{}, got[2]{};
	for (int pass = 0; pass < 2; pass++) {
		const Xform xf = pass == 0 ? Xform{} : x;
		const u_cursor_depth_view a = kooima(el, xf);
		const u_cursor_depth_view b = kooima(er, xf);
		u_cursor_depth_geometry g{};
		REQUIRE(u_cursor_depth_geometry_solve(&a, &b, 0.3f, 0.6f, &g));
		xrt_vec3 c{};
		float h = 0.0f;
		u_cursor_depth_place(&g, -0.2f, 0.03f, &c, &h);
		const float unit = g.canvas_height / (1.0f + 0.2f);
		u_cursor_draw_view *out = pass == 0 ? ref : got;
		REQUIRE(u_cursor_draw_view_from_placement(&a, &c, &g.orientation, unit, &out[0]));
		REQUIRE(u_cursor_draw_view_from_placement(&b, &c, &g.orientation, unit, &out[1]));
	}
	for (int i = 0; i < 2; i++) {
		for (int k = 0; k < 2; k++) {
			CHECK(got[i].origin[k] == Catch::Approx(ref[i].origin[k]).margin(2e-4));
			CHECK(got[i].axis_x[k] == Catch::Approx(ref[i].axis_x[k]).margin(2e-4));
			CHECK(got[i].axis_y[k] == Catch::Approx(ref[i].axis_y[k]).margin(2e-4));
		}
	}
}

TEST_CASE("cursor_draw: the classic arrow - dark outline, white fill, tip at the top-left, premultiplied")
{
	const uint32_t h = 76, w = 48;
	std::vector<uint8_t> img((size_t)w * h * 4, 0xAB);
	u_cursor_draw_arrow_rgba(img.data(), w, h);
	auto px = [&](uint32_t x, uint32_t y) { return &img[((size_t)y * w + x) * 4]; };

	// Premultiplied: no channel ever exceeds alpha.
	for (size_t i = 0; i < img.size(); i += 4) {
		REQUIRE(img[i + 0] <= img[i + 3]);
		REQUIRE(img[i + 1] <= img[i + 3]);
		REQUIRE(img[i + 2] <= img[i + 3]);
	}
	// The tip (arrow px 0.1, 0.5) is outline: opaque and dark.
	CHECK(px(0, 2)[3] == 255);
	CHECK(px(0, 2)[0] < 40);
	// Inside the head (arrow px 2, 9): white fill.
	CHECK(px(9, 36)[3] == 255);
	CHECK(px(9, 36)[0] == 255);
	// Right of the head, above the wing (arrow px 10, 3): empty.
	CHECK(px(40, 12)[3] == 0);
	// Below the tail (arrow px 1, 18.5): empty.
	CHECK(px(4, 74)[3] == 0);
}

/*
 * #1949: display zones. A zone-scoped locate frames its views to the zone rect
 * (the rect IS the canvas), so the hint's cursorUV, the placement's unit and
 * the per-view map are all ZONE-normalised. The compositor re-expresses the
 * map in its tile with u_cursor_draw_into_zone(), exactly the way it places the
 * zone's own layer (zone rect scaled by tile / window).
 */
namespace {

// A window WIN_PX_W x WIN_PX_H px spanning the W x H canvas, and a zone in it.
constexpr uint32_t WIN_PX_W = 1500;
constexpr uint32_t WIN_PX_H = 1000;

// Kooima view from a display-space eye onto the sub-rect (px, window origin
// top-left) of the W x H canvas centred at the origin in the z = 0 plane.
u_cursor_depth_view
kooima_rect(xrt_vec3 eye, int32_t x, int32_t y, int32_t w, int32_t h)
{
	const float mx = W / (float)WIN_PX_W, my = H / (float)WIN_PX_H;
	const float l = -W / 2 + (float)x * mx, r = -W / 2 + (float)(x + w) * mx;
	const float t = H / 2 - (float)y * my, b = H / 2 - (float)(y + h) * my;
	u_cursor_depth_view v{};
	const float d = eye.z;
	v.fov.angle_left = std::atan((l - eye.x) / d);
	v.fov.angle_right = std::atan((r - eye.x) / d);
	v.fov.angle_up = std::atan((t - eye.y) / d);
	v.fov.angle_down = std::atan((b - eye.y) / d);
	v.pose.orientation = {0, 0, 0, 1};
	v.pose.position = eye;
	return v;
}

} // namespace

TEST_CASE("cursor_draw: a zone request maps into the zone's sub-rect of every tile, N views")
{
	// Off-centre zone, four eyes in a row (an N-view zone), viewer off-axis.
	const int32_t zx = 300, zy = 200, zw = 600, zh = 500;
	const xrt_vec3 eyes[4] = {
	    {-0.07f, 0.02f, 0.6f}, {-0.025f, 0.02f, 0.6f}, {0.025f, 0.02f, 0.61f}, {0.07f, 0.02f, 0.6f}};
	u_cursor_depth_view zone_v[4], full_v[4];
	for (int i = 0; i < 4; i++) {
		zone_v[i] = kooima_rect(eyes[i], zx, zy, zw, zh);
		full_v[i] = kooima_rect(eyes[i], 0, 0, (int32_t)WIN_PX_W, (int32_t)WIN_PX_H);
	}

	// The hint: cursor at zone UV (0.3, 0.6) - from the zone locate's
	// outermost pair, as the state tracker solves it.
	u_cursor_depth_geometry g{};
	REQUIRE(u_cursor_depth_geometry_solve(&zone_v[0], &zone_v[3], 0.3f, 0.6f, &g));
	CHECK(g.canvas_height == Catch::Approx(H * (float)zh / (float)WIN_PX_H).margin(1e-5)); // the ZONE's height

	for (float disparity : {0.0f, -0.25f}) {
		xrt_vec3 c{};
		float h = 0.0f;
		u_cursor_depth_place(&g, disparity, 0.05f, &c, &h);
		const float unit = g.canvas_height / (1.0f - disparity);

		u_cursor_draw_request req{};
		req.requested = true;
		req.view_count = 4;
		for (int i = 0; i < 4; i++) {
			REQUIRE(u_cursor_draw_view_from_placement(&zone_v[i], &c, &g.orientation, unit, &req.view[i]));
		}
		req.zone_valid = true;
		req.zone_id = 7;
		req.zone_rect = {{zx, zy}, {zw, zh}};
		REQUIRE(u_cursor_draw_should_draw(&req));

		float clip[4];
		REQUIRE(u_cursor_draw_into_zone(&req, WIN_PX_W, WIN_PX_H, clip));
		CHECK(clip[0] == Catch::Approx(300.0f / 1500.0f));
		CHECK(clip[1] == Catch::Approx(200.0f / 1000.0f));
		CHECK(clip[2] == Catch::Approx(600.0f / 1500.0f));
		CHECK(clip[3] == Catch::Approx(500.0f / 1000.0f));

		// The oracle: the same 3D point and unit seen through views framed to
		// the WHOLE window land exactly where the remapped zone map says, in
		// every view - the tile is the window in a zones frame.
		for (int i = 0; i < 4; i++) {
			u_cursor_draw_view ref{};
			REQUIRE(u_cursor_draw_view_from_placement(&full_v[i], &c, &g.orientation, unit, &ref));
			for (int k = 0; k < 2; k++) {
				CHECK(req.view[i].origin[k] == Catch::Approx(ref.origin[k]).margin(1e-4));
				CHECK(req.view[i].axis_x[k] == Catch::Approx(ref.axis_x[k]).margin(1e-4));
				CHECK(req.view[i].axis_y[k] == Catch::Approx(ref.axis_y[k]).margin(1e-4));
			}
		}
		// On the plane, the hotspot sits at the zone UV inside the zone.
		if (disparity == 0.0f) {
			for (int i = 0; i < 4; i++) {
				CHECK(req.view[i].origin[0] ==
				      Catch::Approx((300.0f + 0.3f * 600.0f) / 1500.0f).margin(1e-4));
				CHECK(req.view[i].origin[1] ==
				      Catch::Approx((200.0f + 0.6f * 500.0f) / 1000.0f).margin(1e-4));
			}
		}
		// One unit is the zone's height: the canvas the compositor sizes
		// the sprite against is the zone's, in the same px.
		CHECK(u_cursor_draw_canvas_px_h(&req, WIN_PX_H) == (uint32_t)zh);
	}
}

TEST_CASE("cursor_draw: zone clip and refusals")
{
	u_cursor_draw_request req{};
	req.requested = true;
	req.view_count = 2;
	req.view[0] = {{0.5f, 0.5f}, {0.1f, 0.0f}, {0.0f, -0.2f}};
	req.view[1] = req.view[0];
	float clip[4];

	// No zone: untouched, whole tile, the caller's canvas height.
	u_cursor_draw_request plain = req;
	REQUIRE(u_cursor_draw_into_zone(&plain, 0, 0, clip));
	CHECK(clip[0] == 0.0f);
	CHECK(clip[1] == 0.0f);
	CHECK(clip[2] == 1.0f);
	CHECK(clip[3] == 1.0f);
	CHECK(plain.view[0].origin[0] == 0.5f);
	CHECK(u_cursor_draw_canvas_px_h(&plain, 900) == 900u);

	// A zone hanging off the window's right/bottom edge clips to the tile;
	// the map itself is not clamped (the sprite stays where it belongs).
	req.zone_valid = true;
	req.zone_rect = {{1000, 800}, {1000, 400}};
	u_cursor_draw_request off = req;
	REQUIRE(u_cursor_draw_into_zone(&off, WIN_PX_W, WIN_PX_H, clip));
	CHECK(clip[0] == Catch::Approx(1000.0f / 1500.0f));
	CHECK(clip[2] == Catch::Approx(500.0f / 1500.0f));
	CHECK(clip[1] == Catch::Approx(0.8f));
	CHECK(clip[3] == Catch::Approx(0.2f));
	CHECK(off.view[1].origin[0] == Catch::Approx((1000.0f + 0.5f * 1000.0f) / 1500.0f));
	CHECK(off.view[1].axis_y[1] == Catch::Approx(-0.2f * 400.0f / 1000.0f));

	// No target size, an empty zone, or a zone wholly outside the window:
	// nothing is drawn.
	u_cursor_draw_request r2 = req;
	CHECK_FALSE(u_cursor_draw_into_zone(&r2, 0, WIN_PX_H, clip));
	r2 = req;
	r2.zone_rect = {{10, 10}, {0, 50}};
	CHECK_FALSE(u_cursor_draw_into_zone(&r2, WIN_PX_W, WIN_PX_H, clip));
	r2 = req;
	r2.zone_rect = {{1600, 10}, {100, 50}};
	CHECK_FALSE(u_cursor_draw_into_zone(&r2, WIN_PX_W, WIN_PX_H, clip));
}


/*
 *
 * The willDraw contract: report true only for a placement that draws.
 *
 */

namespace {

// A camera rig: the display-space scene (eye at `eye_display`, canvas at z = 0)
// scaled by `scale` about the eye and carried so the eye sits at `at` - the
// canvas is then `scale * eye_display.z` in front of it, which is what a
// re-converging camera rig changes from frame to frame.
Xform
camera_rig(xrt_vec3 eye_display, float scale, xrt_vec3 at)
{
	Xform x;
	x.s = scale;
	x.t = {at.x - scale * eye_display.x, at.y - scale * eye_display.y, at.z - scale * eye_display.z};
	return x;
}

struct RigPair
{
	u_cursor_depth_view v[2];
	u_cursor_depth_geometry g;
};

RigPair
rig_pair(const Xform &x, float u, float v, float head_x = 0.0f)
{
	RigPair r{};
	r.v[0] = kooima({head_x - 0.03f, 0.1f, 0.6f}, x);
	r.v[1] = kooima({head_x + 0.03f, 0.1f, 0.6f}, x);
	REQUIRE(u_cursor_depth_geometry_solve(&r.v[0], &r.v[1], u, v, &r.g));
	return r;
}

float
unit_for(const u_cursor_depth_geometry &g, float d)
{
	return g.canvas_height / (1.0f - d);
}

} // namespace

TEST_CASE("cursor_depth: HYBRID - a camera rig re-converging under a still pointer re-anchors, C stays in front")
{
	// EarthView's horizon framing: the anchor is taken while the camera rig's
	// canvas is 1 m from the eye; the rig then re-converges to 3.42 m with the
	// pointer still. The anchor-time line of sight belongs to the old canvas:
	// s = 1 - (1 - t) e / e0 = 1 - 0.375 * 3.42 < 0 put C BEHIND the eye, and
	// willDraw said true over an atlas with no cursor in it.
	const float u = 0.58f, v = 0.75f, d = -0.6f;
	const xrt_vec3 eye_at{0.0f, 0.092f, 0.0f};
	u_cursor_depth_anchor anchor{};
	float hgt = 0.0f;

	const RigPair near_rig = rig_pair(camera_rig({0.0f, 0.1f, 0.6f}, 1.0f / 0.6f, eye_at), u, v);
	REQUIRE(near_rig.g.eye_to_canvas == Catch::Approx(1.0f));
	xrt_vec3 c{};
	u_cursor_depth_place_anchored(&near_rig.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false,
	                              16 * MS, &c, &hgt);
	REQUIRE(anchor.has_anchor);

	const RigPair far_rig = rig_pair(camera_rig({0.0f, 0.1f, 0.6f}, 3.42f / 0.6f, eye_at), u, v);
	REQUIRE(far_rig.g.eye_to_canvas == Catch::Approx(3.42f));
	u_cursor_depth_place_anchored(&far_rig.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false, 32 * MS,
	                              &c, &hgt);

	// Re-anchored on the new canvas: the SCREEN placement, in front of the eye...
	xrt_vec3 cs{};
	float hs = 0.0f;
	u_cursor_depth_place(&far_rig.g, d, 0.03f, &cs, &hs);
	require_near(c, cs, 1e-5f);
	CHECK(anchor.eye_to_canvas == Catch::Approx(3.42f));
	float back = 0.0f;
	REQUIRE(u_cursor_depth_point_disparity(&far_rig.g, &c, &back));
	CHECK(back == Catch::Approx(d).margin(1e-4));
	// ... and it draws.
	CHECK(u_cursor_draw_placement_drawable(far_rig.v, 2, &c, &far_rig.g.orientation, unit_for(far_rig.g, d),
	                                       nullptr));

	// A milder re-convergence (3.42 m -> 5 m) keeps s > 0, so only the plane
	// test catches it: without it C would stay on the old line of sight at
	// the wrong depth. Re-anchored, it is at the requested disparity.
	const RigPair farther = rig_pair(camera_rig({0.0f, 0.1f, 0.6f}, 5.0f / 0.6f, eye_at), u, v);
	xrt_vec3 c5{};
	u_cursor_depth_place_anchored(&farther.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false, 40 * MS,
	                              &c5, &hgt);
	REQUIRE(u_cursor_depth_point_disparity(&farther.g, &c5, &back));
	CHECK(back == Catch::Approx(d).margin(1e-4));
	CHECK(anchor.eye_to_canvas == Catch::Approx(5.0f));

	// Back to 3.42 m (re-anchors again), then a head move keeps the canvas
	// plane: world-fixed, no re-anchor.
	u_cursor_depth_place_anchored(&far_rig.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false, 44 * MS,
	                              &c, &hgt);
	const uint64_t anchored_at = anchor.last_ns;
	CHECK(anchored_at == 44 * MS);
	const RigPair head_moved =
	    rig_pair(camera_rig({0.0f, 0.1f, 0.6f}, 3.42f / 0.6f, eye_at), u, v, /*head_x=*/0.05f);
	xrt_vec3 c2{};
	u_cursor_depth_place_anchored(&head_moved.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false,
	                              48 * MS, &c2, &hgt);
	require_near(c2, c, 1e-4f);
	CHECK(anchor.last_ns == anchored_at);
}

TEST_CASE("cursor_depth: HYBRID - never places C on or behind the eye (head far back re-anchors)")
{
	const float u = 0.3f, v = 0.4f, d = -0.6f;
	u_cursor_depth_anchor anchor{};
	float hgt = 0.0f;
	xrt_vec3 c{};
	const HeadGeom h0 = head({0.0f, 0.0f, 0.3f}, u, v);
	u_cursor_depth_place_anchored(&h0.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false, 16 * MS, &c,
	                              &hgt);
	// Same canvas plane, head 4x farther back: lift = 0.375 * 1.2 > e0 = 0.3.
	const HeadGeom h1 = head({0.0f, 0.0f, 1.2f}, u, v);
	u_cursor_depth_place_anchored(&h1.g, d, 0.03f, U_CURSOR_DEPTH_ANCHOR_HYBRID, &anchor, u, v, false, 32 * MS, &c,
	                              &hgt);
	xrt_vec3 cs{};
	float hs = 0.0f;
	u_cursor_depth_place(&h1.g, d, 0.03f, &cs, &hs);
	require_near(c, cs, 1e-5f);
	CHECK(c.z > 0.0f);
	CHECK(c.z < 1.2f);
}

TEST_CASE("cursor_draw: drawable predicate - large-scale camera rig, far from the origin")
{
	// m2v in the thousands and a rig parked 1e5 m out: floats still resolve it.
	const float u = 0.58f, v = 0.75f, d = -0.6f;
	const Xform x = camera_rig({0.0f, 0.1f, 0.6f}, 5000.0f, {1.0e5f, 2.0e4f, -3.0e4f});
	const RigPair rp = rig_pair(x, u, v);
	xrt_vec3 c{};
	float hgt = 0.0f;
	u_cursor_depth_place(&rp.g, d, 0.03f, &c, &hgt);
	const float unit = unit_for(rp.g, d);

	u_cursor_draw_view out[2]{};
	REQUIRE(u_cursor_draw_placement_drawable(rp.v, 2, &c, &rp.g.orientation, unit, out));
	// The cyclopean projection of C is the pointer: the two views straddle it.
	CHECK(0.5f * (out[0].origin[0] + out[1].origin[0]) == Catch::Approx(u).margin(2e-3));
	CHECK(out[0].origin[1] == Catch::Approx(v).margin(2e-3));
	CHECK(out[0].origin[0] > out[1].origin[0]); // crossed: in front of the glass
	// One unit (the t-scaled canvas height) at depth t spans one tile height.
	CHECK(std::fabs(out[0].axis_y[1]) == Catch::Approx(1.0f).margin(2e-3));

	// The placement behind the eye (what the stale HYBRID anchor produced).
	const xrt_vec3 eye = rp.g.eye;
	const xrt_vec3 behind = {eye.x + (eye.x - c.x) * 0.2f, eye.y + (eye.y - c.y) * 0.2f,
	                         eye.z + (eye.z - c.z) * 0.2f};
	CHECK_FALSE(u_cursor_draw_placement_drawable(rp.v, 2, &behind, &rp.g.orientation, unit, nullptr));
	// In front, but outside every view's frustum (far off to the side).
	const xrt_vec3 aside = {c.x + 50.0f * rp.g.canvas_height, c.y, c.z};
	CHECK_FALSE(u_cursor_draw_placement_drawable(rp.v, 2, &aside, &rp.g.orientation, unit, nullptr));
	// Degenerate size, too few views, non-finite.
	CHECK_FALSE(u_cursor_draw_placement_drawable(rp.v, 2, &c, &rp.g.orientation, 0.0f, nullptr));
	CHECK_FALSE(u_cursor_draw_placement_drawable(rp.v, 2, &c, &rp.g.orientation, 1e-12f, nullptr));
	CHECK_FALSE(u_cursor_draw_placement_drawable(rp.v, 1, &c, &rp.g.orientation, unit, nullptr));
	const xrt_vec3 nan_c = {NAN, c.y, c.z};
	CHECK_FALSE(u_cursor_draw_placement_drawable(rp.v, 2, &nan_c, &rp.g.orientation, unit, nullptr));

	// Behind ONE eye only (unequal eye depths): visible in the other view's
	// tile, still not drawable - the request would be partial.
	const u_cursor_depth_view uneq[2] = {kooima({-0.03f, 0.0f, 0.6f}), kooima({0.03f, 0.0f, 0.4f})};
	const xrt_vec3 between = {-0.03f, 0.0f, 0.5f};
	u_cursor_draw_view one{};
	REQUIRE(u_cursor_draw_view_from_placement(&uneq[0], &between, &rp.g.orientation, 0.1f, &one));
	CHECK(one.origin[0] == Catch::Approx(0.4f).margin(1e-4));
	CHECK_FALSE(u_cursor_draw_placement_drawable(uneq, 2, &between, &rp.g.orientation, 0.1f, nullptr));
}

TEST_CASE("cursor_draw: drawable predicate - the hotspot in ONE tile is enough (edge of the window)")
{
	// At the right edge, crossed disparity shifts the left view's hotspot past
	// x = 1 while the right view still shows it: that frame draws.
	const RigPair rp = rig_pair(Xform{}, 0.999f, 0.5f);
	const float d = -0.6f;
	xrt_vec3 c{};
	float hgt = 0.0f;
	u_cursor_depth_place(&rp.g, d, 0.03f, &c, &hgt);
	u_cursor_draw_view out[2]{};
	REQUIRE(u_cursor_draw_placement_drawable(rp.v, 2, &c, &rp.g.orientation, unit_for(rp.g, d), out));
	CHECK(out[0].origin[0] > 1.0f);
	CHECK(out[1].origin[0] <= 1.0f);
}

// One pixel of a cursor whose colour is c and coverage a, composited by "the
// OS" over black and over white, exactly as DrawIconEx does.
static void
os_draw(const uint8_t c[3], float a, uint8_t on_black[4], uint8_t on_white[4])
{
	for (int k = 0; k < 3; k++) {
		on_black[k] = (uint8_t)lrintf(c[k] * a);
		on_white[k] = (uint8_t)lrintf(c[k] * a + 255.0f * (1.0f - a));
	}
	on_black[3] = on_white[3] = 0; // GDI leaves byte 3 undefined: ignored
}

TEST_CASE("cursor_draw: unblend recovers premultiplied colour and alpha from the black/white pair")
{
	const uint8_t white[3] = {255, 255, 255};
	const uint8_t orange[3] = {250, 120, 10};
	const float alphas[] = {0.0f, 0.25f, 0.5f, 0.8f, 1.0f};
	for (const uint8_t *c : {white, orange}) {
		for (float a : alphas) {
			uint8_t b[4], w[4], out[4];
			os_draw(c, a, b, w);
			u_cursor_draw_unblend(b, w, 1, out);
			CAPTURE(a, (int)c[0]);
			CHECK(std::abs((int)out[3] - (int)lrintf(255.0f * a)) <= 1);
			for (int k = 0; k < 3; k++) {
				// PREMULTIPLIED: colour x coverage, never above alpha.
				CHECK(std::abs((int)out[k] - (int)lrintf(c[k] * a)) <= 1);
				CHECK(out[k] <= out[3]);
			}
			if (a >= 0.25f) {
				// ...and it un-premultiplies back to the colour drawn.
				for (int k = 0; k < 3; k++) {
					const int straight = (int)lrintf(255.0f * out[k] / out[3]);
					CHECK(std::abs(straight - (int)c[k]) <= (a < 0.5f ? 4 : 2));
				}
			} else if (a == 0.0f) {
				CHECK(out[0] == 0); // transparent = zero, never garbage
			}
		}
	}
}

TEST_CASE("cursor_draw: unblend - monochrome AND/XOR cursors, inverted pixels opaque white")
{
	// AND=0 XOR=0: black. AND=0 XOR=1: white. AND=1 XOR=0: transparent.
	// AND=1 XOR=1: inverts the screen (white over black, black over white).
	const uint8_t black_b[4] = {0, 0, 0, 0}, black_w[4] = {0, 0, 0, 0};
	const uint8_t white_b[4] = {255, 255, 255, 0}, white_w[4] = {255, 255, 255, 0};
	const uint8_t clear_b[4] = {0, 0, 0, 0}, clear_w[4] = {255, 255, 255, 0};
	const uint8_t inv_b[4] = {255, 255, 255, 0}, inv_w[4] = {0, 0, 0, 0};
	uint8_t b[16], w[16], out[16];
	memcpy(b + 0, black_b, 4), memcpy(w + 0, black_w, 4);
	memcpy(b + 4, white_b, 4), memcpy(w + 4, white_w, 4);
	memcpy(b + 8, clear_b, 4), memcpy(w + 8, clear_w, 4);
	memcpy(b + 12, inv_b, 4), memcpy(w + 12, inv_w, 4);
	u_cursor_draw_unblend(b, w, 4, out);
	CHECK((out[0] == 0 && out[3] == 255));                    // opaque black
	CHECK((out[4] == 255 && out[5] == 255 && out[7] == 255)); // opaque white
	CHECK(out[11] == 0);                                      // transparent
	CHECK((out[12] == 255 && out[15] == 255));                // invert -> opaque white

	// MUTANT: taking alpha from GDI's byte 3 (undefined for these cursors)
	// would make every one of them invisible - byte 3 is 0 in every input.
	CHECK(out[3] == 255);
}
