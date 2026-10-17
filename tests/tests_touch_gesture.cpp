// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The touch provider's gesture recogniser + rig math (#132) and the
 *         pointer-event ring it is fed from.
 *
 * Pure functions of the events fed in and the timestamps handed to tick(), so
 * this pins the MAPPING TABLE in touch_gesture.h with no device, no loader and
 * no display: which gesture becomes which rig motion, with which sign, and
 * what SELECT / RECENTER look like on the wire.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_defines.h"
#include "xrt/xrt_input_plugin.h"

#include "util/u_pointer_sink.h"

#include "touch_gesture.h"

#include <cmath>


namespace {

constexpr uint32_t kW = 2560;
constexpr uint32_t kH = 1600;
constexpr int64_t kMs = 1000LL * 1000;

struct xrt_input_pointer_event
ev(uint32_t action, uint32_t id, float x, float y, int64_t t_ns, uint32_t count)
{
	struct xrt_input_pointer_event e = {};
	e.struct_size = sizeof(e);
	e.action = action;
	e.pointer_id = id;
	e.x = x;
	e.y = y;
	e.timestamp_ns = t_ns;
	e.pointer_count = count;
	e.surface_width = kW;
	e.surface_height = kH;
	return e;
}

void
down(struct touch_gesture &g, uint32_t id, float x, float y, int64_t t, uint32_t count = 1)
{
	auto e = ev(XRT_INPUT_POINTER_ACTION_DOWN, id, x, y, t, count);
	touch_gesture_feed(&g, &e);
}
void
move(struct touch_gesture &g, uint32_t id, float x, float y, int64_t t, uint32_t count = 1)
{
	auto e = ev(XRT_INPUT_POINTER_ACTION_MOVE, id, x, y, t, count);
	touch_gesture_feed(&g, &e);
}
void
up(struct touch_gesture &g, uint32_t id, float x, float y, int64_t t, uint32_t count = 0)
{
	auto e = ev(XRT_INPUT_POINTER_ACTION_UP, id, x, y, t, count);
	touch_gesture_feed(&g, &e);
}

struct touch_gesture
fresh()
{
	struct touch_gesture g;
	struct touch_gesture_config cfg;
	touch_gesture_config_defaults(&cfg);
	cfg.display_width_m = 0.32f;
	cfg.display_height_m = 0.20f;
	cfg.viewer_z_m = 0.5f;
	touch_gesture_init(&g, &cfg);
	return g;
}

bool
near(float a, float b, float eps = 1e-4f)
{
	return std::fabs(a - b) <= eps;
}

} // namespace


TEST_CASE("touch_gesture: at rest the rig is at F's origin, looking down -Z", "[touch]")
{
	auto g = fresh();
	struct xrt_pose p;
	touch_gesture_nav_pose(&g, &p);
	CHECK(near(p.position.x, 0.0f));
	CHECK(near(p.position.y, 0.0f));
	CHECK(near(p.position.z, 0.0f));
	CHECK(near(p.orientation.w, 1.0f));
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_IDLE);
	CHECK_FALSE(touch_gesture_select(&g, 0));
	CHECK(touch_gesture_recenter_timestamp(&g) == 0);
}

TEST_CASE("touch_gesture: one-finger drag orbits about the pivot on the display plane", "[touch]")
{
	auto g = fresh();
	// Drag right by a quarter of the width: yaw = -pi/4 (camera orbits to -X).
	down(g, 0, 1000.0f, 800.0f, 0);
	move(g, 0, 1000.0f + kW / 4.0f, 800.0f, 20 * kMs);
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_ORBIT);

	struct xrt_pose p;
	touch_gesture_nav_pose(&g, &p);
	const float d0 = 0.5f;
	// pivot at (0,0,-d0); camera = pivot + Ry(-pi/4) * (0,0,d0) = (-d0 sin45, 0, -d0 + d0 cos45)
	CHECK(near(p.position.x, -d0 * std::sin(M_PI / 4), 1e-3f));
	CHECK(near(p.position.y, 0.0f, 1e-3f));
	CHECK(near(p.position.z, -d0 + d0 * std::cos(M_PI / 4), 1e-3f));
	// Still the same distance from the pivot.
	const float dx = p.position.x, dy = p.position.y, dz = p.position.z + d0;
	CHECK(near(std::sqrt(dx * dx + dy * dy + dz * dz), d0, 1e-3f));
	// Orientation is a pure yaw of -pi/4.
	CHECK(near(p.orientation.y, std::sin(-M_PI / 8), 1e-3f));
	CHECK(near(p.orientation.w, std::cos(-M_PI / 8), 1e-3f));

	// An orbit drag never clicks.
	up(g, 0, 1000.0f + kW / 4.0f, 800.0f, 40 * kMs);
	touch_gesture_tick(&g, 41 * kMs);
	CHECK_FALSE(touch_gesture_select(&g, 41 * kMs));
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_IDLE);
}

TEST_CASE("touch_gesture: drag down raises the camera (pitch is clamped)", "[touch]")
{
	auto g = fresh();
	down(g, 0, 100.0f, 100.0f, 0);
	move(g, 0, 100.0f, 100.0f + kH / 2.0f, 20 * kMs); // half the height: pitch = -pi/4
	struct xrt_pose p;
	touch_gesture_nav_pose(&g, &p);
	CHECK(p.position.y > 0.3f); // camera went UP
	CHECK(near(p.position.x, 0.0f, 1e-3f));

	// Keep dragging: pitch clamps at 85 degrees, never flips over the pole.
	move(g, 0, 100.0f, 100.0f + 10.0f * kH, 40 * kMs);
	touch_gesture_nav_pose(&g, &p);
	CHECK(p.position.y <= 0.5f + 1e-3f);
	CHECK(p.position.y > 0.49f);
}

TEST_CASE("touch_gesture: pinch dollies, two-finger drag pans, clamped", "[touch]")
{
	auto g = fresh();
	down(g, 0, 1000.0f, 800.0f, 0, 1);
	down(g, 1, 1400.0f, 800.0f, 1 * kMs, 2);
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_TWO_FINGER);

	// Pinch OUT to twice the distance: d halves (content comes closer).
	move(g, 0, 800.0f, 800.0f, 10 * kMs, 2);
	move(g, 1, 1600.0f, 800.0f, 11 * kMs, 2);
	struct xrt_pose p;
	touch_gesture_nav_pose(&g, &p);
	// Camera sits at pivot + (0,0,d) = (0,0,-0.5+0.25) = (0,0,-0.25)
	CHECK(near(p.position.z, -0.25f, 1e-3f));
	CHECK(near(p.orientation.w, 1.0f));

	// Drag both fingers right by a tenth of the width: content follows, the
	// camera moves -X by a tenth of the display width (0.032 m).
	move(g, 0, 800.0f + kW / 10.0f, 800.0f, 20 * kMs, 2);
	move(g, 1, 1600.0f + kW / 10.0f, 800.0f, 21 * kMs, 2);
	touch_gesture_nav_pose(&g, &p);
	CHECK(near(p.position.x, -0.032f, 1e-3f));
	CHECK(near(p.position.z, -0.25f, 1e-3f)); // dolly unchanged by a pure pan

	// Pinch in absurdly far: clamped at 10x d0.
	move(g, 0, 1199.0f, 800.0f, 30 * kMs, 2);
	move(g, 1, 1201.0f, 800.0f, 31 * kMs, 2);
	touch_gesture_nav_pose(&g, &p);
	CHECK(p.position.z <= -0.5f + 5.0f + 1e-3f);
	CHECK(p.position.z >= -0.5f + 5.0f - 1e-3f);

	// Lifting one finger continues as an orbit from there, never a tap.
	up(g, 1, 1201.0f, 800.0f, 40 * kMs, 1);
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_ORBIT);
	up(g, 0, 1199.0f, 800.0f, 50 * kMs, 0);
	touch_gesture_tick(&g, 51 * kMs);
	CHECK_FALSE(touch_gesture_select(&g, 51 * kMs));
}

TEST_CASE("touch_gesture: a tap pulses SELECT and the aim ray goes through the finger", "[touch]")
{
	auto g = fresh();
	// Tap the exact centre of the surface.
	down(g, 0, kW / 2.0f, kH / 2.0f, 0);
	touch_gesture_tick(&g, 50 * kMs);
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_PENDING);
	CHECK_FALSE(touch_gesture_select(&g, 50 * kMs)); // undecided: not yet a click
	up(g, 0, kW / 2.0f + 3.0f, kH / 2.0f - 2.0f, 100 * kMs);
	touch_gesture_tick(&g, 101 * kMs);
	CHECK(touch_gesture_select(&g, 101 * kMs));
	CHECK(touch_gesture_select(&g, 100 * kMs + TOUCH_GESTURE_SELECT_PULSE_NS - 1));
	touch_gesture_tick(&g, 100 * kMs + TOUCH_GESTURE_SELECT_PULSE_NS);
	CHECK_FALSE(touch_gesture_select(&g, 100 * kMs + TOUCH_GESTURE_SELECT_PULSE_NS));

	struct xrt_pose aim, grip;
	REQUIRE(touch_gesture_aim(&g, &aim, &grip));
	// Aim starts at the nominal viewer (0,0,0.5) and points at the plane
	// centre, i.e. straight down -Z: identity orientation.
	CHECK(near(aim.position.z, 0.5f));
	CHECK(near(aim.orientation.w, 1.0f, 1e-3f));
	// Grip sits on the plane, 3 px right / 2 px up of centre.
	CHECK(near(grip.position.x, 3.0f / kW * 0.32f, 1e-5f));
	CHECK(near(grip.position.y, 2.0f / kH * 0.20f, 1e-5f));
	CHECK(near(grip.position.z, 0.0f));

	// A tap in the top-right quadrant tilts the ray up and right.
	down(g, 0, kW * 0.9f, kH * 0.1f, 200 * kMs);
	up(g, 0, kW * 0.9f, kH * 0.1f, 250 * kMs);
	REQUIRE(touch_gesture_aim(&g, &aim, &grip));
	CHECK(grip.position.x > 0.1f);
	CHECK(grip.position.y > 0.07f);
	// The ray's -Z axis rotated by aim.orientation must point from the viewer to the grip point.
	const float qx = aim.orientation.x, qy = aim.orientation.y, qz = aim.orientation.z, qw = aim.orientation.w;
	// forward = q * (0,0,-1) * q^-1, via the rotation-matrix third column negated.
	const float fx = -(2.0f * (qx * qz + qw * qy));
	const float fy = -(2.0f * (qy * qz - qw * qx));
	const float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
	float tx = grip.position.x - aim.position.x, ty = grip.position.y - aim.position.y,
	      tz = grip.position.z - aim.position.z;
	const float tn = std::sqrt(tx * tx + ty * ty + tz * tz);
	tx /= tn;
	ty /= tn;
	tz /= tn;
	CHECK(near(fx, tx, 1e-3f));
	CHECK(near(fy, ty, 1e-3f));
	CHECK(near(fz, tz, 1e-3f));
}

TEST_CASE("touch_gesture: press-and-hold holds SELECT and does not orbit", "[touch]")
{
	auto g = fresh();
	down(g, 0, 500.0f, 500.0f, 0);
	touch_gesture_tick(&g, 299 * kMs);
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_PENDING);
	touch_gesture_tick(&g, 300 * kMs);
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_SELECT_HOLD);
	CHECK(touch_gesture_select(&g, 300 * kMs));

	// Moving now is a select-drag: the aim follows, the rig stays put.
	move(g, 0, 1500.0f, 900.0f, 400 * kMs);
	struct xrt_pose p;
	touch_gesture_nav_pose(&g, &p);
	CHECK(near(p.position.x, 0.0f));
	CHECK(near(p.orientation.w, 1.0f));
	struct xrt_pose grip;
	REQUIRE(touch_gesture_aim(&g, nullptr, &grip));
	CHECK(grip.position.x > 0.0f);
	CHECK(touch_gesture_select(&g, 400 * kMs));

	up(g, 0, 1500.0f, 900.0f, 500 * kMs);
	touch_gesture_tick(&g, 501 * kMs);
	CHECK_FALSE(touch_gesture_select(&g, 501 * kMs));
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_IDLE);
}

TEST_CASE("touch_gesture: a double tap recenters and raises the durable recenter level", "[touch]")
{
	auto g = fresh();
	// Disturb the rig first.
	down(g, 0, 100.0f, 100.0f, 0);
	move(g, 0, 900.0f, 700.0f, 20 * kMs);
	up(g, 0, 900.0f, 700.0f, 40 * kMs);
	struct xrt_pose p;
	touch_gesture_nav_pose(&g, &p);
	REQUIRE_FALSE(near(p.position.x, 0.0f));

	// Tap, tap.
	down(g, 0, 1000.0f, 800.0f, 1000 * kMs);
	up(g, 0, 1000.0f, 800.0f, 1050 * kMs);
	CHECK(touch_gesture_select(&g, 1051 * kMs)); // first tap clicks...
	down(g, 0, 1002.0f, 801.0f, 1200 * kMs);
	up(g, 0, 1002.0f, 801.0f, 1240 * kMs);
	touch_gesture_tick(&g, 1241 * kMs);
	CHECK_FALSE(touch_gesture_select(&g, 1241 * kMs)); // ...the pair reads as one recenter
	CHECK(touch_gesture_recenter_timestamp(&g) == 1240 * kMs);

	touch_gesture_nav_pose(&g, &p);
	CHECK(near(p.position.x, 0.0f));
	CHECK(near(p.position.y, 0.0f));
	CHECK(near(p.position.z, 0.0f));
	CHECK(near(p.orientation.w, 1.0f));

	// Two taps too far apart are two clicks, not a recenter; the level is unchanged.
	down(g, 0, 1000.0f, 800.0f, 2000 * kMs);
	up(g, 0, 1000.0f, 800.0f, 2050 * kMs);
	down(g, 0, 1000.0f, 800.0f, 2500 * kMs);
	up(g, 0, 1000.0f, 800.0f, 2550 * kMs);
	CHECK(touch_gesture_select(&g, 2551 * kMs));
	CHECK(touch_gesture_recenter_timestamp(&g) == 1240 * kMs);
}

TEST_CASE("touch_gesture: CANCEL lifts every finger without a click", "[touch]")
{
	auto g = fresh();
	down(g, 0, 100.0f, 100.0f, 0);
	down(g, 1, 300.0f, 300.0f, 1 * kMs, 2);
	auto c = ev(XRT_INPUT_POINTER_ACTION_CANCEL, 0, 0.0f, 0.0f, 2 * kMs, 0);
	touch_gesture_feed(&g, &c);
	touch_gesture_tick(&g, 3 * kMs);
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_IDLE);
	CHECK_FALSE(touch_gesture_select(&g, 3 * kMs));
	// The next finger is a fresh gesture.
	down(g, 5, 100.0f, 100.0f, 10 * kMs);
	CHECK(touch_gesture_mode(&g) == TOUCH_GESTURE_MODE_PENDING);
}

TEST_CASE("touch_gesture: sensitivity scales the orbit", "[touch]")
{
	struct touch_gesture g;
	struct touch_gesture_config cfg;
	touch_gesture_config_defaults(&cfg);
	cfg.sensitivity = 2.0f;
	touch_gesture_init(&g, &cfg);
	down(g, 0, 0.0f, 0.0f, 0);
	move(g, 0, kW / 4.0f, 0.0f, 20 * kMs); // a quarter width at 2x = -pi/2 yaw
	struct xrt_pose p;
	touch_gesture_nav_pose(&g, &p);
	CHECK(near(p.orientation.y, std::sin(-M_PI / 4), 1e-3f));
	CHECK(near(p.orientation.w, std::cos(-M_PI / 4), 1e-3f));
}

TEST_CASE("u_pointer_sink: FIFO, drains, and drops the OLDEST on overflow", "[touch]")
{
	u_pointer_sink_reset();
	struct xrt_input_pointer_event out[8];
	CHECK(u_pointer_sink_poll(out, 8) == 0);

	for (uint32_t i = 0; i < 3; i++) {
		auto e = ev(XRT_INPUT_POINTER_ACTION_MOVE, i, (float)i, 0.0f, i, 1);
		u_pointer_sink_push(&e);
	}
	REQUIRE(u_pointer_sink_poll(out, 2) == 2);
	CHECK(out[0].pointer_id == 0);
	CHECK(out[1].pointer_id == 1);
	REQUIRE(u_pointer_sink_poll(out, 8) == 1);
	CHECK(out[0].pointer_id == 2);
	CHECK(out[0].struct_size == sizeof(struct xrt_input_pointer_event));

	// Overflow by 5: the five oldest are gone, the capacity newest remain in order.
	for (uint32_t i = 0; i < U_POINTER_SINK_CAPACITY + 5; i++) {
		auto e = ev(XRT_INPUT_POINTER_ACTION_MOVE, i, 0.0f, 0.0f, i, 1);
		u_pointer_sink_push(&e);
	}
	uint64_t pushed = 0, dropped = 0;
	u_pointer_sink_get_stats(&pushed, &dropped);
	CHECK(pushed == 3u + U_POINTER_SINK_CAPACITY + 5u);
	CHECK(dropped == 5u);
	struct xrt_input_pointer_event big[U_POINTER_SINK_CAPACITY];
	REQUIRE(u_pointer_sink_poll(big, U_POINTER_SINK_CAPACITY) == U_POINTER_SINK_CAPACITY);
	CHECK(big[0].pointer_id == 5u);
	CHECK(big[U_POINTER_SINK_CAPACITY - 1].pointer_id == U_POINTER_SINK_CAPACITY + 4u);
	CHECK(u_pointer_sink_poll(big, 1) == 0);
	u_pointer_sink_reset();
}
