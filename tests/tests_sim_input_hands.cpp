// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The sim_input hand script (#2191): placement relative to the display
 *         plane, the HANDS / PINCH / HAND_DISTANCE_M policy, and the pinch-hold
 *         tip gap — pure functions, no device, no plug-in, no display.
 *
 * The end-to-end counterpart (wrists located in LOCAL through the real
 * runtime, against the display plane xrLocateViews reports) is the
 * `[rig_hands]` arm of tests_oxr_view_space.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_defines.h"

#include "math/m_mathinclude.h"
#include "util/u_hand_simulation.h"

#include "sim_input_hand_script.h"

#include <cmath>
#include <cstdint>
#include <cstdio>


namespace {

constexpr int64_t kNsPerS = 1000000000LL;

float
dist(const xrt_vec3 &a, const xrt_vec3 &b)
{
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

//! Thumb-tip to index-tip gap of the simulated hand for @p curls at @p root.
float
tip_gap(const u_hand_tracking_curl_values &curls, xrt_hand hand, const xrt_space_relation &root)
{
	xrt_hand_joint_set set = {};
	u_hand_sim_simulate_for_valve_index_knuckles(&curls, hand, &root, &set);
	const auto &j = set.values.hand_joint_set_default;
	return dist(j[XRT_HAND_JOINT_THUMB_TIP].relation.pose.position,
	            j[XRT_HAND_JOINT_INDEX_TIP].relation.pose.position);
}

xrt_space_relation
root_at(bool left, int64_t t_ns)
{
	const xrt_pose c = sim_input_hand_center(left, SIM_INPUT_HAND_DISTANCE_DEFAULT_M, 0.0f);
	xrt_space_relation r = {};
	sim_input_hand_relation(&c, sim_input_hand_radius(0.0f), left ? M_PI : 0.0, t_ns, &r);
	return r;
}

} // namespace


TEST_CASE("HANDS / PINCH parse", "[sim_input][hands]")
{
	sim_input_hands_mode h = SIM_INPUT_HANDS_BOTH;
	CHECK(sim_input_parse_hands("right", &h));
	CHECK(h == SIM_INPUT_HANDS_RIGHT);
	CHECK(sim_input_parse_hands("LEFT", &h));
	CHECK(h == SIM_INPUT_HANDS_LEFT);
	CHECK(sim_input_parse_hands("none", &h));
	CHECK(h == SIM_INPUT_HANDS_NONE);
	CHECK(sim_input_parse_hands("Both", &h));
	CHECK(h == SIM_INPUT_HANDS_BOTH);
	CHECK_FALSE(sim_input_parse_hands("righty", &h));
	CHECK_FALSE(sim_input_parse_hands("", &h));
	CHECK(h == SIM_INPUT_HANDS_BOTH); // untouched on failure

	CHECK(sim_input_hand_enabled(SIM_INPUT_HANDS_BOTH, true));
	CHECK(sim_input_hand_enabled(SIM_INPUT_HANDS_BOTH, false));
	CHECK(sim_input_hand_enabled(SIM_INPUT_HANDS_LEFT, true));
	CHECK_FALSE(sim_input_hand_enabled(SIM_INPUT_HANDS_LEFT, false));
	CHECK_FALSE(sim_input_hand_enabled(SIM_INPUT_HANDS_RIGHT, true));
	CHECK(sim_input_hand_enabled(SIM_INPUT_HANDS_RIGHT, false));
	CHECK_FALSE(sim_input_hand_enabled(SIM_INPUT_HANDS_NONE, true));
	CHECK_FALSE(sim_input_hand_enabled(SIM_INPUT_HANDS_NONE, false));

	sim_input_pinch_mode p = SIM_INPUT_PINCH_WAVE;
	CHECK(sim_input_parse_pinch("hold", &p));
	CHECK(p == SIM_INPUT_PINCH_HOLD);
	CHECK(sim_input_parse_pinch("OPEN", &p));
	CHECK(p == SIM_INPUT_PINCH_OPEN);
	CHECK(sim_input_parse_pinch("wave", &p));
	CHECK(p == SIM_INPUT_PINCH_WAVE);
	CHECK_FALSE(sim_input_parse_pinch("1", &p));
	CHECK(p == SIM_INPUT_PINCH_WAVE);
}

TEST_CASE("hands are placed in front of the display plane (#2191)", "[sim_input][hands]")
{
	// Display-plane frame: origin at the display centre, +Z toward the viewer.
	// No display height (fallback 0.40 m) = the previous fixed placement.
	const xrt_pose l = sim_input_hand_center(true, SIM_INPUT_HAND_DISTANCE_DEFAULT_M, 0.0f);
	const xrt_pose r = sim_input_hand_center(false, SIM_INPUT_HAND_DISTANCE_DEFAULT_M, 0.0f);
	CHECK(l.position.x == Catch::Approx(-0.12f));
	CHECK(r.position.x == Catch::Approx(0.12f));
	CHECK(l.position.z == Catch::Approx(0.25f));
	CHECK(r.position.z == Catch::Approx(0.25f));
	CHECK(std::fabs(l.position.y) < 0.05f);
	CHECK(l.position.y == r.position.y);

	// HAND_DISTANCE_M moves the centre out of the glass; nonsense falls back.
	CHECK(sim_input_hand_center(false, 0.4f, 0.0f).position.z == Catch::Approx(0.4f));
	CHECK(sim_input_hand_center(false, 0.0f, 0.0f).position.z == Catch::Approx(SIM_INPUT_HAND_DISTANCE_DEFAULT_M));
	CHECK(sim_input_hand_center(false, -1.0f, 0.0f).position.z == Catch::Approx(SIM_INPUT_HAND_DISTANCE_DEFAULT_M));
	CHECK(sim_input_hand_center(false, NAN, 0.0f).position.z == Catch::Approx(SIM_INPUT_HAND_DISTANCE_DEFAULT_M));

	// The root rides a 4 cm circle in the display plane's XY about that
	// centre, a pure function of the timestamp, with the analytic velocity.
	for (int i = 0; i < 40; i++) {
		const int64_t t = 1000 * kNsPerS + (int64_t)i * (kNsPerS / 10);
		CAPTURE(i);
		xrt_space_relation a = root_at(false, t);
		xrt_space_relation b = root_at(false, t);
		CHECK(a.pose.position.x == b.pose.position.x); // deterministic
		CHECK(dist(a.pose.position, r.position) == Catch::Approx(SIM_INPUT_CIRCLE_RADIUS_M).margin(1e-4));
		CHECK(a.pose.position.z == Catch::Approx(r.position.z));
		const float speed =
		    std::sqrt(a.linear_velocity.x * a.linear_velocity.x + a.linear_velocity.y * a.linear_velocity.y);
		CHECK(speed ==
		      Catch::Approx(2.0 * M_PI * SIM_INPUT_CIRCLE_RADIUS_M / SIM_INPUT_CIRCLE_PERIOD_S).margin(1e-4));
		// Both hands stay on their own side of the display centre.
		CHECK(root_at(true, t).pose.position.x < -0.05f);
		CHECK(a.pose.position.x > 0.05f);
	}
}

TEST_CASE("PINCH=hold keeps thumb and index tips together; wave and open do not (#2191)", "[sim_input][hands]")
{
	const int64_t t0 = 1000 * kNsPerS;
	for (bool left : {true, false}) {
		const xrt_hand hand = left ? XRT_HAND_LEFT : XRT_HAND_RIGHT;
		const double phase = left ? M_PI : 0.0;
		float hold_max = 0.0f, open_min = 1.0f, wave_min = 1.0f, wave_max = 0.0f;
		// Sample a few seconds: the curl and the circle are both time-driven.
		for (int i = 0; i < 60; i++) {
			const int64_t t = t0 + (int64_t)i * (kNsPerS / 10);
			const xrt_space_relation root = root_at(left, t);
			u_hand_tracking_curl_values c = {};

			sim_input_hand_curls(SIM_INPUT_PINCH_HOLD, phase, t, &c);
			hold_max = std::fmax(hold_max, tip_gap(c, hand, root));
			CHECK(c.middle < 0.3f); // the other fingers stay relaxed
			CHECK(c.ring < 0.3f);
			CHECK(c.little < 0.3f);

			sim_input_hand_curls(SIM_INPUT_PINCH_OPEN, phase, t, &c);
			open_min = std::fmin(open_min, tip_gap(c, hand, root));

			sim_input_hand_curls(SIM_INPUT_PINCH_WAVE, phase, t, &c);
			const float g = tip_gap(c, hand, root);
			wave_min = std::fmin(wave_min, g);
			wave_max = std::fmax(wave_max, g);
		}
		CAPTURE(left, hold_max, open_min, wave_min, wave_max);
		std::printf("#2191 %s tip gap: hold max %.4f m, open min %.4f m, wave %.4f..%.4f m\n",
		            left ? "left " : "right", hold_max, open_min, wave_min, wave_max);
		// A sustained pinch: under 1.5 cm EVERY sample (a typical consumer's
		// pinch threshold is 2-3 cm).
		CHECK(hold_max < 0.015f);
		// Open: never a pinch.
		CHECK(open_min > 0.04f);
		// Wave: varies (it opens well past any pinch threshold).
		CHECK(wave_max > 0.04f);
	}
}

TEST_CASE("placement scales with the physical display height", "[sim_input][hands]")
{
	// The Linux field panel: 0.30 x 0.19 m, nominal viewer (0, 0.1, 0.6). At
	// z = 0.25 the viewer sees |x| < 0.15 * (0.6 - 0.25) / 0.6 = 0.0875 m; the
	// old fixed ±0.12 m centres were off-screen.
	const float h = 0.19f;
	const xrt_pose l = sim_input_hand_center(true, SIM_INPUT_HAND_DISTANCE_DEFAULT_M, h);
	const xrt_pose r = sim_input_hand_center(false, SIM_INPUT_HAND_DISTANCE_DEFAULT_M, h);
	CHECK(l.position.x == Catch::Approx(-0.30f * h));
	CHECK(r.position.x == Catch::Approx(0.30f * h));
	CHECK(l.position.y == Catch::Approx(-0.10f * h));
	// HAND_DISTANCE_M stays metres — it is not scaled.
	CHECK(r.position.z == Catch::Approx(SIM_INPUT_HAND_DISTANCE_DEFAULT_M));
	// The whole circle stays inside the visible half-width.
	const float radius = sim_input_hand_radius(h);
	CHECK(radius == Catch::Approx(0.10f * h));
	CHECK(r.position.x + radius < 0.0875f);

	// A large panel caps the radius at 4 cm; nonsense heights fall back.
	CHECK(sim_input_hand_radius(1.0f) == Catch::Approx(SIM_INPUT_CIRCLE_RADIUS_M));
	CHECK(sim_input_hand_radius(-1.0f) == Catch::Approx(SIM_INPUT_CIRCLE_RADIUS_M));
	CHECK(sim_input_hand_center(false, 0.25f, NAN).position.x == Catch::Approx(0.12f));
}
