// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-058 conformance (ADR-054 Amendment 1's tracking -> conversion
 *         -> weave statement), headless, on the CPU reference.
 *
 * A provider produces a result every 15th display frame (4 fps against a
 * 60 Hz display), each one 15 frames (250 ms) after the frame whose eyes S3
 * snapped for it, so a shown result is 250-483 ms old — the shape of the open provider on Apple Silicon (264–386
 * ms per composed frame). A scripted eye path runs for two seconds. For every
 * display frame that has a result to show, three pictures of the SAME latest
 * content are compared:
 *
 *   GT    direct synthesis at this frame's own eyes;
 *   (i)   today: the result as converted, placed for its conversion eyes;
 *   (ii)  ADR-058: late synthesis of the latest image + depth at this
 *         frame's eyes (u_lift_reproject_render).
 *
 * Asserted, on every frame within the 0.10 m correction limit (frames past
 * it are eased toward the plane on purpose and only counted): (ii) matches GT (it is the same synthesis on the same
 * inputs, so the tolerance is 0 -- what this pins is that the ring's source + the frame's eyes are all late placement
 * needs), and (i) does not whenever the eyes moved in a way the synthesis depends on.
 *
 * Content is static, so every error measured is PLACEMENT error, never
 * content age. What the synthesis depends on (u_lift_reproject.h): both
 * geometries put the source camera at the viewpoints' midpoint and
 * x_s = p' + (E - C) q, so a common translation of the eyes changes nothing
 * except, on the COMPOSED path, through the budget's 1 / distance scaling;
 * a head turn (the eyes' spread and depth offset) changes every view. Runs:
 *
 *   - COMPOSED, sway + lean + head turn: (i) is off whenever the distance or
 *     the turn moved; (ii) is exact.
 *   - COMPOSED, lateral only: (i) is already exact -- pinned, because it
 *     bounds what late placement buys: lateral swim of a lifted result does
 *     not exist in today's geometry.
 *   - DISPLAY (metric depth + depthToDisplay), sway + lean + head turn: only
 *     the turn matters (the metric placement is relative to C, distance
 *     included); (ii) is exact.
 *
 * The per-run numbers (mean / max |error| in 1/255 over RGB of both views,
 * along the path) print on stdout.
 *
 * @ingroup tests
 */

#include "catch_amalgamated.hpp"

#include "lift_late_scene.hpp"

#include "util/u_lift_depth.h"
#include "util/u_lift_reproject.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

using namespace lift_late;

namespace {

constexpr uint32_t kW = 128, kH = 72;  // one view
constexpr uint32_t kDW = 32, kDH = 18; // depth grid
constexpr int kHz = 60;
constexpr int kPeriod = 15;  // provider: one result per 15 display frames = 4 fps
constexpr int kLatency = 15; // result visible 250 ms after its eyes were snapped
constexpr int kFrames = 2 * kHz;
constexpr double kPi = 3.14159265358979323846;

//! Eyes at time t (rect-relative metres).
using EyePath = std::function<void(double t, float eyes[2][3])>;

struct RunStats
{
	const char *name;
	int frames = 0;
	int moved = 0;       //!< frames whose eyes differ from the result's
	int eased = 0;       //!< frames beyond the 0.10 m limit (disparity eased, not compared)
	double i_mean = 0.0; //!< mean over frames of the per-frame mean error, (i)
	double i_max = 0.0;
	double ii_mean = 0.0;
	double ii_max = 0.0;
	double i_over4_mean = 0.0, i_over4_max = 0.0; //!< share of texels off by > 4/255, (i)
	double max_delta_m = 0.0;
};

/*!
 * One conformance run. @p make_source fills a source whose RECORDED
 * viewpoints are @p eyes (the conversion's S3 snapshot).
 */
RunStats
run(const char *name,
    const EyePath &path,
    const std::function<void(const float eyes[2][3], u_lift_reproject_source *)> &make_source,
    const std::function<bool(const u_lift_reproject_delta &)> &must_differ)
{
	RunStats rs;
	rs.name = name;
	u_lift_reproject_policy pol;
	u_lift_reproject_policy_defaults(&pol);
	const size_t n = (size_t)2 * kW * kH * 4;
	std::vector<float> today(n), late(n), gt(n);
	int today_for = -1;
	double i_sum = 0.0, ii_sum = 0.0;

	for (int f = kLatency; f < kFrames; f++) {
		// The provider: conversions start at multiples of kPeriod; the newest
		// one finished by frame f is the one that started kLatency earlier.
		const int r = ((f - kLatency) / kPeriod) * kPeriod;
		float e_r[2][3], e_f[2][3];
		path((double)r / kHz, e_r);
		path((double)f / kHz, e_f);

		u_lift_reproject_source src{};
		make_source(e_r, &src);
		if (today_for != r) {
			// (i) what the conversion thread published: views at e_r.
			u_lift_reproject_decision d0;
			REQUIRE(u_lift_reproject_render(&pol, &src, e_r, 2, kW, kH, today.data(), &d0) ==
			        U_LIFT_REPROJECT_PLACE);
			today_for = r;
		}
		// GT: the same content synthesized directly at this frame's eyes,
		// from a source recorded AT those eyes (no late step involved).
		u_lift_reproject_source direct{};
		make_source(e_f, &direct);
		u_lift_reproject_decision dg;
		REQUIRE(u_lift_reproject_render(&pol, &direct, e_f, 2, kW, kH, gt.data(), &dg) ==
		        U_LIFT_REPROJECT_PLACE);
		REQUIRE(dg.delta.max_m == 0.0f);
		// (ii) late synthesis of the ring's source at this frame's eyes.
		u_lift_reproject_decision dl;
		const u_lift_reproject_verdict v =
		    u_lift_reproject_render(&pol, &src, e_f, 2, kW, kH, late.data(), &dl);
		REQUIRE(v != U_LIFT_REPROJECT_HOLD);
		rs.max_delta_m = dl.delta.max_m > rs.max_delta_m ? dl.delta.max_m : rs.max_delta_m;
		if (v == U_LIFT_REPROJECT_EASE) {
			// Beyond the limit the policy flattens on purpose: not a placement
			// to compare with the direct synthesis.
			CHECK(dl.delta.max_m > pol.max_m);
			rs.eased++;
			continue;
		}

		const Err ei = diff(today, gt);
		const Err eii = diff(late, gt);
		rs.frames++;
		i_sum += ei.mean;
		rs.i_over4_mean += ei.over4;
		rs.i_over4_max = ei.over4 > rs.i_over4_max ? ei.over4 : rs.i_over4_max;
		ii_sum += eii.mean;
		rs.i_max = ei.max > rs.i_max ? ei.max : rs.i_max;
		rs.ii_max = eii.max > rs.ii_max ? eii.max : rs.ii_max;

		INFO(name << " frame " << f << " (result of frame " << r << "): delta " << dl.delta.max_m * 100.0
		          << " cm, (i) mean " << ei.mean << " max " << ei.max << ", (ii) mean " << eii.mean << " max "
		          << eii.max);
		CHECK(eii.max == 0.0); // (ii) is the direct synthesis
		if (must_differ(dl.delta)) {
			rs.moved++;
			CHECK(ei.mean > 0.05); // (i) visibly misplaced
		}
	}
	rs.i_mean = rs.frames > 0 ? i_sum / rs.frames : 0.0;
	rs.ii_mean = rs.frames > 0 ? ii_sum / rs.frames : 0.0;
	rs.i_over4_mean = rs.frames > 0 ? rs.i_over4_mean / rs.frames : 0.0;
	std::printf(
	    "[ADR-058 conformance] %-36s %3d frames compared (%3d moved, %2d eased past the limit, max eye "
	    "delta %.1f cm): (i) today mean %.3f max %.1f /255, texels > 4/255 %.2f%% avg %.2f%% worst | "
	    "(ii) late mean %.3f max %.1f /255\n",
	    rs.name, rs.frames, rs.moved, rs.eased, rs.max_delta_m * 100.0, rs.i_mean, rs.i_max,
	    100.0 * rs.i_over4_mean, 100.0 * rs.i_over4_max, rs.ii_mean, rs.ii_max);
	return rs;
}

// A viewer swaying +-6 cm at 0.5 Hz, leaning in and out +-10 cm at 0.35 Hz
// and turning the head +-20 degrees at 0.3 Hz.
void
sway_lean_turn(double t, float e[2][3])
{
	const float c[3] = {(float)(0.06 * std::sin(2.0 * kPi * 0.5 * t)),
	                    (float)(0.01 * std::sin(2.0 * kPi * 0.8 * t)),
	                    (float)(0.60 + 0.10 * std::sin(2.0 * kPi * 0.35 * t))};
	eye_pair_yaw(c, 0.35 * std::sin(2.0 * kPi * 0.3 * t), e);
}

void
lateral_only(double t, float e[2][3])
{
	const float c[3] = {(float)(0.06 * std::sin(2.0 * kPi * 0.5 * t)),
	                    (float)(0.02 * std::sin(2.0 * kPi * 0.8 * t)), 0.60f};
	eye_pair(c, e);
}

//! The eyes' relative change (a head turn) beyond 2 mm.
bool
spread_moved(const u_lift_reproject_delta &d)
{
	double s = 0.0;
	for (int k = 0; k < 3; k++) {
		const double r = (double)d.d_m[1][k] - (double)d.d_m[0][k];
		s += r * r;
	}
	return std::sqrt(s) > 0.002;
}

//! The pair's distance changed beyond 1 cm.
bool
distance_moved(const u_lift_reproject_delta &d)
{
	return std::fabs(0.5 * ((double)d.d_m[0][2] + (double)d.d_m[1][2])) > 0.01;
}

struct Scene
{
	std::vector<float> color = make_color(kW, kH);
	std::vector<float> disp = make_disparity(kDW, kDH);
	std::vector<float> metric = make_metric(kDW, kDH, 1.8f, 2.0f, 2.4f);
};

} // namespace

TEST_CASE("ADR-058 conformance: composed stream, 4 fps provider, sway + lean + turn", "[lift_late][conformance]")
{
	Scene s;
	auto make = [&](const float eyes[2][3], u_lift_reproject_source *src) {
		src->geometry = U_LIFT_REPROJECT_COMPOSED;
		src->params = composed_params(eyes, 0.04f);
		src->color = s.color.data();
		src->color_w = kW;
		src->color_h = kH;
		src->depth = s.disp.data();
		src->depth_w = kDW;
		src->depth_h = kDH;
	};
	// Composed S5 depends on the eyes' distance (the budget) and the turn,
	// not on a common lateral shift.
	auto moved = [](const u_lift_reproject_delta &d) { return distance_moved(d) || spread_moved(d); };
	const RunStats rs = run("composed, sway + lean + turn", sway_lean_turn, make, moved);
	CHECK(rs.moved > rs.frames / 2);
	CHECK(rs.ii_max == 0.0);
	CHECK(rs.i_mean > 0.1);
}

TEST_CASE("ADR-058 conformance: composed stream, lateral motion alone is invariant", "[lift_late][conformance]")
{
	Scene s;
	auto make = [&](const float eyes[2][3], u_lift_reproject_source *src) {
		src->geometry = U_LIFT_REPROJECT_COMPOSED;
		src->params = composed_params(eyes, 0.04f);
		src->color = s.color.data();
		src->color_w = kW;
		src->color_h = kH;
		src->depth = s.disp.data();
		src->depth_w = kDW;
		src->depth_h = kDH;
	};
	auto never = [](const u_lift_reproject_delta &) { return false; };
	const RunStats rs = run("composed, lateral only", lateral_only, make, never);
	CHECK(rs.max_delta_m > 0.02); // the eyes did move
	CHECK(rs.i_mean < 0.01);      // ... and today's result is still right (float rounding of E - C only)
	CHECK(rs.ii_max == 0.0);
}

TEST_CASE("ADR-058 conformance: display-placed metric depth, 4 fps provider, sway + lean + turn",
          "[lift_late][conformance]")
{
	Scene s;
	auto make = [&](const float eyes[2][3], u_lift_reproject_source *src) {
		src->geometry = U_LIFT_REPROJECT_DISPLAY;
		u_conv_viewsyn_params_defaults(&src->params);
		src->params.upsample = false;
		src->params.snap = false;
		src->params.depth.kind = XRT_CONV_DEPTH_METRIC;
		src->params.view_count = 2;
		std::memcpy(src->params.viewpoints_m, eyes, sizeof(float) * 6);
		const float c[3] = {0.5f * (eyes[0][0] + eyes[1][0]), 0.5f * (eyes[0][1] + eyes[1][1]),
		                    0.5f * (eyes[0][2] + eyes[1][2])};
		const float rc[3] = {0.0f, 0.0f, 0.0f}; // the provider built it for these eyes
		src->display.transform_valid =
		    u_lift_depth_to_display(c, rc, kRectW, kRectH, 2.0f, src->display.depth_to_display);
		src->display.rect_w_m = kRectW;
		src->display.rect_h_m = kRectH;
		src->display.near_m = 1.8f;
		src->display.far_m = 2.4f;
		src->color = s.color.data();
		src->color_w = kW;
		src->color_h = kH;
		src->depth = s.metric.data();
		src->depth_w = kDW;
		src->depth_h = kDH;
	};
	// Metric placement is relative to C: only the turn changes the views.
	const RunStats rs = run("display (metric), sway + lean + turn", sway_lean_turn, make, spread_moved);
	CHECK(rs.moved > rs.frames / 2);
	CHECK(rs.ii_max == 0.0);
	CHECK(rs.i_mean > 0.1);
}
