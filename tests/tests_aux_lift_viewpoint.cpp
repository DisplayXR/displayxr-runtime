// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift viewpoint policy (ADR-048): rebase, rig factors, axis
 *         mask, clamp and the per-stream recenter filter.
 *
 * What must hold:
 *  - the defaults keep v1's X-only look-around for the first second;
 *  - ipd / parallax follow the display-rig steps 1a / 1b;
 *  - masked axes are pinned (y = 0, z = nominal) and the clamp bounds x / y;
 *  - EASE_TO_CURRENT: nothing moves before the hold; after it the rendered
 *    offset decays with the time constant, independent of the frame rate;
 *  - EASE_TO_NEUTRAL decays the offset and restores look-around on return;
 *  - a reset (stream create / tracking loss) forgets the filter.
 */

#include "catch_amalgamated.hpp"

#include "util/u_lift_viewpoint.h"

#include <cmath>

namespace {

constexpr uint64_t kMs = 1000ull * 1000ull;
constexpr float kNz = 0.6f;

//! A symmetric eye pair around midpoint (x, y, z), 64 mm apart.
void
pair(float x, float y, float z, float out[6])
{
	out[0] = x - 0.032f;
	out[1] = y;
	out[2] = z;
	out[3] = x + 0.032f;
	out[4] = y;
	out[5] = z;
}

//! Run the policy at a fixed midpoint for @p seconds at @p hz; returns the last rendered midpoint x.
float
run(const u_lift_view_control &vc, u_lift_recenter &r, float x, float seconds, float hz, uint64_t &t)
{
	float in[6], out[6], mid[3] = {};
	pair(x, 0.0f, kNz, in);
	const int steps = (int)std::lround(seconds * hz);
	const uint64_t dt = (uint64_t)(1e9 / hz);
	for (int i = 0; i < steps; i++) {
		t += dt;
		u_lift_viewpoint_apply(&vc, &r, kNz, t, in, 2, out, nullptr, mid);
	}
	return mid[0];
}

} // namespace

TEST_CASE("lift viewpoint: rebase subtracts the rect centre", "[lift_viewpoint]")
{
	float p[6] = {0.10f, 0.02f, 0.6f, 0.164f, 0.02f, 0.6f};
	const float centre[3] = {0.10f, 0.02f, 0.0f};
	u_lift_viewpoint_rebase(p, 2, centre);
	CHECK(p[0] == Catch::Approx(0.0f).margin(1e-6));
	CHECK(p[3] == Catch::Approx(0.064f));
	CHECK(p[1] == Catch::Approx(0.0f).margin(1e-6));
	CHECK(p[2] == Catch::Approx(0.6f));
}

TEST_CASE("lift viewpoint: sanitize fills defaults", "[lift_viewpoint]")
{
	u_lift_view_control vc = {};
	vc.ipd_factor = 2.0f;
	vc.parallax_factor = -1.0f;
	vc.axis_mode = 0;
	vc.max_offset_m = -3.0f;
	vc.recenter_mode = 9;
	vc.hold_s = -1.0f;
	vc.tau_s = 0.0f;
	u_lift_view_control_sanitize(&vc);
	CHECK(vc.ipd_factor == 1.0f);
	CHECK(vc.parallax_factor == 0.0f);
	CHECK(vc.axis_mode == U_LIFT_AXIS_X);
	CHECK(vc.max_offset_m == 0.0f);
	CHECK(vc.recenter_mode == U_LIFT_RECENTER_EASE_TO_CURRENT);
	CHECK(vc.hold_s == U_LIFT_RECENTER_HOLD_DEFAULT_S);
	CHECK(vc.tau_s == U_LIFT_RECENTER_TAU_DEFAULT_S);
}

TEST_CASE("lift viewpoint: ipd and parallax factors (rig steps 1a / 1b)", "[lift_viewpoint]")
{
	u_lift_view_control vc;
	u_lift_view_control_default(&vc);
	vc.recenter_mode = U_LIFT_RECENTER_OFF;
	vc.axis_mode = U_LIFT_AXIS_XYZ;
	vc.ipd_factor = 0.5f;
	vc.parallax_factor = 0.5f;
	float in[6], out[6], base = 0.0f, mid[3];
	pair(0.10f, 0.04f, 0.8f, in);
	u_lift_viewpoint_apply(&vc, nullptr, kNz, 0, in, 2, out, &base, mid);
	CHECK(base == Catch::Approx(0.032f));
	CHECK(mid[0] == Catch::Approx(0.05f));
	CHECK(mid[1] == Catch::Approx(0.02f));
	CHECK(mid[2] == Catch::Approx(kNz + 0.5f * (0.8f - kNz)));
	CHECK(out[0] == Catch::Approx(0.05f - 0.016f));
	CHECK(out[3] == Catch::Approx(0.05f + 0.016f));

	vc.ipd_factor = 0.0f; // mono
	u_lift_viewpoint_apply(&vc, nullptr, kNz, 0, in, 2, out, &base, mid);
	CHECK(base == Catch::Approx(0.0f).margin(1e-6));
	CHECK(out[0] == Catch::Approx(out[3]));
}

TEST_CASE("lift viewpoint: axis mask and clamp", "[lift_viewpoint]")
{
	u_lift_view_control vc;
	u_lift_view_control_default(&vc);
	vc.recenter_mode = U_LIFT_RECENTER_OFF;
	float in[6], out[6], mid[3];
	pair(0.30f, 0.05f, 0.9f, in);

	vc.axis_mode = U_LIFT_AXIS_X;
	u_lift_viewpoint_apply(&vc, nullptr, kNz, 0, in, 2, out, nullptr, mid);
	CHECK(mid[0] == Catch::Approx(0.30f));
	CHECK(mid[1] == 0.0f);
	CHECK(mid[2] == Catch::Approx(kNz));

	vc.axis_mode = U_LIFT_AXIS_XY;
	u_lift_viewpoint_apply(&vc, nullptr, kNz, 0, in, 2, out, nullptr, mid);
	CHECK(mid[1] == Catch::Approx(0.05f));
	CHECK(mid[2] == Catch::Approx(kNz));

	vc.axis_mode = U_LIFT_AXIS_XYZ;
	u_lift_viewpoint_apply(&vc, nullptr, kNz, 0, in, 2, out, nullptr, mid);
	CHECK(mid[2] == Catch::Approx(0.9f));

	vc.max_offset_m = 0.1f;
	u_lift_viewpoint_apply(&vc, nullptr, kNz, 0, in, 2, out, nullptr, mid);
	CHECK(mid[0] == Catch::Approx(0.1f));
	CHECK(mid[1] == Catch::Approx(0.05f));
	// The eye separation survives the clamp.
	CHECK(out[3] - out[0] == Catch::Approx(0.064f));
}

TEST_CASE("lift viewpoint: EASE_TO_CURRENT holds, then decays with tau", "[lift_viewpoint]")
{
	u_lift_view_control vc;
	u_lift_view_control_default(&vc); // hold 1 s, tau 2 s
	u_lift_recenter r = {};
	uint64_t t = 1000 * kMs;

	// Before the hold: the full offset (v1 look-around).
	float x = run(vc, r, 0.10f, 0.9f, 60.0f, t);
	CHECK(x == Catch::Approx(0.10f));

	// Past the hold by one time constant: ~e^-1 of the offset left.
	run(vc, r, 0.10f, 0.1f, 60.0f, t);
	x = run(vc, r, 0.10f, 2.0f, 60.0f, t);
	CHECK(x == Catch::Approx(0.10f * std::exp(-1.0f)).margin(0.004f));

	// Long after: settled straight-on.
	x = run(vc, r, 0.10f, 20.0f, 60.0f, t);
	CHECK(std::fabs(x) < 0.002f);

	// A new head movement looks around immediately, relative to the new reference.
	x = run(vc, r, 0.15f, 1.0f / 60.0f, 60.0f, t);
	CHECK(x == Catch::Approx(0.05f).margin(0.003f));
}

TEST_CASE("lift viewpoint: the filter is frame-rate independent", "[lift_viewpoint]")
{
	u_lift_view_control vc;
	u_lift_view_control_default(&vc);
	u_lift_recenter a = {}, b = {};
	uint64_t ta = 0, tb = 0;
	float xa = run(vc, a, 0.10f, 3.0f, 30.0f, ta);
	float xb = run(vc, b, 0.10f, 3.0f, 120.0f, tb);
	CHECK(xa == Catch::Approx(xb).margin(0.003f));
}

TEST_CASE("lift viewpoint: a small offset never starts the hold", "[lift_viewpoint]")
{
	u_lift_view_control vc;
	u_lift_view_control_default(&vc);
	u_lift_recenter r = {};
	uint64_t t = 0;
	float x = run(vc, r, 0.003f, 10.0f, 60.0f, t);
	CHECK(x == Catch::Approx(0.003f));
}

TEST_CASE("lift viewpoint: EASE_TO_NEUTRAL decays and recovers", "[lift_viewpoint]")
{
	u_lift_view_control vc;
	u_lift_view_control_default(&vc);
	vc.recenter_mode = U_LIFT_RECENTER_EASE_TO_NEUTRAL;
	u_lift_recenter r = {};
	uint64_t t = 0;
	float x = run(vc, r, 0.10f, 0.5f, 60.0f, t);
	CHECK(x == Catch::Approx(0.10f));
	x = run(vc, r, 0.10f, 20.0f, 60.0f, t);
	CHECK(std::fabs(x) < 0.002f);
	// Back on axis for a while: look-around is restored.
	run(vc, r, 0.0f, 20.0f, 60.0f, t);
	x = run(vc, r, 0.10f, 1.0f / 60.0f, 60.0f, t);
	CHECK(x == Catch::Approx(0.10f).margin(0.003f));
}

TEST_CASE("lift viewpoint: reset forgets the reference", "[lift_viewpoint]")
{
	u_lift_view_control vc;
	u_lift_view_control_default(&vc);
	u_lift_recenter r = {};
	uint64_t t = 0;
	run(vc, r, 0.10f, 20.0f, 60.0f, t);
	u_lift_recenter_reset(&r);
	float x = run(vc, r, 0.10f, 1.0f / 60.0f, 60.0f, t);
	CHECK(x == Catch::Approx(0.10f));
}

TEST_CASE("lift viewpoint: OFF is the plain rect-relative offset", "[lift_viewpoint]")
{
	u_lift_view_control vc;
	u_lift_view_control_default(&vc);
	vc.recenter_mode = U_LIFT_RECENTER_OFF;
	u_lift_recenter r = {};
	uint64_t t = 0;
	float x = run(vc, r, 0.10f, 20.0f, 60.0f, t);
	CHECK(x == Catch::Approx(0.10f));
}
