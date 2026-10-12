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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
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


/*
 *
 * #1952: compositor refusals make willDraw false.
 *
 */

namespace {

/*!
 * The state tracker's per-frame flow, reduced to the latch: a locate reports
 * willDraw from the latch, xrEndFrame hands a request (report-only when the
 * locate said false), the compositor answers, the answer is folded in.
 */
struct LatchSim
{
	u_cursor_draw_latch latch{};
	u_cursor_draw_report report{};
	int warns = 0;

	//! One frame. @p refuse: what the compositor will do (NONE = draw).
	//! Returns {willDraw reported by this frame's locate, sprite drawn}.
	std::pair<bool, bool>
	frame(u_cursor_draw_refusal refuse)
	{
		const bool will_draw = u_cursor_draw_latch_allows(&latch);

		u_cursor_draw_request req{};
		req.requested = true;
		req.report_only = !will_draw;
		u_cursor_draw_report_begin(&report);
		req.report = &report;

		// The compositor.
		bool drew_pixels = false;
		if (refuse != U_CURSOR_DRAW_REFUSAL_NONE) {
			if (refuse != U_CURSOR_DRAW_REFUSAL_NOT_REACHED) { // NOT_REACHED: says nothing
				u_cursor_draw_report_refused(&req, refuse);
			}
		} else {
			drew_pixels = !req.report_only;
			u_cursor_draw_report_drew(&req);
		}

		if (u_cursor_draw_latch_record(&latch, &report)) {
			warns++;
		}
		return {will_draw, drew_pixels};
	}
};

} // namespace

TEST_CASE("cursor_draw: refusal latch - a refusal makes the NEXT willDraw false, a draw restores it")
{
	LatchSim s;
	// A fresh latch allows (no request ever = nothing changes).
	CHECK(u_cursor_draw_latch_allows(&s.latch));

	auto f0 = s.frame(U_CURSOR_DRAW_REFUSAL_NONE);
	CHECK(f0.first);
	CHECK(f0.second);

	// The compositor refuses: THIS frame's locate already said true (the
	// one-frame lag is inherent - the refusal is only known at the commit),
	// the next one says false.
	auto f1 = s.frame(U_CURSOR_DRAW_REFUSAL_MULTI_SCREEN);
	CHECK(f1.first);
	CHECK_FALSE(f1.second);
	CHECK_FALSE(u_cursor_draw_latch_allows(&s.latch));

	// Sustained refusal: stays false, no oscillation, nothing drawn.
	for (int i = 0; i < 5; i++) {
		auto f = s.frame(U_CURSOR_DRAW_REFUSAL_MULTI_SCREEN);
		CHECK_FALSE(f.first);
		CHECK_FALSE(f.second);
	}

	// The refusal clears: the report-only probe frame draws NOTHING (the app
	// is showing its OS cursor - two cursors otherwise) but restores true.
	auto f7 = s.frame(U_CURSOR_DRAW_REFUSAL_NONE);
	CHECK_FALSE(f7.first);
	CHECK_FALSE(f7.second);
	CHECK(u_cursor_draw_latch_allows(&s.latch));

	auto f8 = s.frame(U_CURSOR_DRAW_REFUSAL_NONE);
	CHECK(f8.first);
	CHECK(f8.second);

	// One WARN for the whole episode.
	CHECK(s.warns == 1);
}

TEST_CASE("cursor_draw: refusal latch - willDraw follows the previous commit, sprite only while true")
{
	// Any refuse/draw pattern: willDraw is exactly the previous commit's
	// outcome; the sprite is never drawn while willDraw is false (no double
	// cursor) and always drawn while it is true and the compositor can.
	LatchSim s;
	const u_cursor_draw_refusal pattern[] = {
	    U_CURSOR_DRAW_REFUSAL_NONE, U_CURSOR_DRAW_REFUSAL_FORCED,      U_CURSOR_DRAW_REFUSAL_NONE,
	    U_CURSOR_DRAW_REFUSAL_NONE, U_CURSOR_DRAW_REFUSAL_NO_TILES,    U_CURSOR_DRAW_REFUSAL_NO_TILES,
	    U_CURSOR_DRAW_REFUSAL_NONE, U_CURSOR_DRAW_REFUSAL_NOT_REACHED, U_CURSOR_DRAW_REFUSAL_NONE,
	    U_CURSOR_DRAW_REFUSAL_NONE, U_CURSOR_DRAW_REFUSAL_NONE,        U_CURSOR_DRAW_REFUSAL_DEFAULT_LOOK,
	    U_CURSOR_DRAW_REFUSAL_NONE, U_CURSOR_DRAW_REFUSAL_NONE,
	};
	bool prev_refused = false;
	for (u_cursor_draw_refusal r : pattern) {
		auto f = s.frame(r);
		if (!f.first) {
			CHECK_FALSE(f.second);
		}
		if (f.first && r == U_CURSOR_DRAW_REFUSAL_NONE) {
			CHECK(f.second);
		}
		CHECK(f.first == !prev_refused);
		prev_refused = r != U_CURSOR_DRAW_REFUSAL_NONE;
	}
}

TEST_CASE("cursor_draw: refusal latch - a silent commit counts as a refusal (NOT_REACHED)")
{
	u_cursor_draw_latch latch{};
	u_cursor_draw_report report;
	u_cursor_draw_report_begin(&report);
	// The compositor never got to its cursor pass and wrote nothing.
	CHECK(u_cursor_draw_latch_record(&latch, &report));
	CHECK_FALSE(u_cursor_draw_latch_allows(&latch));
	CHECK((latch.warned & (1u << U_CURSOR_DRAW_REFUSAL_NOT_REACHED)) != 0);
}

TEST_CASE("cursor_draw: refusal latch - one WARN per distinct reason, never per frame")
{
	u_cursor_draw_latch latch{};
	u_cursor_draw_report report{};
	u_cursor_draw_request req{};
	req.report = &report;

	u_cursor_draw_report_refused(&req, U_CURSOR_DRAW_REFUSAL_NO_DISPLAY_INFO);
	CHECK(u_cursor_draw_latch_record(&latch, &report));
	CHECK_FALSE(u_cursor_draw_latch_record(&latch, &report));
	CHECK_FALSE(u_cursor_draw_latch_record(&latch, &report));

	// A draw in between does not re-arm the WARN for the same reason.
	u_cursor_draw_report_drew(&req);
	CHECK_FALSE(u_cursor_draw_latch_record(&latch, &report));
	u_cursor_draw_report_refused(&req, U_CURSOR_DRAW_REFUSAL_NO_DISPLAY_INFO);
	CHECK_FALSE(u_cursor_draw_latch_record(&latch, &report));

	// A new reason warns once.
	u_cursor_draw_report_refused(&req, U_CURSOR_DRAW_REFUSAL_SPRITE_UNUSABLE);
	CHECK(u_cursor_draw_latch_record(&latch, &report));
	CHECK_FALSE(u_cursor_draw_latch_record(&latch, &report));
}

TEST_CASE("cursor_draw: refusal report - the last word of the commit wins; no listener is a no-op")
{
	u_cursor_draw_report report;
	u_cursor_draw_report_begin(&report);
	CHECK_FALSE(report.answered);
	CHECK(report.reason == U_CURSOR_DRAW_REFUSAL_NOT_REACHED);

	u_cursor_draw_request req{};
	req.report = &report;
	u_cursor_draw_report_drew(&req);
	CHECK((report.answered && report.drew));
	// A later refusal in the same commit (e.g. the renderer after the
	// compositor resolved the request) overrides.
	u_cursor_draw_report_refused(&req, U_CURSOR_DRAW_REFUSAL_NO_COMPOSE_PASS);
	CHECK((report.answered && !report.drew && report.reason == U_CURSOR_DRAW_REFUSAL_NO_COMPOSE_PASS));
	// Refusing "for no reason" is still a refusal.
	u_cursor_draw_report_refused(&req, U_CURSOR_DRAW_REFUSAL_NONE);
	CHECK((!report.drew && report.reason == U_CURSOR_DRAW_REFUSAL_NOT_REACHED));

	// No listener (report == NULL, every zero-initialised request): no-op,
	// no crash.
	u_cursor_draw_request quiet{};
	u_cursor_draw_report_drew(&quiet);
	u_cursor_draw_report_refused(&quiet, U_CURSOR_DRAW_REFUSAL_FORCED);
	u_cursor_draw_report_drew(nullptr);
	CHECK(quiet.report == nullptr);

	// Every reason has a name.
	for (int r = 0; r < U_CURSOR_DRAW_REFUSAL_COUNT; r++) {
		CHECK(strcmp(u_cursor_draw_refusal_name((u_cursor_draw_refusal)r), "unknown") != 0);
	}
}


/*
 *
 * Spec v7: measured disparity (ADR-046 Amendment 3).
 *
 * Synthetic stereo pairs: layers (back to front) of value noise, a flat
 * gradient ("sky") or vertical stripes, each at a disparity D in SOURCE px
 * (x_A - x_B, > 0 crossed, the web's sign): view A shows a layer's texture
 * at x - D/2, view B at x + D/2. The measurement must report x_B - x_A.
 *
 */

namespace {

enum class Tex
{
	Noise,
	Sky,
	Stripes
};

struct Layer
{
	Tex tex;
	float d;              // disparity, source px, > 0 crossed
	float x0, y0, x1, y1; // cyclopean rect, source px
	uint32_t seed;
	float period = 8.0f; // stripes
};

float
hash01(int32_t x, int32_t y, uint32_t seed)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (float)(h & 0xffffu) / 65535.0f;
}

// Smooth value noise on a 3 px lattice: texture at a few-px scale, continuous
// in x so fractional disparities are exact.
float
noise(float x, float y, uint32_t seed)
{
	const float s = 3.0f;
	const float fx = x / s, fy = y / s;
	const int32_t ix = (int32_t)std::floor(fx), iy = (int32_t)std::floor(fy);
	float tx = fx - (float)ix, ty = fy - (float)iy;
	tx = tx * tx * (3 - 2 * tx);
	ty = ty * ty * (3 - 2 * ty);
	const float a = hash01(ix, iy, seed), b = hash01(ix + 1, iy, seed);
	const float c = hash01(ix, iy + 1, seed), d = hash01(ix + 1, iy + 1, seed);
	const float top = a + (b - a) * tx;
	const float bottom = c + (d - c) * tx;
	return top + (bottom - top) * ty;
}

struct Scene
{
	std::vector<Layer> layers; // back to front

	// Luma 0..255 of view A (sign +1) or B (sign -1) at source px (x, y).
	float
	at(int sign, float x, float y) const
	{
		float v = 128.0f;
		for (const Layer &l : layers) {
			const float xc = x - (float)sign * l.d * 0.5f;
			const float yc = y - (float)sign * vertical * 0.5f; // an unrectified pair
			if (xc < l.x0 || xc >= l.x1 || yc < l.y0 || yc >= l.y1) {
				continue;
			}
			switch (l.tex) {
			case Tex::Noise: v = 40.0f + 180.0f * noise(xc, yc, l.seed); break;
			case Tex::Sky: v = 150.0f + 40.0f * (yc - l.y0) / (l.y1 - l.y0); break;
			case Tex::Stripes: v = 128.0f + 90.0f * std::sin(2.0f * 3.14159265f * xc / l.period); break;
			}
		}
		return v;
	}

	//! Vertical misalignment between the views, source px (y_A - y_B).
	float vertical = 0.0f;
};

struct Measured
{
	bool planned = false;
	u_cursor_measure_plan plan{};
	u_cursor_measure_result res{};
};

uint16_t
to_half(float f)
{
	// Positive normal values only (luma / 255): enough for 8-bit steps.
	if (!(f > 0.0f)) {
		return 0;
	}
	int e = 0;
	const float mant = std::frexp(f, &e); // f = mant * 2^e, mant in [0.5, 1)
	const uint32_t he = (uint32_t)(e - 1 + 15);
	const uint32_t hm = (uint32_t)std::lround((mant * 2.0f - 1.0f) * 1024.0f);
	return (uint16_t)((he << 10) + hm);
}

// Copy the plan's rect out of both views of a sub_w x sub_h sub-image in
// `fmt` (storage order: bottom-up rows if asked), then run the matcher.
Measured
measure(const Scene &sc,
        int32_t sub_w,
        int32_t sub_h,
        float u,
        float v,
        float height = 0.03f,
        u_cursor_measure_format fmt = U_CURSOR_MEASURE_FORMAT_RGBA8,
        bool bottom_up = false,
        u_cursor_measure_cache *cache = nullptr)
{
	Measured m;
	u_cursor_measure_request req{};
	req.requested = true;
	req.tag = 7;
	req.u = u;
	req.v = v;
	req.height = height;
	req.view_index[0] = 0;
	req.view_index[1] = 1;
	m.planned = u_cursor_measure_make_plan(&req, sub_w, sub_h, bottom_up, &m.plan);
	if (!m.planned) {
		return m;
	}
	const u_cursor_measure_plan &p = m.plan;
	const uint32_t bpp = u_cursor_measure_format_bytes(fmt);
	const uint32_t pitch = (uint32_t)p.w * bpp + 12; // padded rows, like a real readback
	std::vector<uint8_t> buf[2];
	for (int view = 0; view < 2; view++) {
		buf[view].assign((size_t)pitch * p.h, 0);
		for (int32_t r = 0; r < p.h; r++) {
			const int32_t storage_row = p.y + r;
			const int32_t top_down_row = bottom_up ? sub_h - 1 - storage_row : storage_row;
			for (int32_t c = 0; c < p.w; c++) {
				const float l = sc.at(view == 0 ? +1 : -1, (float)(p.x + c) + 0.5f, (float)top_down_row + 0.5f);
				const uint8_t g = (uint8_t)std::lround(std::fmin(255.0f, std::fmax(0.0f, l)));
				uint8_t *t = &buf[view][(size_t)r * pitch + (size_t)c * bpp];
				switch (fmt) {
				case U_CURSOR_MEASURE_FORMAT_RGBA8:
				case U_CURSOR_MEASURE_FORMAT_BGRA8: t[0] = t[1] = t[2] = g, t[3] = 255; break;
				case U_CURSOR_MEASURE_FORMAT_RGB10A2: {
					const uint32_t q = (uint32_t)std::lround(g * 1023.0 / 255.0);
					const uint32_t word = q | (q << 10) | (q << 20) | (3u << 30);
					memcpy(t, &word, 4);
					break;
				}
				case U_CURSOR_MEASURE_FORMAT_RGBA16F: {
					const uint16_t hv = to_half((float)g / 255.0f);
					const uint16_t px[4] = {hv, hv, hv, 0x3c00};
					memcpy(t, px, 8);
					break;
				}
				default: break;
				}
			}
		}
	}
	const u_cursor_measure_patch a{buf[0].data(), pitch, fmt};
	const u_cursor_measure_patch b{buf[1].data(), pitch, fmt};
	REQUIRE(u_cursor_measure_run(&req, &m.plan, &a, &b, cache, &m.res));
	return m;
}

Scene
textured(float d, uint32_t seed = 1)
{
	Scene s;
	s.layers.push_back({Tex::Noise, d, -1e4f, -1e4f, 1e4f, 1e4f, seed});
	return s;
}

} // namespace

TEST_CASE("cursor_measure: zero request does nothing; plan stays bounded and inside the sub-image")
{
	u_cursor_measure_request zero{};
	CHECK_FALSE(u_cursor_measure_should_sample(&zero));
	u_cursor_measure_plan p{};
	CHECK_FALSE(u_cursor_measure_make_plan(&zero, 960, 1080, false, &p));

	u_cursor_measure_request req{};
	req.requested = true;
	req.u = 0.5f;
	req.v = 0.5f;
	req.height = 0.03f;
	req.view_index[1] = 1;
	CHECK(u_cursor_measure_should_sample(&req));
	// Pointer off the canvas, on every side (and NaN): nothing is measured.
	for (float uv : {-0.01f, 1.01f, NAN}) {
		CAPTURE(uv);
		req.u = uv;
		CHECK_FALSE(u_cursor_measure_should_sample(&req));
		req.u = 0.5f;
		req.v = uv;
		CHECK_FALSE(u_cursor_measure_should_sample(&req));
		req.v = 0.5f;
	}
	// ...and a sprite with no height.
	req.height = 0.0f;
	CHECK_FALSE(u_cursor_measure_should_sample(&req));
	req.height = 0.03f;
	// A filled-in request that was not flagged as requested is not one.
	req.requested = false;
	CHECK_FALSE(u_cursor_measure_should_sample(&req));
	req.requested = true;
	req.view_index[1] = 0;
	CHECK_FALSE(u_cursor_measure_should_sample(&req)); // one view: nothing to match

	req.view_index[1] = 1;
	for (float h : {0.01f, 0.03f, 0.2f}) {      // a huge sprite still caps the footprint
		for (int32_t sw : {960, 1920, 3840}) {  // 1080p SBS, 4K SBS, 8K SBS
			for (float u : {0.0f, 0.5f, 1.0f}) { // edges clamp
				CAPTURE(h, sw, u);
				req.u = u;
				req.height = h;
				REQUIRE(u_cursor_measure_make_plan(&req, sw, sw * 9 / 8, false, &p));
				CHECK(p.work_w <= U_CURSOR_MEASURE_MAX_WORK_W);
				CHECK(p.work_h <= U_CURSOR_MEASURE_MAX_WORK_H);
				CHECK(p.x >= 0);
				CHECK(p.x + p.w <= sw);
				CHECK(p.y >= 0);
				CHECK(p.y + p.h <= sw * 9 / 8);
				CHECK(p.factor == (sw <= 960 ? 1 : sw <= 1920 ? 2 : 4));
			}
		}
	}
	// Bottom-up storage: the same rows, counted from the other end.
	req.u = 0.5f;
	req.v = 0.2f;
	req.height = 0.03f;
	u_cursor_measure_plan td{}, bu{};
	REQUIRE(u_cursor_measure_make_plan(&req, 960, 1080, false, &td));
	REQUIRE(u_cursor_measure_make_plan(&req, 960, 1080, true, &bu));
	CHECK(bu.y == 1080 - (td.y + td.h));
	CHECK(bu.top_y == td.top_y);
}

TEST_CASE("cursor_measure: a textured shift is found, sub-pixel, crossed < 0")
{
	for (float d : {0.0f, 7.0f, 12.5f, 30.0f, 60.0f, -20.0f}) {
		CAPTURE(d);
		const Measured m = measure(textured(d), 960, 1080, 0.5f, 0.5f);
		REQUIRE(m.planned);
		REQUIRE(m.res.valid);
		REQUIRE(m.res.confident);
		CHECK(m.res.disparity_px == Catch::Approx(-d).margin(0.35f)); // x_B - x_A
		CHECK(m.res.sub_w == 960.0f);
		CHECK(m.res.ncc >= U_CURSOR_MEASURE_MIN_NCC);
	}
	// Beyond the search range there is no confident answer.
	const Measured beyond = measure(textured(90.0f), 960, 1080, 0.5f, 0.5f);
	CHECK_FALSE(beyond.res.confident);
}

TEST_CASE("cursor_measure: 4K SBS (factor 2) reports SOURCE px")
{
	const Measured m = measure(textured(40.0f, 3), 1920, 2160, 0.4f, 0.6f);
	REQUIRE(m.plan.factor == 2);
	REQUIRE(m.res.confident);
	CHECK(m.res.disparity_px == Catch::Approx(-40.0f).margin(0.8f));
}

TEST_CASE("cursor_measure: every texel format and bottom-up storage give the same answer")
{
	const Scene s = textured(18.0f, 5);
	const Measured ref = measure(s, 960, 1080, 0.3f, 0.7f);
	REQUIRE(ref.res.confident);
	for (auto fmt : {U_CURSOR_MEASURE_FORMAT_BGRA8, U_CURSOR_MEASURE_FORMAT_RGB10A2, U_CURSOR_MEASURE_FORMAT_RGBA16F}) {
		CAPTURE((int)fmt);
		const Measured m = measure(s, 960, 1080, 0.3f, 0.7f, 0.03f, fmt);
		REQUIRE(m.res.confident);
		CHECK(m.res.disparity_px == Catch::Approx(ref.res.disparity_px).margin(0.2f));
	}
	const Measured bu = measure(s, 960, 1080, 0.3f, 0.7f, 0.03f, U_CURSOR_MEASURE_FORMAT_RGBA8, true);
	REQUIRE(bu.res.confident);
	CHECK(bu.res.disparity_px == Catch::Approx(ref.res.disparity_px).margin(0.01f));
}

TEST_CASE("cursor_measure: a GL (bottom-up) texture is read in its own rows")
{
	// Foreground only in the bottom 40% of the image; the pointer is near the
	// top. Reading the copy's rows the wrong way up lands on the foreground.
	Scene s;
	s.layers.push_back({Tex::Noise, -10.0f, -1e4f, -1e4f, 1e4f, 1e4f, 51});
	s.layers.push_back({Tex::Noise, 30.0f, -1e4f, 650.0f, 1e4f, 1e4f, 52});
	const Measured td = measure(s, 960, 1080, 0.5f, 0.2f);
	const Measured bu = measure(s, 960, 1080, 0.5f, 0.2f, 0.03f, U_CURSOR_MEASURE_FORMAT_RGBA8, true);
	REQUIRE(td.res.confident);
	REQUIRE(bu.res.confident);
	CHECK(td.res.disparity_px == Catch::Approx(10.0f).margin(0.5f));
	CHECK(bu.res.disparity_px == Catch::Approx(10.0f).margin(0.5f));
}

TEST_CASE("cursor_measure: BGRA channel order is honoured (red-only texture)")
{
	// Texture only in RED: luma std ~10 read right, ~4 (under the floor) if
	// red were taken for blue.
	const int32_t sub_w = 960, sub_h = 1080;
	u_cursor_measure_request req{};
	req.requested = true;
	req.u = 0.5f;
	req.v = 0.5f;
	req.height = 0.03f;
	req.view_index[1] = 1;
	u_cursor_measure_plan p{};
	REQUIRE(u_cursor_measure_make_plan(&req, sub_w, sub_h, false, &p));
	std::vector<uint8_t> buf[2];
	for (int view = 0; view < 2; view++) {
		buf[view].resize((size_t)p.w * p.h * 4);
		for (int32_t r = 0; r < p.h; r++) {
			for (int32_t c = 0; c < p.w; c++) {
				const float x = (float)(p.x + c) - (view == 0 ? 1.0f : -1.0f) * 6.0f; // d = 12
				uint8_t *t = &buf[view][((size_t)r * p.w + c) * 4];
				t[0] = 128;                                                        // B
				t[1] = 128;                                                        // G
				t[2] = (uint8_t)(28 + 200 * noise(x, (float)(p.y + r), 61)); // R
				t[3] = 255;
			}
		}
	}
	const u_cursor_measure_patch a{buf[0].data(), (uint32_t)p.w * 4, U_CURSOR_MEASURE_FORMAT_BGRA8};
	const u_cursor_measure_patch b{buf[1].data(), (uint32_t)p.w * 4, U_CURSOR_MEASURE_FORMAT_BGRA8};
	u_cursor_measure_result res{};
	REQUIRE(u_cursor_measure_run(&req, &p, &a, &b, nullptr, &res));
	REQUIRE(res.confident);
	CHECK(res.disparity_px == Catch::Approx(-12.0f).margin(0.35f));
}

TEST_CASE("cursor_measure: an unrectified pair (rows off by up to 2) still matches")
{
	for (float dy : {-2.0f, 1.0f, 2.0f}) {
		CAPTURE(dy);
		Scene s = textured(14.0f, 71);
		s.vertical = dy;
		const Measured m = measure(s, 960, 1080, 0.5f, 0.5f);
		REQUIRE(m.res.confident);
		CHECK(m.res.disparity_px == Catch::Approx(-14.0f).margin(0.35f));
	}
}

TEST_CASE("cursor_measure: flat sky is not confident (texture floor) -> screen plane")
{
	Scene s;
	s.layers.push_back({Tex::Sky, 0.0f, -1e4f, 0.0f, 1e4f, 1080.0f, 1});
	const Measured m = measure(s, 960, 1080, 0.5f, 0.3f);
	REQUIRE(m.res.valid);
	CHECK_FALSE(m.res.confident);
	CHECK(m.res.matched == 0);
	CHECK(m.res.disparity_px == 0.0f);

	// Even with a crossed shift, a flat region has nothing to match.
	Scene s2;
	s2.layers.push_back({Tex::Sky, 20.0f, -1e4f, 0.0f, 1e4f, 1080.0f, 1});
	CHECK_FALSE(measure(s2, 960, 1080, 0.5f, 0.3f).res.confident);
}

TEST_CASE("cursor_measure: faint texture and uncorrelated views are not confident")
{
	// Texture std ~3 luma levels: below the floor (web MIN_TEXTURE 6). A
	// perfect synthetic shift would otherwise match it - real video's noise
	// and compression would not. Control: the same texture at full contrast.
	const int32_t w = 200, h = 40;
	float d = 0, c = 0;
	int32_t dy = 0;
	for (float amp : {180.0f, 12.0f}) {
		CAPTURE(amp);
		std::vector<uint8_t> a((size_t)w * h), b((size_t)w * h);
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				a[(size_t)y * w + x] = (uint8_t)(128 - amp / 2 + amp * noise((float)x - 10.0f, (float)y, 31));
				b[(size_t)y * w + x] = (uint8_t)(128 - amp / 2 + amp * noise((float)x + 10.0f, (float)y, 31));
			}
		}
		const bool ok = u_cursor_measure_match_block(a.data(), b.data(), w, h, 92, 12, +1, -64, 64, &d, &dy, &c);
		CHECK(ok == (amp > 100.0f));
	}

	// Views that do not show the same thing (one eye occluded): whatever
	// scores best is a coincidence, below the NCC floor or not unique.
	for (uint32_t seed = 40; seed < 46; seed++) {
		CAPTURE(seed);
		std::vector<uint8_t> p((size_t)w * h), q((size_t)w * h);
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				p[(size_t)y * w + x] = (uint8_t)(40 + 180 * noise((float)x, (float)y, seed));
				q[(size_t)y * w + x] = (uint8_t)(40 + 180 * noise((float)x, (float)y, seed + 100));
			}
		}
		float pts[U_CURSOR_MEASURE_POINTS][2];
		u_cursor_measure_footprint_points(100.0f, 12.0f, 14.0f, pts);
		u_cursor_measure_result diag{};
		CHECK_FALSE(u_cursor_measure_footprint(p.data(), q.data(), w, h, pts, U_CURSOR_MEASURE_POINTS, &d, &diag));
		CHECK(diag.matched == 0);
	}
}

TEST_CASE("cursor_measure: repetitive stripes are rejected (uniqueness)")
{
	// Odd periods put the rival peaks between coarse samples: they must be
	// refined to their top, or an exactly periodic pattern slips through.
	for (float period : {6.0f, 7.0f, 8.0f, 9.0f, 11.0f, 13.0f, 24.0f}) {
		for (float d : {0.0f, 5.0f, 10.0f}) {
			CAPTURE(period, d);
			Scene s;
			s.layers.push_back({Tex::Stripes, d, -1e4f, -1e4f, 1e4f, 1e4f, 1, period});
			const Measured m = measure(s, 960, 1080, 0.5f, 0.5f);
			REQUIRE(m.res.valid);
			CHECK_FALSE(m.res.confident);
			CHECK(m.res.matched == 0); // every block is ambiguous on its own
		}
	}
}

TEST_CASE("cursor_measure: occlusion edge - the nearest confident layer wins, never a blend")
{
	// Background behind the glass (uncrossed 10), a crossed (24) textured
	// foreground whose edge runs through the footprint.
	for (float edge : {470.0f, 480.0f, 492.0f}) {
		CAPTURE(edge);
		Scene s;
		s.layers.push_back({Tex::Noise, -10.0f, -1e4f, -1e4f, 1e4f, 1e4f, 11});
		s.layers.push_back({Tex::Noise, 24.0f, -1e4f, -1e4f, edge, 1e4f, 12});
		const Measured m = measure(s, 960, 1080, 0.5f, 0.5f); // tip at x = 480
		REQUIRE(m.res.confident);
		CHECK(m.res.disparity_px == Catch::Approx(-24.0f).margin(0.5f));
	}
	// Foreground entirely outside the footprint: the background answers.
	Scene s;
	s.layers.push_back({Tex::Noise, -10.0f, -1e4f, -1e4f, 1e4f, 1e4f, 11});
	s.layers.push_back({Tex::Noise, 24.0f, -1e4f, -1e4f, 300.0f, 1e4f, 12});
	const Measured m = measure(s, 960, 1080, 0.5f, 0.5f);
	REQUIRE(m.res.confident);
	CHECK(m.res.disparity_px == Catch::Approx(10.0f).margin(0.5f)); // uncrossed: behind the glass
}

TEST_CASE("cursor_measure: left-right check rejects a match the other view cannot confirm")
{
	// A (left) shows a textured patch P twice, 30 px apart; B shows it once.
	// A's left copy finds B's uniquely (d = 10), but B's copy, matched back
	// into A, has two equal answers - an inconsistent, half-occluded match.
	const int32_t w = 160, h = 40;
	std::vector<uint8_t> a((size_t)w * h), b((size_t)w * h);
	for (int32_t y = 0; y < h; y++) {
		for (int32_t x = 0; x < w; x++) {
			a[(size_t)y * w + x] = (uint8_t)(40 + 180 * noise((float)x, (float)y, 99));
			b[(size_t)y * w + x] = (uint8_t)(40 + 180 * noise((float)x + 500.0f, (float)y, 77));
		}
	}
	auto stamp = [&](std::vector<uint8_t> &img, int32_t x0) {
		for (int32_t y = 8; y < 32; y++) {
			for (int32_t x = 0; x < 24; x++) {
				img[(size_t)y * w + x0 + x] = (uint8_t)(40 + 180 * noise((float)x, (float)y, 5));
			}
		}
	};
	stamp(a, 70);
	stamp(a, 100);
	stamp(b, 60);
	// Forward: block at A x=74 -> B at 64 (d = 10), unique in B.
	float d = 0, c = 0;
	int32_t dy = 0;
	REQUIRE(u_cursor_measure_match_block(a.data(), b.data(), w, h, 74, 12, +1, -64, 64, &d, &dy, &c));
	CHECK(d == Catch::Approx(10.0f).margin(0.3f));
	// Backward: ambiguous -> the footprint rejects the candidate.
	CHECK_FALSE(u_cursor_measure_match_block(b.data(), a.data(), w, h, 64, 12, -1, -64, 64, &d, &dy, &c));
	const float pts[1][2] = {{74.0f + 8.0f, 12.0f + 8.0f}};
	u_cursor_measure_result diag{};
	float out = 0;
	CHECK_FALSE(u_cursor_measure_footprint(a.data(), b.data(), w, h, pts, 1, &out, &diag));
	CHECK(diag.matched == 1);
	CHECK(diag.lr_rejected == 1);
}

TEST_CASE("cursor_measure: left-right check rejects a confident but INCONSISTENT reverse match")
{
	// A shows P at 70 and P' (P plus a little unrelated texture) at 100; B
	// shows only P' at 60. A's P finds B's P' uniquely (d = 10, NCC ~0.86),
	// but B's P' finds its exact copy in A at 100 (d' = 40) - unique, and
	// confident, yet 30 px from the forward answer.
	const int32_t w = 200, h = 40;
	std::vector<uint8_t> a((size_t)w * h), b((size_t)w * h);
	for (int32_t y = 0; y < h; y++) {
		for (int32_t x = 0; x < w; x++) {
			a[(size_t)y * w + x] = (uint8_t)(40 + 180 * noise((float)x, (float)y, 199));
			b[(size_t)y * w + x] = (uint8_t)(40 + 180 * noise((float)x + 700.0f, (float)y, 177));
		}
	}
	auto p = [](int32_t x, int32_t y) { return 128.0f + 70.0f * (noise((float)x, (float)y, 5) - 0.5f) * 2.0f; };
	auto pp = [&](int32_t x, int32_t y) { return p(x, y) + 45.0f * (noise((float)x, (float)y, 6) - 0.5f) * 2.0f; };
	for (int32_t y = 4; y < 36; y++) {
		for (int32_t x = 0; x < 24; x++) {
			a[(size_t)y * w + 70 + x] = (uint8_t)p(x, y);
			a[(size_t)y * w + 100 + x] = (uint8_t)pp(x, y);
			b[(size_t)y * w + 60 + x] = (uint8_t)pp(x, y);
		}
	}
	float d = 0, c = 0;
	int32_t dy = 0;
	REQUIRE(u_cursor_measure_match_block(a.data(), b.data(), w, h, 74, 12, +1, -64, 64, &d, &dy, &c));
	CHECK(d == Catch::Approx(10.0f).margin(0.3f));
	CHECK(c >= U_CURSOR_MEASURE_MIN_NCC);
	REQUIRE(u_cursor_measure_match_block(b.data(), a.data(), w, h, 64, 12, -1, -64, 64, &d, &dy, &c));
	CHECK(d == Catch::Approx(40.0f).margin(0.3f));
	const float pts[1][2] = {{74.0f + 8.0f, 12.0f + 8.0f}};
	u_cursor_measure_result diag{};
	float out = 0;
	CHECK_FALSE(u_cursor_measure_footprint(a.data(), b.data(), w, h, pts, 1, &out, &diag));
	CHECK(diag.matched == 1);
	CHECK(diag.lr_rejected == 1);
}

TEST_CASE("cursor_measure: an unchanged frame is not re-measured (paused video)")
{
	u_cursor_measure_cache cache{};
	const Scene s = textured(16.0f, 21);
	const Measured first = measure(s, 960, 1080, 0.5f, 0.5f, 0.03f, U_CURSOR_MEASURE_FORMAT_RGBA8, false, &cache);
	REQUIRE(first.res.confident);
	CHECK_FALSE(first.res.reused);
	const Measured again = measure(s, 960, 1080, 0.5f, 0.5f, 0.03f, U_CURSOR_MEASURE_FORMAT_RGBA8, false, &cache);
	CHECK(again.res.reused);
	CHECK(again.res.disparity_px == first.res.disparity_px);
	// New content (the video plays on) or a moved pointer: measured again.
	const Measured moved = measure(s, 960, 1080, 0.52f, 0.5f, 0.03f, U_CURSOR_MEASURE_FORMAT_RGBA8, false, &cache);
	CHECK_FALSE(moved.res.reused);
	const Measured next =
	    measure(textured(30.0f, 21), 960, 1080, 0.52f, 0.5f, 0.03f, U_CURSOR_MEASURE_FORMAT_RGBA8, false, &cache);
	CHECK_FALSE(next.res.reused);
	CHECK(next.res.disparity_px == Catch::Approx(-30.0f).margin(0.35f));

	// The same pixels under a different footprint (the pointer moved inside
	// one copy) are a different question: never answered from the cache.
	u_cursor_measure_request req{};
	req.requested = true;
	req.u = 0.5f;
	req.v = 0.5f;
	req.height = 0.03f;
	req.view_index[1] = 1;
	u_cursor_measure_plan p{};
	REQUIRE(u_cursor_measure_make_plan(&req, 960, 1080, false, &p));
	std::vector<uint8_t> px((size_t)p.w * p.h * 4, 0);
	for (size_t i = 0; i < px.size(); i += 4) {
		px[i] = px[i + 1] = px[i + 2] = (uint8_t)(40 + 180 * noise((float)((i / 4) % p.w), (float)((i / 4) / p.w), 3));
	}
	const u_cursor_measure_patch a{px.data(), (uint32_t)p.w * 4, U_CURSOR_MEASURE_FORMAT_RGBA8};
	u_cursor_measure_cache c2{};
	u_cursor_measure_result r1{}, r2{};
	REQUIRE(u_cursor_measure_run(&req, &p, &a, &a, &c2, &r1));
	req.u = 0.505f;
	REQUIRE(u_cursor_measure_run(&req, &p, &a, &a, &c2, &r2));
	CHECK_FALSE(r2.reused);
}

TEST_CASE("cursor_measure: px -> baselines from the views, and the pixel policy is A.1")
{
	// Eyes 64 mm apart, 0.6 m from a 0.30 x 0.20 m canvas, 960 px per view.
	const auto a = kooima({-0.032f, 0.0f, 0.6f});
	const auto b = kooima({0.032f, 0.0f, 0.6f});
	u_cursor_depth_geometry g{};
	REQUIRE(u_cursor_depth_geometry_solve(&a, &b, 0.5f, 0.5f, &g));
	float d = 0, ppb = 0;
	// Halfway to the eye is d = -1: on screen x_B - x_A = -64 mm = -204.8 px.
	REQUIRE(u_cursor_measure_to_baselines(&a, &b, &g, -204.8f, 960.0f, &d, &ppb));
	CHECK(d == Catch::Approx(-1.0f));
	CHECK(ppb == Catch::Approx(204.8f));
	// Same answer under a camera rig (rigid + 12.5x scale): ratios only.
	Xform x;
	x.q = {0.0f, 0.38268343f, 0.0f, 0.92387953f};
	x.t = {1.0f, 2.0f, -3.0f};
	x.s = 12.5f;
	const auto a1 = kooima({-0.032f, 0.0f, 0.6f}, x);
	const auto b1 = kooima({0.032f, 0.0f, 0.6f}, x);
	u_cursor_depth_geometry g1{};
	REQUIRE(u_cursor_depth_geometry_solve(&a1, &b1, 0.5f, 0.5f, &g1));
	float d1 = 0, ppb1 = 0;
	REQUIRE(u_cursor_measure_to_baselines(&a1, &b1, &g1, -204.8f, 960.0f, &d1, &ppb1));
	CHECK(d1 == Catch::Approx(-1.0f).epsilon(1e-4));
	CHECK(ppb1 == Catch::Approx(204.8f).epsilon(1e-4));
	// Swapped views (B left of A): the sign follows the views, not the index.
	REQUIRE(u_cursor_measure_to_baselines(&b, &a, &g, 204.8f, 960.0f, &d, &ppb));
	CHECK(d == Catch::Approx(-1.0f));

	// Pixel policy: measured < 0 ? max(measured - margin_px, cap_px) : 0.
	const auto t = tuning();
	const float margin_px = t.margin * ppb;
	CHECK(u_cursor_measure_target_px(&t, true, -20.0f, ppb) == Catch::Approx(-20.0f - margin_px));
	CHECK(u_cursor_measure_target_px(&t, true, -500.0f, ppb) == Catch::Approx(t.min_disparity * ppb)); // cap
	CHECK(u_cursor_measure_target_px(&t, true, 15.0f, ppb) == 0.0f);   // behind the glass: on it
	CHECK(u_cursor_measure_target_px(&t, true, 0.0f, ppb) == 0.0f);    // on the glass
	CHECK(u_cursor_measure_target_px(&t, false, -20.0f, ppb) == 0.0f); // not confident: screen plane
	for (float m = -300.0f; m <= 300.0f; m += 7.3f) {
		CHECK(u_cursor_measure_target_px(&t, true, m, ppb) <= 0.0f); // never behind the glass
		CHECK(u_cursor_measure_target_px(&t, true, m, ppb) / ppb ==
		      Catch::Approx(u_cursor_depth_target(&t, true, m / ppb)).margin(1e-6));
	}
}

// Hidden ([.]): CPU cost of one measured frame, for the Amendment 3 evidence.
// Run a Release build: tests_aux_cursor_depth "[.bench]"
TEST_CASE("cursor_measure: CPU cost per measured frame (1080p / 4K SBS)", "[.bench]")
{
	for (int32_t sw : {960, 1920}) {
		const int32_t sh = sw * 9 / 8; // a 1080p / 4K SBS eye: 960x1080, 1920x2160
		const float d = 0.03f * (float)sw;
		struct Case
		{
			const char *name;
			Scene scene;
		} cases[3] = {{"textured", textured(d, 81)}, {"sky", {}}, {"stripes", {}}};
		cases[1].scene.layers.push_back({Tex::Sky, 0.0f, -1e4f, -1e4f, 1e4f, 1e5f, 1});
		cases[2].scene.layers.push_back({Tex::Stripes, d, -1e4f, -1e4f, 1e4f, 1e4f, 1, 9.0f});
		for (Case &cs : cases) {
			// Build the copy once, then time u_cursor_measure_run alone.
			u_cursor_measure_cache cache{};
			Measured m = measure(cs.scene, sw, sh, 0.5f, 0.5f, 0.03f, U_CURSOR_MEASURE_FORMAT_BGRA8, false, &cache);
			u_cursor_measure_request req{};
			req.requested = true;
			req.u = 0.5f;
			req.v = 0.5f;
			req.height = 0.03f;
			req.view_index[1] = 1;
			std::vector<uint8_t> buf[2];
			const uint32_t pitch = (uint32_t)m.plan.w * 4;
			for (int v = 0; v < 2; v++) {
				buf[v].resize((size_t)pitch * m.plan.h);
				for (int32_t r = 0; r < m.plan.h; r++) {
					for (int32_t c = 0; c < m.plan.w; c++) {
						const uint8_t g = (uint8_t)cs.scene.at(v == 0 ? 1 : -1, (float)(m.plan.x + c) + 0.5f,
						                                       (float)(m.plan.y + r) + 0.5f);
						uint8_t *t = &buf[v][(size_t)r * pitch + (size_t)c * 4];
						t[0] = t[1] = t[2] = g;
						t[3] = 255;
					}
				}
			}
			const u_cursor_measure_patch a{buf[0].data(), pitch, U_CURSOR_MEASURE_FORMAT_BGRA8};
			const u_cursor_measure_patch b{buf[1].data(), pitch, U_CURSOR_MEASURE_FORMAT_BGRA8};
			const int iters = 200;
			u_cursor_measure_result res{};
			auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < iters; i++) {
				REQUIRE(u_cursor_measure_run(&req, &m.plan, &a, &b, nullptr, &res)); // no cache: full match
			}
			auto t1 = std::chrono::steady_clock::now();
			u_cursor_measure_cache warm{};
			REQUIRE(u_cursor_measure_run(&req, &m.plan, &a, &b, &warm, &res));
			auto t2 = std::chrono::steady_clock::now();
			for (int i = 0; i < iters; i++) {
				REQUIRE(u_cursor_measure_run(&req, &m.plan, &a, &b, &warm, &res)); // paused video: reused
			}
			auto t3 = std::chrono::steady_clock::now();
			const double full_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / iters;
			const double reuse_us = std::chrono::duration<double, std::micro>(t3 - t2).count() / iters;
			printf("cursor_measure bench: eye %dx%d (%s SBS) %-8s copy %dx%d px/view (%u KB both), factor %d: "
			       "match %.1f us, reused %.1f us, confident=%d %.2f px\n",
			       sw, sh, sw == 960 ? "1080p" : "4K", cs.name, m.plan.w, m.plan.h,
			       (unsigned)(2 * pitch * m.plan.h / 1024), m.plan.factor, full_us, reuse_us, (int)res.confident,
			       res.disparity_px);
		}
	}
}


/*
 *
 * ADR-046 Amendment 2 B.7: crosstalk softening of the lifted cursor.
 *
 */

TEST_CASE("cursor_soften: defaults - both on, the documented values")
{
	u_cursor_soften_params p;
	u_cursor_soften_params_default(&p);
	CHECK(p.blur);
	CHECK(p.alpha);
	CHECK(p.blur_k == Catch::Approx(0.1f));
	CHECK(p.blur_max_px == Catch::Approx(8.0f));
	CHECK(p.blur_max_frac == Catch::Approx(0.06f));
	CHECK(p.alpha_min == Catch::Approx(0.5f));
	CHECK(p.full_d == Catch::Approx(0.03f));
}

TEST_CASE("cursor_soften: on the glass there is no softening at all")
{
	u_cursor_soften_params p;
	u_cursor_soften_params_default(&p);
	const u_cursor_soften s = u_cursor_draw_soften(0.0f, 0.0f, &p);
	CHECK(s.blur_px == 0.0f);
	CHECK(s.alpha == 1.0f);
	// A NULL params pointer and non-finite input are "no softening" too.
	const u_cursor_soften n = u_cursor_draw_soften(-0.05f, 10.0f, nullptr);
	CHECK((n.blur_px == 0.0f && n.alpha == 1.0f));
	const u_cursor_soften f = u_cursor_draw_soften(NAN, INFINITY, &p);
	CHECK((f.blur_px == 0.0f && f.alpha == 1.0f));
}

TEST_CASE("cursor_soften: blur = k x ghost, capped; monotonic in the ghost offset")
{
	u_cursor_soften_params p;
	u_cursor_soften_params_default(&p);
	p.blur_max_px = 1.5f; // the absolute cap (the unsized entry point applies only this)
	// Below the cap: exactly k x ghost (sign of the offset is irrelevant).
	CHECK(u_cursor_draw_soften(-0.02f, 5.0f, &p).blur_px == Catch::Approx(0.5f));
	CHECK(u_cursor_draw_soften(-0.02f, -5.0f, &p).blur_px == Catch::Approx(0.5f));
	CHECK(u_cursor_draw_soften(-0.02f, 14.0f, &p).blur_px == Catch::Approx(1.4f));
	// At and beyond the cap.
	CHECK(u_cursor_draw_soften(-0.02f, 15.0f, &p).blur_px == Catch::Approx(1.5f));
	CHECK(u_cursor_draw_soften(-0.02f, 200.0f, &p).blur_px == Catch::Approx(1.5f));
	float prev = -1.0f;
	for (float g = 0.0f; g <= 40.0f; g += 0.5f) {
		const float b = u_cursor_draw_soften(-0.02f, g, &p).blur_px;
		CHECK(b >= prev);
		CHECK(b <= p.blur_max_px);
		prev = b;
	}
	// The blur depends on the ghost offset only, not on d.
	CHECK(u_cursor_draw_soften(-0.5f, 5.0f, &p).blur_px == Catch::Approx(0.5f));
}

TEST_CASE("cursor_soften: opacity eases 1 -> alpha_min by smoothstep over |d|, then holds")
{
	u_cursor_soften_params p;
	u_cursor_soften_params_default(&p);
	p.alpha_min = 0.75f; // the numbers below are worked for these
	p.full_d = 0.1f;
	// Midpoint of a smoothstep is exactly half way.
	CHECK(u_cursor_draw_soften(-0.05f, 0.0f, &p).alpha == Catch::Approx(0.875f));
	// Quarter: smoothstep(0.25) = 0.15625.
	CHECK(u_cursor_draw_soften(-0.025f, 0.0f, &p).alpha == Catch::Approx(1.0f - 0.25f * 0.15625f));
	CHECK(u_cursor_draw_soften(-0.1f, 0.0f, &p).alpha == Catch::Approx(0.75f));
	CHECK(u_cursor_draw_soften(-0.6f, 0.0f, &p).alpha == Catch::Approx(0.75f));
	// |d|: symmetric (the policy never puts the cursor behind, but the
	// easing must not go above 1 or below alpha_min for any input).
	CHECK(u_cursor_draw_soften(0.05f, 0.0f, &p).alpha == Catch::Approx(0.875f));
	float prev = 2.0f;
	for (float d = 0.0f; d >= -0.3f; d -= 0.005f) {
		const float a = u_cursor_draw_soften(d, 0.0f, &p).alpha;
		CHECK(a <= prev);
		CHECK(a >= p.alpha_min - 1e-6f);
		CHECK(a <= 1.0f);
		prev = a;
	}
	// Smooth start: the slope at d = 0 is zero (tiny lift, ~no fade).
	CHECK(u_cursor_draw_soften(-0.001f, 0.0f, &p).alpha > 0.9999f);
}

TEST_CASE("cursor_soften: the presets switch each effect independently")
{
	u_cursor_soften_params p;
	CHECK(!u_cursor_soften_params_parse(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &p));
	CHECK((p.blur && p.alpha));
	CHECK(!u_cursor_soften_params_parse("both", nullptr, nullptr, nullptr, nullptr, nullptr, &p));
	CHECK((p.blur && p.alpha));

	CHECK(u_cursor_soften_params_parse("off", nullptr, nullptr, nullptr, nullptr, nullptr, &p));
	u_cursor_soften s = u_cursor_draw_soften(-0.2f, 30.0f, &p);
	CHECK((s.blur_px == 0.0f && s.alpha == 1.0f));

	CHECK(u_cursor_soften_params_parse("alpha", nullptr, nullptr, nullptr, nullptr, nullptr, &p));
	s = u_cursor_draw_soften(-0.2f, 30.0f, &p);
	CHECK(s.blur_px == 0.0f);
	CHECK(s.alpha == Catch::Approx(U_CURSOR_SOFTEN_DEFAULT_ALPHA_MIN));

	CHECK(u_cursor_soften_params_parse("blur", nullptr, nullptr, nullptr, nullptr, nullptr, &p));
	s = u_cursor_draw_soften(-0.2f, 30.0f, &p);
	CHECK(s.blur_px == Catch::Approx(0.1f * 30.0f)); // k x ghost, under the 8 px ceiling (unsized)
	CHECK(s.alpha == 1.0f);
}

TEST_CASE("cursor_soften: numeric knobs parse, and bad values keep the default")
{
	u_cursor_soften_params p;
	CHECK(u_cursor_soften_params_parse(nullptr, "0.2", "2", nullptr, "0.5", "0.05", &p));
	CHECK(p.blur_k == Catch::Approx(0.2f));
	CHECK(p.blur_max_px == Catch::Approx(2.0f));
	CHECK(p.alpha_min == Catch::Approx(0.5f));
	CHECK(p.full_d == Catch::Approx(0.05f));
	const u_cursor_soften s = u_cursor_draw_soften(-0.05f, 5.0f, &p);
	CHECK(s.blur_px == Catch::Approx(1.0f));
	CHECK(s.alpha == Catch::Approx(0.5f));

	// Out of range / garbage: each falls back on its own.
	CHECK(!u_cursor_soften_params_parse(nullptr, "-1", "9", "0.9", "1.5", "0", &p));
	CHECK(p.blur_k == Catch::Approx(0.1f));
	CHECK(p.blur_max_px == Catch::Approx(8.0f));
	CHECK(p.blur_max_frac == Catch::Approx(0.06f));
	CHECK(p.alpha_min == Catch::Approx(0.5f));
	CHECK(p.full_d == Catch::Approx(0.03f));
	CHECK(!u_cursor_soften_params_parse(nullptr, "abc", "1x", nullptr, "", "nan", &p));
	CHECK(p.blur_k == Catch::Approx(0.1f));
	CHECK(p.full_d == Catch::Approx(0.03f));
	// Windows cmd's `set X=off && app` leaves a trailing space: still honoured.
	CHECK(u_cursor_soften_params_parse("off ", nullptr, "2 ", nullptr, nullptr, nullptr, &p));
	CHECK((!p.blur && !p.alpha));
	CHECK(p.blur_max_px == Catch::Approx(2.0f));
	CHECK(u_cursor_soften_params_parse("blurx", nullptr, nullptr, nullptr, nullptr, nullptr, &p) == false);
	CHECK((p.blur && p.alpha));
	// Boundaries that are legal: k = 0 and max = 0 disable the blur.
	CHECK(u_cursor_soften_params_parse(nullptr, "0", nullptr, nullptr, "1", nullptr, &p));
	CHECK(u_cursor_draw_soften(-0.2f, 30.0f, &p).blur_px == 0.0f);
	CHECK(u_cursor_draw_soften(-0.2f, 30.0f, &p).alpha == Catch::Approx(1.0f));
}

TEST_CASE("cursor_soften: ghost offset = largest NEIGHBOUR-view hotspot distance, in panel px")
{
	u_cursor_draw_request req{};
	req.view_count = 2;
	req.view[0].origin[0] = 0.50f;
	req.view[0].origin[1] = 0.40f;
	req.view[1].origin[0] = 0.51f; // 0.01 of a 1920 px canvas = 19.2 px
	req.view[1].origin[1] = 0.40f;
	CHECK(u_cursor_draw_ghost_px(&req, 1920.0f, 1080.0f) == Catch::Approx(19.2f));
	// Same view positions = on the glass: no ghost.
	req.view[1].origin[0] = 0.50f;
	CHECK(u_cursor_draw_ghost_px(&req, 1920.0f, 1080.0f) == 0.0f);
	// Vertical component counts with the tile's height.
	req.view[1].origin[1] = 0.41f;
	CHECK(u_cursor_draw_ghost_px(&req, 1920.0f, 1000.0f) == Catch::Approx(10.0f));
	// Four views: neighbours only (0-3 spans three gaps), the largest wins.
	req.view_count = 4;
	for (int i = 0; i < 4; i++) {
		req.view[i].origin[1] = 0.4f;
	}
	req.view[0].origin[0] = 0.50f;
	req.view[1].origin[0] = 0.51f;
	req.view[2].origin[0] = 0.53f;
	req.view[3].origin[0] = 0.54f;
	CHECK(u_cursor_draw_ghost_px(&req, 1000.0f, 1000.0f) == Catch::Approx(20.0f));
	// No canvas size, no ghost; one view, no ghost.
	CHECK(u_cursor_draw_ghost_px(&req, 0.0f, 1000.0f) == 0.0f);
	req.view_count = 1;
	CHECK(u_cursor_draw_ghost_px(&req, 1000.0f, 1000.0f) == 0.0f);
	CHECK(u_cursor_draw_ghost_px(nullptr, 1000.0f, 1000.0f) == 0.0f);
}

TEST_CASE("cursor_soften: the padded quad grows by the blur radius, hotspot fixed")
{
	// A 20 x 40 px sprite in a 1000 x 1000 px tile, hotspot at its top-left.
	u_cursor_draw_view v{};
	v.origin[0] = 0.5f;
	v.origin[1] = 0.5f;
	v.axis_x[0] = 1.0f;  // one unit = the tile, display right = tile right
	v.axis_y[1] = -1.0f; // display up = tile up
	float xy[4][2];
	u_cursor_draw_corners(&v, 0.04f, 0.5f, 0.0f, 0.0f, xy);

	u_cursor_soften_quad q;
	// No blur: unpadded, standard uv, radius 0.
	u_cursor_draw_soften_quad(xy, 1000.0f, 1000.0f, 0.0f, &q);
	CHECK(q.radius_uv[0] == 0.0f);
	CHECK(q.radius_uv[1] == 0.0f);
	for (int k = 0; k < 4; k++) {
		CHECK(q.corners[k][0] == xy[k][0]);
		CHECK(q.corners[k][1] == xy[k][1]);
	}
	CHECK((q.uv[0][0] == 0.0f && q.uv[0][1] == 0.0f && q.uv[3][0] == 1.0f && q.uv[3][1] == 1.0f));

	// 2 px blur: radius = 2 / 20 in u, 2 / 40 in v; every edge moves 2 px out.
	u_cursor_draw_soften_quad(xy, 1000.0f, 1000.0f, 2.0f, &q);
	CHECK(q.radius_uv[0] == Catch::Approx(0.1f));
	CHECK(q.radius_uv[1] == Catch::Approx(0.05f));
	CHECK(q.uv[0][0] == Catch::Approx(-0.1f));
	CHECK(q.uv[0][1] == Catch::Approx(-0.05f));
	CHECK(q.uv[3][0] == Catch::Approx(1.1f));
	CHECK(q.uv[3][1] == Catch::Approx(1.05f));
	// Top-left corner moved 2 px left and 2 px up (tile y down).
	CHECK(q.corners[0][0] == Catch::Approx(xy[0][0] - 0.002f));
	CHECK(q.corners[0][1] == Catch::Approx(xy[0][1] - 0.002f));
	// Bottom-right moved 2 px right and down.
	CHECK(q.corners[3][0] == Catch::Approx(xy[3][0] + 0.002f));
	CHECK(q.corners[3][1] == Catch::Approx(xy[3][1] + 0.002f));
	// The uv -> position map is unchanged: uv (0, 0) - the hotspot here -
	// still lands on the view's origin (the click point does not move).
	const float u0 = -q.uv[0][0] / (q.uv[2][0] - q.uv[0][0]);
	const float v0 = -q.uv[0][1] / (q.uv[1][1] - q.uv[0][1]);
	const float hx =
	    q.corners[0][0] + u0 * (q.corners[2][0] - q.corners[0][0]) + v0 * (q.corners[1][0] - q.corners[0][0]);
	const float hy =
	    q.corners[0][1] + u0 * (q.corners[2][1] - q.corners[0][1]) + v0 * (q.corners[1][1] - q.corners[0][1]);
	CHECK(hx == Catch::Approx(0.5f));
	CHECK(hy == Catch::Approx(0.5f));

	// A degenerate quad is returned unpadded.
	float flat[4][2] = {{0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}};
	u_cursor_draw_soften_quad(flat, 1000.0f, 1000.0f, 2.0f, &q);
	CHECK(q.radius_uv[0] == 0.0f);
}

TEST_CASE("cursor_soften: the blur cap is proportional to the DRAWN sprite height")
{
	u_cursor_soften_params p;
	u_cursor_soften_params_default(&p);
	// A big ghost: the blur sits at its cap.
	const float ghost = 200.0f;
	// 0.06 x 64 px (the mirrored arrow at 240 DPI) = 3.84 px - David's ~4 px.
	CHECK(u_cursor_draw_soften_sized(-0.05f, ghost, 64.0f, &p).blur_px == Catch::Approx(3.84f));
	// The same cursor at 96 DPI (~25.6 px) gets the same blur RELATIVE to its size.
	CHECK(u_cursor_draw_soften_sized(-0.05f, ghost, 25.6f, &p).blur_px == Catch::Approx(1.536f));
	// A huge accessibility pointer is held at the hard ceiling.
	CHECK(u_cursor_draw_soften_sized(-0.05f, ghost, 256.0f, &p).blur_px == Catch::Approx(p.blur_max_px));
	// Below the cap it is still k x ghost.
	CHECK(u_cursor_draw_soften_sized(-0.05f, 20.0f, 64.0f, &p).blur_px == Catch::Approx(2.0f));
	// Unknown size: only the ceiling (the unsized entry point is exactly that).
	CHECK(u_cursor_draw_soften_sized(-0.05f, ghost, 0.0f, &p).blur_px == Catch::Approx(p.blur_max_px));
	CHECK(u_cursor_draw_soften_sized(-0.05f, ghost, NAN, &p).blur_px ==
	      u_cursor_draw_soften(-0.05f, ghost, &p).blur_px);
	// frac = 0 turns the proportional cap off.
	p.blur_max_frac = 0.0f;
	CHECK(u_cursor_draw_soften_sized(-0.05f, ghost, 64.0f, &p).blur_px == Catch::Approx(p.blur_max_px));
	// On the glass: nothing, whatever the size.
	u_cursor_soften_params_default(&p);
	const u_cursor_soften g = u_cursor_draw_soften_sized(0.0f, 0.0f, 64.0f, &p);
	CHECK((g.blur_px == 0.0f && g.alpha == 1.0f));
}

TEST_CASE("cursor_soften: the default opacity reaches 0.5 at the typical lift (0.03)")
{
	u_cursor_soften_params p;
	u_cursor_soften_params_default(&p);
	CHECK(u_cursor_draw_soften(-0.03f, 0.0f, &p).alpha == Catch::Approx(0.5f));
	CHECK(u_cursor_draw_soften(-0.015f, 0.0f, &p).alpha == Catch::Approx(0.75f)); // smoothstep midpoint
	CHECK(u_cursor_draw_soften(-0.2f, 0.0f, &p).alpha == Catch::Approx(0.5f));
}
