// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-058 phase 1 groundwork: u_lift_reproject (viewpoint delta,
 *         correction limit + easing, late synthesis on the CPU reference).
 *
 *  - delta = 0 -> late synthesis is the reference render, bit for bit, and
 *    the reference render is the march the GPU stages are pinned to;
 *  - a known lateral move on a metric plane -> the pixel shift the
 *    depthToDisplay geometry predicts (dx * q / W, q = dc / d - 1);
 *  - beyond the limit -> ease non-increasing, rendered disparity
 *    non-increasing, views identical at twice the limit;
 *  - RELATIVE depth (or no transform, or an unknown kind) -> HOLD.
 *
 * @ingroup tests
 */

#include "catch_amalgamated.hpp"

#include "lift_late_scene.hpp"

#include "util/u_lift_depth.h"
#include "util/u_lift_reproject.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace lift_late;

namespace {

constexpr uint32_t kW = 160, kH = 90;  // colour = one output view
constexpr uint32_t kDW = 40, kDH = 24; // depth grid

u_lift_reproject_policy
default_policy()
{
	u_lift_reproject_policy p;
	u_lift_reproject_policy_defaults(&p);
	return p;
}

//! A DISPLAY source over a metric map, viewpoint midpoint @p c (rect-relative).
struct DisplayScene
{
	std::vector<float> color, depth;
	u_lift_reproject_source src{};
	float rect_center[3] = {0.05f, -0.02f, 0.0f};

	DisplayScene(const float eyes[2][3], float dc, std::vector<float> metric, float near_m, float far_m)
	{
		color = make_color(kW, kH);
		depth = std::move(metric);
		src.geometry = U_LIFT_REPROJECT_DISPLAY;
		u_conv_viewsyn_params_defaults(&src.params);
		src.params.upsample = false;
		src.params.snap = false;
		src.params.depth.kind = XRT_CONV_DEPTH_METRIC;
		src.params.view_count = 2;
		float c[3] = {0, 0, 0};
		for (int i = 0; i < 2; i++) {
			for (int k = 0; k < 3; k++) {
				src.params.viewpoints_m[i][k] = eyes[i][k];
				c[k] += 0.5f * eyes[i][k];
			}
		}
		src.display.transform_valid =
		    u_lift_depth_to_display(c, rect_center, kRectW, kRectH, dc, src.display.depth_to_display);
		src.display.rect_w_m = kRectW;
		src.display.rect_h_m = kRectH;
		src.display.near_m = near_m;
		src.display.far_m = far_m;
		src.color = color.data();
		src.color_w = kW;
		src.color_h = kH;
		src.depth = depth.data();
		src.depth_w = kDW;
		src.depth_h = kDH;
	}
};

//! Mean of R over a band of the centre row, in source-u units.
double
mean_r(const std::vector<float> &views, uint32_t view, uint32_t x0, uint32_t x1, uint32_t y)
{
	const float *v = views.data() + (size_t)view * kW * kH * 4;
	double s = 0.0;
	for (uint32_t x = x0; x < x1; x++) {
		s += v[((size_t)y * kW + x) * 4];
	}
	return s / (double)(x1 - x0);
}

} // namespace

TEST_CASE("lift reproject: viewpoint delta", "[lift_reproject]")
{
	const float rec[2][3] = {{-0.03f, 0.0f, 0.6f}, {0.03f, 0.0f, 0.6f}};
	const float cur[2][3] = {{0.0f, 0.04f, 0.6f}, {0.06f, 0.0f, 0.5f}};
	u_lift_reproject_delta d;
	REQUIRE(u_lift_reproject_compute_delta(rec, cur, 2, &d));
	CHECK(d.view_count == 2);
	CHECK(d.d_m[0][0] == Catch::Approx(0.03f));
	CHECK(d.d_m[0][1] == Catch::Approx(0.04f));
	CHECK(d.len_m[0] == Catch::Approx(0.05f));
	CHECK(d.len_m[1] == Catch::Approx(std::sqrt(0.03 * 0.03 + 0.1 * 0.1)));
	CHECK(d.max_m == Catch::Approx(d.len_m[1]));

	REQUIRE(u_lift_reproject_compute_delta(rec, rec, 2, &d));
	CHECK(d.max_m == 0.0f);

	CHECK_FALSE(u_lift_reproject_compute_delta(rec, cur, 0, &d));
	CHECK_FALSE(u_lift_reproject_compute_delta(rec, cur, U_LIFT_REPROJECT_MAX_VIEWS + 1, &d));
	const float bad[2][3] = {{NAN, 0, 0.6f}, {0, 0, 0.6f}};
	CHECK_FALSE(u_lift_reproject_compute_delta(rec, bad, 2, &d));
	CHECK(d.view_count == 0);
}

TEST_CASE("lift reproject: correction limit + easing policy", "[lift_reproject]")
{
	u_lift_reproject_policy p = default_policy();
	CHECK(p.max_m == U_LIFT_REPROJECT_MAX_M_DEFAULT);
	CHECK(U_LIFT_REPROJECT_MAX_M_DEFAULT == 0.10f);

	CHECK_FALSE(u_lift_reproject_policy_set_max_m(&p, 0.0f));
	CHECK_FALSE(u_lift_reproject_policy_set_max_m(&p, -1.0f));
	CHECK_FALSE(u_lift_reproject_policy_set_max_m(&p, NAN));
	CHECK_FALSE(u_lift_reproject_policy_set_max_m(&p, INFINITY));
	CHECK(p.max_m == U_LIFT_REPROJECT_MAX_M_DEFAULT);
	REQUIRE(u_lift_reproject_policy_set_max_m(&p, 0.05f));
	CHECK(p.max_m == 0.05f);
	p = default_policy();

	CHECK(u_lift_reproject_ease(&p, 0.0f) == 1.0f);
	CHECK(u_lift_reproject_ease(&p, 0.10f) == 1.0f);
	CHECK(u_lift_reproject_ease(&p, 0.20f) == 0.0f);
	CHECK(u_lift_reproject_ease(&p, 1.0f) == 0.0f);
	CHECK(u_lift_reproject_ease(&p, NAN) == 0.0f);
	CHECK(u_lift_reproject_ease(&p, 0.15f) == Catch::Approx(0.5f));
	float prev = 1.0f;
	for (int i = 0; i <= 400; i++) {
		const float e = u_lift_reproject_ease(&p, 0.001f * (float)i);
		INFO("delta " << 0.001f * i << " ease " << e);
		CHECK(e >= 0.0f);
		CHECK(e <= 1.0f);
		CHECK(e <= prev);
		CHECK(prev - e < 0.02f); // continuous: no jump at the limit
		prev = e;
	}
}

TEST_CASE("lift reproject: delta 0 is the reference render bit for bit", "[lift_reproject]")
{
	const std::vector<float> color = make_color(kW, kH);
	const std::vector<float> disp = make_disparity(kDW, kDH);
	const float c[3] = {0.0f, 0.0f, 0.6f};
	float eyes[2][3];
	eye_pair(c, eyes);
	u_lift_reproject_source src{};
	src.geometry = U_LIFT_REPROJECT_COMPOSED;
	src.params = composed_params(eyes, 0.04f);
	src.color = color.data();
	src.color_w = kW;
	src.color_h = kH;
	src.depth = disp.data();
	src.depth_w = kDW;
	src.depth_h = kDH;

	std::vector<float> ref((size_t)2 * kW * kH * 4, -1.0f), late(ref.size(), -2.0f);
	REQUIRE(
	    u_conv_viewsyn_ref_render(&src.params, color.data(), kW, kH, disp.data(), kDW, kDH, kW, kH, ref.data()));
	u_lift_reproject_policy pol = default_policy();
	u_lift_reproject_decision dec;
	REQUIRE(u_lift_reproject_render(&pol, &src, eyes, 2, kW, kH, late.data(), &dec) == U_LIFT_REPROJECT_PLACE);
	CHECK(dec.ease == 1.0f);
	CHECK(dec.delta.max_m == 0.0f);
	CHECK(std::memcmp(ref.data(), late.data(), ref.size() * sizeof(float)) == 0);

	// The reference render IS the march the GPU stages are pinned to: R / G
	// encode the source uv, so interior texels read the march's hit back.
	u_conv_viewsyn_frame fr;
	REQUIRE(u_conv_viewsyn_resolve(&src.params, &fr));
	struct Depth
	{
		const std::vector<float> *d;
		const u_conv_viewsyn_frame *f;
		const u_conv_viewsyn_params *p;
	} dd{&disp, &fr, &src.params};
	auto sample = [](void *user, double u, double v) -> double {
		auto *s = static_cast<Depth *>(user);
		int x = (int)std::floor(u * kDW), y = (int)std::floor((1.0 - v) * kDH);
		x = x < 0 ? 0 : (x > (int)kDW - 1 ? (int)kDW - 1 : x);
		y = y < 0 ? 0 : (y > (int)kDH - 1 ? (int)kDH - 1 : y);
		return u_conv_viewsyn_normalise(&s->p->depth, s->f->near_lo, s->f->near_hi,
		                                (*s->d)[(size_t)y * kDW + x]);
	};
	uint32_t checked = 0;
	for (uint32_t i = 0; i < 2; i++) {
		for (uint32_t y = 4; y < kH - 4; y += 7) {
			for (uint32_t x = 8; x < kW - 8; x += 5) {
				const double uv[2] = {(x + 0.5) / kW, 1.0 - (y + 0.5) / kH};
				double s[2];
				(void)u_conv_viewsyn_march(&fr, &fr.views[i], sample, &dd, uv, s);
				if (s[0] < 0.5 / kW || s[0] > 1.0 - 0.5 / kW || s[1] < 0.5 / kH ||
				    s[1] > 1.0 - 0.5 / kH) {
					continue; // clamp-to-edge region: R / G saturate
				}
				const float *px = &ref[(((size_t)i * kH + y) * kW + x) * 4];
				CHECK(std::fabs(px[0] - s[0]) < 1e-5);
				CHECK(std::fabs(px[1] - s[1]) < 1e-5);
				checked++;
			}
		}
	}
	CHECK(checked > 500);
}

TEST_CASE("lift reproject: lateral moves on a metric plane shift by (E - C) * q / W", "[lift_reproject]")
{
	const float dc = 2.0f, d = 1.6f; // plane in front of the glass: q = dc / d - 1 = 0.25
	const double q = dc / d - 1.0;
	const float c[3] = {0.0f, 0.0f, 0.6f};
	float eyes[2][3];
	eye_pair(c, eyes);
	DisplayScene s(eyes, dc, std::vector<float>((size_t)kDW * kDH, d), d, d);
	REQUIRE(s.src.display.transform_valid);

	// The transform round-trips: R and dc come back out of the matrix, and
	// the source camera is the viewpoints' midpoint.
	u_conv_viewsyn_params p;
	float rc[3];
	REQUIRE(u_lift_reproject_display_params(&s.src.params, &s.src.display, eyes, 2, &p, rc));
	for (int k = 0; k < 3; k++) {
		CHECK(rc[k] == Catch::Approx(s.rect_center[k]).margin(1e-5));
	}
	CHECK_FALSE(p.has_source_camera);
	CHECK(p.convergence == Catch::Approx(dc));

	u_lift_reproject_policy pol = default_policy();
	std::vector<float> before((size_t)2 * kW * kH * 4), after(before.size());
	u_lift_reproject_decision dec;
	REQUIRE(u_lift_reproject_render(&pol, &s.src, eyes, 2, kW, kH, before.data(), &dec) == U_LIFT_REPROJECT_PLACE);

	// Away from the edge taper the texel at output u shows source
	// u + (E.x - C.x) * q / W: the metric placement depthToDisplay defines.
	const uint32_t y = kH / 2, x0 = kW / 4, x1 = kW * 3 / 4;
	const double u_out = (x0 + x1) * 0.5 / kW;
	for (uint32_t i = 0; i < 2; i++) {
		const double want = u_out + (eyes[i][0] - c[0]) * q / kRectW;
		CHECK(std::fabs(mean_r(before, i, x0, x1, y) - want) < 2e-4);
	}

	const float dx = 0.04f; // within the 0.10 m limit
	SECTION("the whole pair moves: every view is unchanged")
	{
		float moved[2][3];
		std::memcpy(moved, eyes, sizeof(moved));
		moved[0][0] += dx;
		moved[1][0] += dx;
		REQUIRE(u_lift_reproject_render(&pol, &s.src, moved, 2, kW, kH, after.data(), &dec) ==
		        U_LIFT_REPROJECT_PLACE);
		CHECK(dec.delta.max_m == Catch::Approx(dx));
		for (uint32_t i = 0; i < 2; i++) {
			CHECK(std::fabs(mean_r(after, i, x0, x1, y) - mean_r(before, i, x0, x1, y)) < 2e-4);
		}
	}
	SECTION("one eye moves out: the views spread by -/+ dx/2 * q / W")
	{
		float moved[2][3];
		std::memcpy(moved, eyes, sizeof(moved));
		moved[1][0] += dx; // IPD + 4 cm: the midpoint moves 2 cm, each view 2 cm off it
		REQUIRE(u_lift_reproject_render(&pol, &s.src, moved, 2, kW, kH, after.data(), &dec) ==
		        U_LIFT_REPROJECT_PLACE);
		CHECK(dec.delta.max_m == Catch::Approx(dx));
		const double half = 0.5 * dx * q / kRectW; // 2.5 px at 160 wide
		CHECK(half * kW == Catch::Approx(2.5));
		const double du0 = mean_r(after, 0, x0, x1, y) - mean_r(before, 0, x0, x1, y);
		const double du1 = mean_r(after, 1, x0, x1, y) - mean_r(before, 1, x0, x1, y);
		INFO("du0 " << du0 * kW << " px, du1 " << du1 * kW << " px, want -/+" << half * kW);
		CHECK(std::fabs(du0 + half) < 2e-4);
		CHECK(std::fabs(du1 - half) < 2e-4);
	}
	SECTION("the pair moves closer: parallax scales with the eye distance")
	{
		float moved[2][3];
		std::memcpy(moved, eyes, sizeof(moved));
		moved[0][2] -= dx;
		moved[1][2] -= dx;
		REQUIRE(u_lift_reproject_render(&pol, &s.src, moved, 2, kW, kH, after.data(), &dec) ==
		        U_LIFT_REPROJECT_PLACE);
		// Same eye offsets, same q: x_s = p' + (E - C) q holds at any distance,
		// so a plane's views do not change; the ray march sees the distance
		// through the slope of a non-planar map (the conformance test's box).
		for (uint32_t i = 0; i < 2; i++) {
			const double want = u_out + (eyes[i][0] - c[0]) * q / kRectW;
			CHECK(std::fabs(mean_r(after, i, x0, x1, y) - want) < 2e-4);
		}
	}
}

TEST_CASE("lift reproject: beyond the limit the disparity eases toward the plane", "[lift_reproject]")
{
	const std::vector<float> color = make_color(kW, kH);
	const std::vector<float> disp = make_disparity(kDW, kDH);
	const float c[3] = {0.0f, 0.0f, 0.6f};
	float eyes[2][3];
	eye_pair(c, eyes);
	u_lift_reproject_source src{};
	src.geometry = U_LIFT_REPROJECT_COMPOSED;
	src.params = composed_params(eyes, 0.04f);
	src.color = color.data();
	src.color_w = kW;
	src.color_h = kH;
	src.depth = disp.data();
	src.depth_w = kDW;
	src.depth_h = kDH;
	u_lift_reproject_policy pol = default_policy();

	// Disparity on the near box: R of view 1 - view 0 over its centre.
	auto box_disparity = [&](const std::vector<float> &v) {
		const uint32_t y = kH / 2;
		return mean_r(v, 1, kW * 7 / 16, kW * 9 / 16, y) - mean_r(v, 0, kW * 7 / 16, kW * 9 / 16, y);
	};
	std::vector<float> views((size_t)2 * kW * kH * 4);
	double prev_disp = INFINITY;
	float prev_ease = 2.0f;
	for (float m : {0.0f, 0.05f, 0.10f, 0.12f, 0.15f, 0.18f, 0.20f, 0.30f}) {
		float cur[2][3];
		std::memcpy(cur, eyes, sizeof(cur));
		for (int i = 0; i < 2; i++) {
			cur[i][0] += m * 0.6f; // a diagonal move of length m
			cur[i][2] += m * 0.8f;
		}
		u_lift_reproject_decision dec;
		const u_lift_reproject_verdict v =
		    u_lift_reproject_render(&pol, &src, cur, 2, kW, kH, views.data(), &dec);
		const double bd = std::fabs(box_disparity(views));
		INFO("move " << m << " m: ease " << dec.ease << " box disparity " << bd * kW << " px");
		CHECK(v == (m <= 0.10f ? U_LIFT_REPROJECT_PLACE : U_LIFT_REPROJECT_EASE));
		CHECK(dec.delta.max_m == Catch::Approx(m).margin(1e-5));
		CHECK(dec.ease <= prev_ease);
		if (m > 0.10f) {
			CHECK(bd <= prev_disp + 1e-6);
		}
		if (m >= 0.20f) {
			CHECK(dec.ease == 0.0f);
			// Flat on the plane: both views are the same picture (to the
			// float rounding of each eye's own ray).
			const size_t vf = (size_t)kW * kH * 4;
			const std::vector<float> v0(views.begin(), views.begin() + vf),
			    v1(views.begin() + vf, views.end());
			CHECK(diff(v0, v1).max < 1e-3);
		}
		prev_ease = dec.ease;
		prev_disp = bd;
	}
}

TEST_CASE("lift reproject: RELATIVE depth, no transform or an unknown kind HOLDS", "[lift_reproject]")
{
	const float c[3] = {0.0f, 0.0f, 0.6f};
	float eyes[2][3];
	eye_pair(c, eyes);
	u_lift_reproject_policy pol = default_policy();
	std::vector<float> out((size_t)2 * kW * kH * 4, 7.0f);
	auto untouched = [&] {
		for (float f : out) {
			if (f != 7.0f) {
				return false;
			}
		}
		return true;
	};
	float moved[2][3];
	std::memcpy(moved, eyes, sizeof(moved));
	moved[0][0] += 0.03f;
	moved[1][0] += 0.03f;

	DisplayScene metric(eyes, 2.0f, make_metric(kDW, kDH, 1.8f, 2.0f, 2.4f), 1.8f, 2.4f);
	REQUIRE(metric.src.display.transform_valid);

	// RELATIVE (affine) depth with an otherwise valid transform: no scale, no placement.
	for (uint32_t kind : {XRT_CONV_DEPTH_AFFINE_DISPARITY, XRT_CONV_DEPTH_AFFINE_INVERSE, XRT_CONV_DEPTH_AFFINE,
	                      XRT_CONV_DEPTH_STEREO_DISPARITY_PX, XRT_CONV_DEPTH_UNKNOWN}) {
		DisplayScene s = metric;
		s.src.color = s.color.data();
		s.src.depth = s.depth.data();
		s.src.params.depth.kind = kind;
		u_lift_reproject_decision dec;
		INFO("kind " << kind);
		CHECK(u_lift_reproject_render(&pol, &s.src, moved, 2, kW, kH, out.data(), &dec) ==
		      U_LIFT_REPROJECT_HOLD);
		CHECK(dec.verdict == U_LIFT_REPROJECT_HOLD);
		CHECK(dec.ease == 0.0f);
		CHECK(untouched());
	}
	{
		DisplayScene s = metric;
		s.src.color = s.color.data();
		s.src.depth = s.depth.data();
		s.src.display.transform_valid = false;
		CHECK(u_lift_reproject_render(&pol, &s.src, moved, 2, kW, kH, out.data(), nullptr) ==
		      U_LIFT_REPROJECT_HOLD);
		s.src.display.transform_valid = true;
		s.src.display.near_m = 0.0f; // nearDepth unknown
		CHECK(u_lift_reproject_render(&pol, &s.src, moved, 2, kW, kH, out.data(), nullptr) ==
		      U_LIFT_REPROJECT_HOLD);
		CHECK(untouched());
	}
	{
		// The same metric source places (the gate is the depth, not the move).
		DisplayScene s = metric;
		s.src.color = s.color.data();
		s.src.depth = s.depth.data();
		std::vector<float> o2(out.size());
		CHECK(u_lift_reproject_render(&pol, &s.src, moved, 2, kW, kH, o2.data(), nullptr) ==
		      U_LIFT_REPROJECT_PLACE);
		// METRIC_INVERSE (1 / metres) places identically to METRIC.
		std::vector<float> inv = s.depth;
		for (float &f : inv) {
			f = 1.0f / f;
		}
		s.src.depth = inv.data();
		s.src.params.depth.kind = XRT_CONV_DEPTH_METRIC_INVERSE;
		std::vector<float> o3(out.size());
		CHECK(u_lift_reproject_render(&pol, &s.src, moved, 2, kW, kH, o3.data(), nullptr) ==
		      U_LIFT_REPROJECT_PLACE);
		// Same placement; float rounding of 1 / d may move a silhouette texel
		// by one march step, so compare the mean.
		const Err e = diff(o2, o3);
		INFO("metric vs metric-inverse: mean " << e.mean << " max " << e.max << " /255");
		CHECK(e.mean < 0.05);
	}
	{
		// COMPOSED: an unknown kind and a view-count mismatch hold.
		const std::vector<float> color = make_color(kW, kH);
		const std::vector<float> disp = make_disparity(kDW, kDH);
		u_lift_reproject_source src{};
		src.geometry = U_LIFT_REPROJECT_COMPOSED;
		src.params = composed_params(eyes, 0.04f);
		src.color = color.data();
		src.color_w = kW;
		src.color_h = kH;
		src.depth = disp.data();
		src.depth_w = kDW;
		src.depth_h = kDH;
		CHECK(u_lift_reproject_render(&pol, &src, moved, 1, kW, kH, out.data(), nullptr) ==
		      U_LIFT_REPROJECT_HOLD);
		src.params.depth.kind = XRT_CONV_DEPTH_UNKNOWN;
		CHECK(u_lift_reproject_render(&pol, &src, moved, 2, kW, kH, out.data(), nullptr) ==
		      U_LIFT_REPROJECT_HOLD);
		// An eye at the glass cannot be placed.
		src.params.depth.kind = XRT_CONV_DEPTH_AFFINE_DISPARITY;
		float at_glass[2][3];
		std::memcpy(at_glass, moved, sizeof(at_glass));
		at_glass[0][2] = 0.0f;
		CHECK(u_lift_reproject_render(&pol, &src, at_glass, 2, kW, kH, out.data(), nullptr) ==
		      U_LIFT_REPROJECT_HOLD);
		CHECK(untouched());
	}
}

TEST_CASE("lift reproject: content age vs placement age", "[lift_reproject]")
{
	u_lift_reproject_ages a;
	u_lift_reproject_compute_ages(1000, 400, 900, &a);
	CHECK(a.content_age_ns == 600);
	CHECK(a.placement_age_ns == 100);
	u_lift_reproject_compute_ages(1000, 400, 0, &a);
	CHECK(a.placement_age_ns == 600);
	u_lift_reproject_compute_ages(1000, 1200, 1100, &a);
	CHECK(a.content_age_ns == 0);
	CHECK(a.placement_age_ns == 0);
}
