// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's synthetic viewer at a target time (ADR-054 §4.1 / N6
 *         Part 1, #2144): what every sim_display DP's get_eye_positions_at
 *         slot answers before the webcam override.
 *
 * Pins that the fake-tracking sway is deterministic in the TARGET time (a
 * query at t and at t + 16 ms differ predictably, and the same t answers the
 * same pair), that sway 0 is the static nominal layout sim_display always
 * reported (1 / 2 / 4 views), and that the square-wave is_tracking is
 * evaluated at the target rather than now.
 */

#include "catch_amalgamated.hpp"

#include "sim_display_eyes.h"

#include <cmath>

namespace {

constexpr int64_t kMs = 1000ll * 1000ll;

sim_fake_eyes_params
params(float sway_mm = 0.0f, int64_t period_ms = 4000)
{
	sim_fake_eyes_params p = {};
	p.nominal_x_m = 0.0f;
	p.nominal_y_m = 0.1f;
	p.nominal_z_m = 0.6f;
	p.ipd_m = 0.064f;
	p.sway_m = sway_mm / 1000.0f;
	p.sway_period_ns = period_ms * kMs;
	return p;
}

} // namespace

TEST_CASE("sim eyes: sway 0 is the static nominal layout at any target")
{
	const sim_fake_eyes_params p = params();
	xrt_eye_positions e = {};

	sim_fake_eyes_at(&p, 2, 123 * kMs, &e);
	CHECK(e.count == 2);
	CHECK(e.valid);
	CHECK(e.eyes[0].x == Catch::Approx(-0.032f));
	CHECK(e.eyes[1].x == Catch::Approx(0.032f));
	CHECK(e.eyes[0].y == Catch::Approx(0.1f));
	CHECK(e.eyes[0].z == Catch::Approx(0.6f));
	CHECK(e.timestamp_ns == 123 * kMs);

	xrt_eye_positions later = {};
	sim_fake_eyes_at(&p, 2, 9999 * kMs, &later);
	CHECK(later.eyes[0].x == Catch::Approx(e.eyes[0].x));
	CHECK(later.eyes[1].y == Catch::Approx(e.eyes[1].y));

	sim_fake_eyes_at(&p, 1, 0, &e);
	CHECK(e.count == 1);
	CHECK(e.eyes[0].x == Catch::Approx(0.0f));

	sim_fake_eyes_at(&p, 4, 0, &e);
	CHECK(e.count == 4);
	CHECK(e.eyes[0].y == Catch::Approx(0.1f - 0.032f));
	CHECK(e.eyes[2].y == Catch::Approx(0.1f + 0.032f));
	CHECK(e.eyes[3].x == Catch::Approx(0.032f));
}

TEST_CASE("sim eyes: the sway is deterministic in the target time")
{
	// 20 mm sway, 4 s period: a quarter period in is the +20 mm extreme.
	const sim_fake_eyes_params p = params(20.0f, 4000);
	xrt_eye_positions a = {}, b = {}, a2 = {};

	sim_fake_eyes_at(&p, 2, 1000 * kMs, &a); // phase pi/2: x = +sway, y = nominal
	CHECK(a.eyes[0].x == Catch::Approx(0.020f - 0.032f).margin(1e-5f));
	CHECK(a.eyes[1].x == Catch::Approx(0.020f + 0.032f).margin(1e-5f));
	CHECK(a.eyes[0].y == Catch::Approx(0.1f).margin(1e-5f));

	sim_fake_eyes_at(&p, 2, 1016 * kMs, &b); // 16 ms later: moved back toward centre
	const float dx = b.eyes[0].x - a.eyes[0].x;
	CHECK(dx < 0.0f);
	CHECK(std::fabs(dx) < 0.001f); // a frame's worth of a 4 s sway, not a jump
	CHECK(dx == Catch::Approx(0.020f * (std::sin(6.28318530717958647692 * 1016.0 / 4000.0) - 1.0)).margin(1e-6f));
	// The pair keeps its IPD throughout.
	CHECK(b.eyes[1].x - b.eyes[0].x == Catch::Approx(0.064f));

	// Same target, same answer: nothing depends on the wall clock.
	sim_fake_eyes_at(&p, 2, 1000 * kMs, &a2);
	CHECK(a2.eyes[0].x == a.eyes[0].x);
	CHECK(a2.eyes[0].y == a.eyes[0].y);

	// Periodic: one full period later is the same pair.
	xrt_eye_positions c = {};
	sim_fake_eyes_at(&p, 2, 5000 * kMs, &c);
	CHECK(c.eyes[0].x == Catch::Approx(a.eyes[0].x).margin(1e-6f));

	// Depth never sways.
	CHECK(b.eyes[0].z == Catch::Approx(0.6f));
}

TEST_CASE("sim eyes: the square-wave is_tracking is evaluated at the target")
{
	uint64_t edges = 99;
	CHECK(sim_fake_tracking_at(0, 123456789, &edges)); // no period: always tracking
	CHECK(edges == 0);

	// 500 ms period: tracking in [0,500), lost in [500,1000), ...
	CHECK(sim_fake_tracking_at(500, 0, &edges));
	CHECK(edges == 0);
	CHECK(sim_fake_tracking_at(500, 499 * kMs, &edges));
	CHECK(edges == 0);
	CHECK_FALSE(sim_fake_tracking_at(500, 500 * kMs, &edges));
	CHECK(edges == 1);
	CHECK_FALSE(sim_fake_tracking_at(500, 999 * kMs, nullptr));
	CHECK(sim_fake_tracking_at(500, 1000 * kMs, &edges));
	CHECK(edges == 2);
	// A target 16 ms ahead of a loss edge already reports the loss: the
	// query answers for when the pixels are seen, not for now.
	CHECK(sim_fake_tracking_at(500, 1484 * kMs, nullptr));
	CHECK_FALSE(sim_fake_tracking_at(500, 1500 * kMs, nullptr));
}
