// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The gamepad provider's mapping table: stick → rig velocity with
 *         deadzone, the integrator, the button actions.
 *
 * `gamepad_map.c` is pure (no OS, no device, no hub), so this compiles it
 * straight in and needs no pad. What it pins is the POLICY the devices
 * trust: a stick inside the deadzone moves nothing, the first live reading
 * is continuous (no jump at the edge), left stick = translate in the rig's
 * yaw frame, right stick = look with right = turn RIGHT, triggers = up /
 * down, pitch clamps and never flips the rig, and the button table says
 * what it says.
 */

#include "catch_amalgamated.hpp"

#include "gamepad_map.h"

#include <cmath>


namespace {

constexpr float kEps = 1e-5f;

struct gamepad_state
connected_state()
{
	struct gamepad_state s = {};
	s.connected = true;
	return s;
}

struct gamepad_map_config
default_config()
{
	struct gamepad_map_config c = GAMEPAD_MAP_CONFIG_DEFAULT;
	return c;
}

} // namespace


TEST_CASE("gamepad map: radial deadzone is zero inside, continuous at the edge, unit at full", "[gamepad_map]")
{
	float x = 1.0f, y = 1.0f;

	gamepad_map_apply_deadzone(0.10f, 0.05f, 0.15f, &x, &y);
	CHECK(x == 0.0f);
	CHECK(y == 0.0f);

	// Just past the edge: tiny, not a jump to 0.15.
	gamepad_map_apply_deadzone(0.1501f, 0.0f, 0.15f, &x, &y);
	CHECK(x > 0.0f);
	CHECK(x < 0.001f);
	CHECK(y == 0.0f);

	// Full deflection stays 1, direction preserved.
	gamepad_map_apply_deadzone(1.0f, 0.0f, 0.15f, &x, &y);
	CHECK(std::fabs(x - 1.0f) < kEps);
	gamepad_map_apply_deadzone(0.0f, -1.0f, 0.15f, &x, &y);
	CHECK(std::fabs(y + 1.0f) < kEps);

	// A square-gated diagonal is capped at the unit circle.
	gamepad_map_apply_deadzone(1.0f, 1.0f, 0.15f, &x, &y);
	CHECK(std::fabs(std::sqrt(x * x + y * y) - 1.0f) < 1e-4f);

	// NaN and out-of-range inputs are harmless.
	gamepad_map_apply_deadzone(NAN, 5.0f, 0.15f, &x, &y);
	CHECK(x == 0.0f);
	CHECK(std::fabs(y - 1.0f) < kEps);
}

TEST_CASE("gamepad map: a disconnected pad or a centred stick yields zero velocity", "[gamepad_map]")
{
	const struct gamepad_map_config cfg = default_config();
	struct gamepad_nav_velocity v;

	struct gamepad_state off = {};
	off.left_y = 1.0f; // ignored: not connected
	gamepad_map_nav_velocity(&off, &gamepad_map_default_table, &cfg, &v);
	CHECK(v.forward_mps == 0.0f);
	CHECK(v.yaw_dps == 0.0f);

	struct gamepad_state idle = connected_state();
	idle.left_x = 0.05f;
	idle.right_y = -0.1f; // inside the 0.15 deadzone
	gamepad_map_nav_velocity(&idle, &gamepad_map_default_table, &cfg, &v);
	CHECK(v.strafe_mps == 0.0f);
	CHECK(v.forward_mps == 0.0f);
	CHECK(v.up_mps == 0.0f);
	CHECK(v.yaw_dps == 0.0f);
	CHECK(v.pitch_dps == 0.0f);
}

TEST_CASE("gamepad map: left stick translates, right stick looks, triggers climb", "[gamepad_map]")
{
	const struct gamepad_map_config cfg = default_config();
	struct gamepad_nav_velocity v;

	struct gamepad_state s = connected_state();
	s.left_y = 1.0f; // stick up = forward
	gamepad_map_nav_velocity(&s, &gamepad_map_default_table, &cfg, &v);
	CHECK(std::fabs(v.forward_mps - cfg.nav_speed_mps) < kEps);
	CHECK(v.strafe_mps == 0.0f);

	s = connected_state();
	s.left_x = -1.0f; // stick left = strafe left
	gamepad_map_nav_velocity(&s, &gamepad_map_default_table, &cfg, &v);
	CHECK(std::fabs(v.strafe_mps + cfg.nav_speed_mps) < kEps);

	s = connected_state();
	s.right_x = 1.0f; // stick right = turn RIGHT = negative yaw about +Y
	gamepad_map_nav_velocity(&s, &gamepad_map_default_table, &cfg, &v);
	CHECK(std::fabs(v.yaw_dps + cfg.nav_turn_dps) < kEps);
	CHECK(v.pitch_dps == 0.0f);

	s = connected_state();
	s.right_y = 1.0f; // stick up = look up
	gamepad_map_nav_velocity(&s, &gamepad_map_default_table, &cfg, &v);
	CHECK(std::fabs(v.pitch_dps - cfg.nav_turn_dps) < kEps);

	struct gamepad_map_config inverted = cfg;
	inverted.invert_look_y = true;
	gamepad_map_nav_velocity(&s, &gamepad_map_default_table, &inverted, &v);
	CHECK(std::fabs(v.pitch_dps + cfg.nav_turn_dps) < kEps);

	s = connected_state();
	s.right_trigger = 1.0f;
	gamepad_map_nav_velocity(&s, &gamepad_map_default_table, &cfg, &v);
	CHECK(std::fabs(v.up_mps - cfg.nav_speed_mps) < kEps);
	s.left_trigger = 1.0f; // both: they cancel
	gamepad_map_nav_velocity(&s, &gamepad_map_default_table, &cfg, &v);
	CHECK(std::fabs(v.up_mps) < kEps);

	// Speed knobs scale linearly.
	struct gamepad_map_config fast = cfg;
	fast.nav_speed_mps = 3.0f;
	fast.nav_turn_dps = 10.0f;
	s = connected_state();
	s.left_y = 1.0f;
	s.right_x = -1.0f;
	gamepad_map_nav_velocity(&s, &gamepad_map_default_table, &fast, &v);
	CHECK(std::fabs(v.forward_mps - 3.0f) < kEps);
	CHECK(std::fabs(v.yaw_dps - 10.0f) < kEps);
}

TEST_CASE("gamepad map: the integrator moves forward along the current heading", "[gamepad_map]")
{
	struct gamepad_nav_pose p = GAMEPAD_NAV_POSE_IDENTITY;
	struct gamepad_nav_velocity v = {};

	// Forward at identity is −Z.
	v.forward_mps = 2.0f;
	gamepad_map_integrate(&p, &v, 0.5);
	CHECK(std::fabs(p.x) < kEps);
	CHECK(std::fabs(p.z + 1.0f) < kEps);

	// Turn left 90° (positive yaw), then forward: now −X.
	p = GAMEPAD_NAV_POSE_IDENTITY;
	v = {};
	v.yaw_dps = 90.0f;
	gamepad_map_integrate(&p, &v, 1.0);
	CHECK(std::fabs(p.yaw_rad - (float)(M_PI / 2.0)) < 1e-4f);
	v = {};
	v.forward_mps = 1.0f;
	gamepad_map_integrate(&p, &v, 1.0);
	CHECK(std::fabs(p.x + 1.0f) < 1e-4f);
	CHECK(std::fabs(p.z) < 1e-4f);

	// Strafe right from that heading: +X of the rig is now −Z of F.
	v = {};
	v.strafe_mps = 1.0f;
	gamepad_map_integrate(&p, &v, 1.0);
	CHECK(std::fabs(p.z + 1.0f) < 1e-4f);

	// Up is world up whatever the pitch.
	p = GAMEPAD_NAV_POSE_IDENTITY;
	p.pitch_rad = 0.5f;
	v = {};
	v.up_mps = 1.0f;
	gamepad_map_integrate(&p, &v, 0.25);
	CHECK(std::fabs(p.y - 0.25f) < kEps);
	CHECK(std::fabs(p.z) < kEps);

	// A zero or negative dt is a no-op.
	struct gamepad_nav_pose before = p;
	v.forward_mps = 5.0f;
	gamepad_map_integrate(&p, &v, 0.0);
	gamepad_map_integrate(&p, &v, -1.0);
	CHECK(p.z == before.z);
}

TEST_CASE("gamepad map: pitch clamps and yaw wraps", "[gamepad_map]")
{
	struct gamepad_nav_pose p = GAMEPAD_NAV_POSE_IDENTITY;
	struct gamepad_nav_velocity v = {};

	v.pitch_dps = 720.0f;
	gamepad_map_integrate(&p, &v, 1.0);
	CHECK(std::fabs(p.pitch_rad - (float)(GAMEPAD_MAP_PITCH_LIMIT_DEG * M_PI / 180.0)) < 1e-4f);
	v.pitch_dps = -720.0f;
	gamepad_map_integrate(&p, &v, 1.0);
	CHECK(std::fabs(p.pitch_rad + (float)(GAMEPAD_MAP_PITCH_LIMIT_DEG * M_PI / 180.0)) < 1e-4f);

	p = GAMEPAD_NAV_POSE_IDENTITY;
	v = {};
	v.yaw_dps = 90.0f;
	gamepad_map_integrate(&p, &v, 8.0); // two full turns
	CHECK(std::fabs(p.yaw_rad) < 1e-3f);
}

TEST_CASE("gamepad map: the xrt pose is yaw about +Y then pitch about +X", "[gamepad_map]")
{
	struct gamepad_nav_pose p = GAMEPAD_NAV_POSE_IDENTITY;
	struct xrt_pose out;

	gamepad_map_nav_pose_to_xrt(&p, &out);
	CHECK(std::fabs(out.orientation.w - 1.0f) < kEps);
	CHECK(out.position.x == 0.0f);

	p.yaw_rad = (float)(M_PI / 2.0);
	p.x = 1.0f;
	p.y = 2.0f;
	p.z = 3.0f;
	gamepad_map_nav_pose_to_xrt(&p, &out);
	CHECK(std::fabs(out.orientation.y - std::sqrt(0.5f)) < 1e-4f);
	CHECK(std::fabs(out.orientation.w - std::sqrt(0.5f)) < 1e-4f);
	CHECK(std::fabs(out.orientation.x) < kEps);
	CHECK(out.position.x == 1.0f);
	CHECK(out.position.y == 2.0f);
	CHECK(out.position.z == 3.0f);

	// Unit quaternion for any combination.
	p.pitch_rad = 0.7f;
	gamepad_map_nav_pose_to_xrt(&p, &out);
	const float n = out.orientation.x * out.orientation.x + out.orientation.y * out.orientation.y +
	                out.orientation.z * out.orientation.z + out.orientation.w * out.orientation.w;
	CHECK(std::fabs(n - 1.0f) < 1e-5f);
}

TEST_CASE("gamepad map: activity is any button, a stick past the deadzone, or a trigger pull", "[gamepad_map]")
{
	const float dz = 0.15f;
	struct gamepad_state s = connected_state();

	CHECK_FALSE(gamepad_map_has_activity(&s, dz)); // at rest

	s.left_x = 0.10f; // inside the deadzone: a resting stick, not a touch
	s.right_y = -0.12f;
	CHECK_FALSE(gamepad_map_has_activity(&s, dz));

	s.left_x = 0.3f;
	CHECK(gamepad_map_has_activity(&s, dz));

	s = connected_state();
	s.right_y = -0.5f;
	CHECK(gamepad_map_has_activity(&s, dz));

	s = connected_state();
	s.buttons = GAMEPAD_BTN_DPAD_LEFT;
	CHECK(gamepad_map_has_activity(&s, dz));

	s = connected_state();
	s.right_trigger = 0.02f; // trigger noise
	CHECK_FALSE(gamepad_map_has_activity(&s, dz));
	s.right_trigger = 0.2f;
	CHECK(gamepad_map_has_activity(&s, dz));

	s.connected = false; // nothing counts on a disconnected pad
	s.buttons = GAMEPAD_BTN_A;
	CHECK_FALSE(gamepad_map_has_activity(&s, dz));
}

TEST_CASE("gamepad map: last-used input wins — the idle rule behind presence", "[gamepad_map]")
{
	constexpr int64_t kSecond = 1000LL * 1000 * 1000;
	const int64_t now = 100 * kSecond;

	// Timeout 0 = connected-only: always active, even never touched.
	CHECK(gamepad_map_recently_active(now, 0, 0.0f));
	CHECK(gamepad_map_recently_active(now, 0, -1.0f));

	// With a timeout, a pad never touched since plug-in is NOT active.
	CHECK_FALSE(gamepad_map_recently_active(now, 0, 20.0f));

	// Touched inside the window: active; at the edge: still active; past it: idle.
	CHECK(gamepad_map_recently_active(now, now - 1 * kSecond, 20.0f));
	CHECK(gamepad_map_recently_active(now, now - 20 * kSecond, 20.0f));
	CHECK_FALSE(gamepad_map_recently_active(now, now - 21 * kSecond, 20.0f));

	// Touching again re-arms it: the verdict is a pure function of the
	// latest activity timestamp, nothing latches.
	CHECK(gamepad_map_recently_active(now + 30 * kSecond, now + 25 * kSecond, 20.0f));
}

TEST_CASE("gamepad map: button actions follow the table", "[gamepad_map]")
{
	const struct gamepad_map_table *t = &gamepad_map_default_table;
	struct gamepad_state s = connected_state();

	CHECK_FALSE(gamepad_map_action_pressed(&s, t, GAMEPAD_ACT_LEFT_SELECT));
	CHECK_FALSE(gamepad_map_action_pressed(&s, t, GAMEPAD_ACT_RECENTER));

	s.buttons = GAMEPAD_BTN_SHOULDER_LEFT;
	CHECK(gamepad_map_action_pressed(&s, t, GAMEPAD_ACT_LEFT_SELECT));
	CHECK_FALSE(gamepad_map_action_pressed(&s, t, GAMEPAD_ACT_RIGHT_SELECT));

	s.buttons = GAMEPAD_BTN_A;
	CHECK(gamepad_map_action_pressed(&s, t, GAMEPAD_ACT_RIGHT_SELECT));

	s.buttons = GAMEPAD_BTN_MENU;
	CHECK(gamepad_map_action_pressed(&s, t, GAMEPAD_ACT_RIGHT_MENU));
	CHECK_FALSE(gamepad_map_action_pressed(&s, t, GAMEPAD_ACT_LEFT_MENU));

	s.buttons = GAMEPAD_BTN_VIEW;
	CHECK(gamepad_map_action_pressed(&s, t, GAMEPAD_ACT_LEFT_MENU));

	s.buttons = GAMEPAD_BTN_THUMB_RIGHT;
	CHECK(gamepad_map_action_pressed(&s, t, GAMEPAD_ACT_RECENTER));

	// Not connected: nothing is pressed, whatever the bits say.
	s.connected = false;
	CHECK_FALSE(gamepad_map_action_pressed(&s, t, GAMEPAD_ACT_RECENTER));

	// Every action in the default table is bound to something.
	for (int a = 0; a < GAMEPAD_ACT_COUNT; a++) {
		bool bound = false;
		for (uint32_t i = 0; i < t->button_count; i++) {
			bound |= t->buttons[i].action == (enum gamepad_map_action)a;
		}
		CHECK(bound);
	}
}
