// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The sim_stylus script (ADR-034 Amendment 8): the tip on the glass in
 *         draw mode, above it in hover mode, the pressure ramp, the pen axes,
 *         the button schedule and the dock / presence schedule — pure
 *         functions, no device, no plug-in, no display.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_defines.h"

#include "sim_stylus_script.h"

#include <cmath>
#include <cstdint>


namespace {

constexpr int64_t kNsPerS = 1000000000LL;
constexpr int64_t kNsPerMs = 1000000LL;

sim_stylus_script_config
base_config()
{
	sim_stylus_script_config c = {};
	c.mode = SIM_STYLUS_MODE_DRAW;
	c.display_width_m = 0.60f;
	c.display_height_m = 0.34f;
	c.hover_height_m = SIM_STYLUS_HOVER_HEIGHT_DEFAULT_M;
	c.period_s = 6.0f;
	c.left_handed = false;
	c.buttons = true;
	c.dock_ms = 0;
	c.dock_sleep_ms = SIM_STYLUS_DOCK_SLEEP_DEFAULT_MS;
	return c;
}

//! Rotate @p v by unit quaternion @p q.
xrt_vec3
rotate(const xrt_quat &q, const xrt_vec3 &v)
{
	// v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z)
	const xrt_vec3 u = {q.x, q.y, q.z};
	const xrt_vec3 c1 = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
	const xrt_vec3 c2 = {u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x};
	return {v.x + 2.0f * (q.w * c1.x + c2.x), v.y + 2.0f * (q.w * c1.y + c2.y), v.z + 2.0f * (q.w * c1.z + c2.z)};
}

} // namespace


TEST_CASE("draw: the tip is ON the glass with pressure for most of each stroke", "[sim_stylus]")
{
	const sim_stylus_script_config c = base_config();
	int contact = 0;
	int lifted = 0;
	const int steps = 600;
	for (int i = 0; i < steps; i++) {
		const int64_t t = (int64_t)i * 10 * kNsPerMs; // 6 s at 10 ms
		sim_stylus_state s;
		sim_stylus_script_eval(&c, t, &s);
		REQUIRE(s.tracked);
		REQUIRE_FALSE(s.docked);
		if (s.contact) {
			contact++;
			CHECK(s.tip.position.z == 0.0f);
			CHECK(s.tip_force >= SIM_STYLUS_FORCE_MIN - 1e-4f);
			CHECK(s.tip_force <= 1.0f + 1e-4f);
		} else {
			lifted++;
			CHECK(s.tip_force == 0.0f);
			CHECK(s.tip.position.z >= 0.0f);
			CHECK(s.tip.position.z <= SIM_STYLUS_LIFT_HEIGHT_M + 1e-4f);
		}
		// The figure stays inside the panel.
		CHECK(std::fabs(s.tip.position.x) <= 0.5f * c.display_width_m);
		CHECK(std::fabs(s.tip.position.y) <= 0.5f * c.display_height_m);
	}
	CHECK(contact == Catch::Approx(steps * SIM_STYLUS_DRAW_CONTACT_FRAC).margin(2));
	CHECK(lifted > 0);
}

TEST_CASE("draw: the pressure ramp rises to 1 mid-stroke", "[sim_stylus]")
{
	const sim_stylus_script_config c = base_config();
	sim_stylus_state start, mid;
	sim_stylus_script_eval(&c, 0, &start);
	const double mid_s = 0.5 * SIM_STYLUS_DRAW_CONTACT_FRAC * c.period_s;
	sim_stylus_script_eval(&c, (int64_t)(mid_s * kNsPerS), &mid);
	CHECK(start.tip_force == Catch::Approx(SIM_STYLUS_FORCE_MIN).margin(1e-3));
	CHECK(mid.tip_force == Catch::Approx(1.0f).margin(1e-3));
}

TEST_CASE("hover: the tip stays HOVER_HEIGHT_M above the glass, no pressure", "[sim_stylus]")
{
	sim_stylus_script_config c = base_config();
	c.mode = SIM_STYLUS_MODE_HOVER;
	c.hover_height_m = 0.03f;
	for (int i = 0; i < 100; i++) {
		sim_stylus_state s;
		sim_stylus_script_eval(&c, (int64_t)i * 73 * kNsPerMs, &s);
		CHECK(s.tip.position.z == Catch::Approx(0.03f));
		CHECK(s.tip_force == 0.0f);
		CHECK_FALSE(s.contact);
	}
}

TEST_CASE("pen axes: -Z points into the glass, the grip sits back up the barrel", "[sim_stylus]")
{
	for (bool left : {false, true}) {
		sim_stylus_script_config c = base_config();
		c.left_handed = left;
		sim_stylus_state s;
		sim_stylus_script_eval(&c, 1 * kNsPerS, &s);

		const xrt_quat &q = s.tip.orientation;
		CHECK(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w == Catch::Approx(1.0f).margin(1e-4));

		const xrt_vec3 fwd = rotate(q, {0.0f, 0.0f, -1.0f}); // out of the tip
		CHECK(fwd.z < -0.5f);                                // into the glass
		// The barrel leans toward the holding hand: the tip points away from it.
		if (left) {
			CHECK(fwd.x > 0.0f);
		} else {
			CHECK(fwd.x < 0.0f);
		}
		const xrt_vec3 up = rotate(q, {0.0f, 1.0f, 0.0f});
		CHECK(up.y > 0.0f);

		// grip = tip - fwd * offset
		CHECK(s.grip.position.x ==
		      Catch::Approx(s.tip.position.x - fwd.x * SIM_STYLUS_GRIP_OFFSET_M).margin(1e-4));
		CHECK(s.grip.position.y ==
		      Catch::Approx(s.tip.position.y - fwd.y * SIM_STYLUS_GRIP_OFFSET_M).margin(1e-4));
		CHECK(s.grip.position.z ==
		      Catch::Approx(s.tip.position.z - fwd.z * SIM_STYLUS_GRIP_OFFSET_M).margin(1e-4));
	}
}

TEST_CASE("velocity matches the motion of the tip", "[sim_stylus]")
{
	const sim_stylus_script_config c = base_config();
	const int64_t t = 2 * kNsPerS;
	sim_stylus_state a, b;
	sim_stylus_script_eval(&c, t, &a);
	sim_stylus_script_eval(&c, t + 10 * kNsPerMs, &b);
	CHECK(a.tip_linear_velocity.x == Catch::Approx((b.tip.position.x - a.tip.position.x) / 0.01f).epsilon(0.05));
	CHECK(a.tip_linear_velocity.y == Catch::Approx((b.tip.position.y - a.tip.position.y) / 0.01f).epsilon(0.05));
}

TEST_CASE("buttons: each pulse fires on its schedule, and BUTTONS=0 silences them", "[sim_stylus]")
{
	sim_stylus_script_config c = base_config();
	sim_stylus_state s;
	sim_stylus_script_eval(&c, (int64_t)(1.05 * kNsPerS), &s);
	CHECK(s.front_click);
	CHECK_FALSE(s.back_click);
	sim_stylus_script_eval(&c, (int64_t)(3.05 * kNsPerS), &s);
	CHECK(s.back_click);
	sim_stylus_script_eval(&c, (int64_t)(5.05 * kNsPerS), &s);
	CHECK(s.front_double_tap);
	sim_stylus_script_eval(&c, (int64_t)(9.05 * kNsPerS), &s);
	CHECK(s.back_double_tap);
	sim_stylus_script_eval(&c, (int64_t)(6.5 * kNsPerS), &s);
	CHECK(s.middle_force == Catch::Approx(1.0f).margin(1e-3));
	sim_stylus_script_eval(&c, (int64_t)(0.5 * kNsPerS), &s);
	CHECK_FALSE(s.front_click);
	CHECK(s.middle_force == 0.0f);
	CHECK_FALSE(s.system_click);

	c.buttons = false;
	sim_stylus_script_eval(&c, (int64_t)(1.05 * kNsPerS), &s);
	CHECK_FALSE(s.front_click);
	sim_stylus_script_eval(&c, (int64_t)(6.5 * kNsPerS), &s);
	CHECK(s.middle_force == 0.0f);
}

TEST_CASE("dock: docked half the time, asleep (untracked) after DOCK_SLEEP_MS", "[sim_stylus]")
{
	sim_stylus_script_config c = base_config();
	c.dock_ms = 4000;
	c.dock_sleep_ms = 1000;

	sim_stylus_state s;
	// [0, 4 s): free.
	sim_stylus_script_eval(&c, 2 * kNsPerS, &s);
	CHECK_FALSE(s.docked);
	CHECK(s.tracked);
	CHECK(sim_stylus_script_tracked(&c, 2 * kNsPerS));
	// [4, 5 s): docked, still tracked: an app sees docked_logitech before the pen sleeps.
	sim_stylus_script_eval(&c, (int64_t)(4.5 * kNsPerS), &s);
	CHECK(s.docked);
	CHECK(s.tracked);
	CHECK(s.tip_force == 0.0f);
	CHECK_FALSE(s.front_click);
	CHECK(s.tip.position.x > 0.5f * c.display_width_m); // beside the panel
	CHECK(sim_stylus_script_tracked(&c, (int64_t)(4.5 * kNsPerS)));
	// [5, 8 s): asleep in the dock.
	sim_stylus_script_eval(&c, 6 * kNsPerS, &s);
	CHECK(s.docked);
	CHECK_FALSE(s.tracked);
	CHECK_FALSE(sim_stylus_script_tracked(&c, 6 * kNsPerS));
	// [8, 12 s): free again.
	CHECK(sim_stylus_script_tracked(&c, 9 * kNsPerS));

	// DOCK_SLEEP_MS = 0: stays tracked while docked.
	c.dock_sleep_ms = 0;
	CHECK(sim_stylus_script_tracked(&c, 7 * kNsPerS));
	// DOCK_MS = 0: never docks.
	c.dock_ms = 0;
	sim_stylus_script_eval(&c, 6 * kNsPerS, &s);
	CHECK_FALSE(s.docked);
}

TEST_CASE("MODE parsing", "[sim_stylus]")
{
	sim_stylus_mode m = SIM_STYLUS_MODE_DRAW;
	CHECK(sim_stylus_parse_mode("hover", &m));
	CHECK(m == SIM_STYLUS_MODE_HOVER);
	CHECK(sim_stylus_parse_mode("draw", &m));
	CHECK(m == SIM_STYLUS_MODE_DRAW);
	CHECK_FALSE(sim_stylus_parse_mode("scribble", &m));
	CHECK(m == SIM_STYLUS_MODE_DRAW);
	CHECK_FALSE(sim_stylus_parse_mode(nullptr, &m));
}
