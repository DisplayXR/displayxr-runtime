// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief #1692 — the qwerty driver's analytic velocity.
 *
 * Two layers. The first pins @ref qwerty_step_velocity itself: a known
 * translation over a known dt, a known rotation, and the composed
 * pitch+roll+yaw case, which is checked against the property that actually
 * matters — integrating the reported angular velocity over the step must
 * reproduce the orientation the step produced.
 *
 * The second drives the real devices, because the interesting claim is not
 * that the formula is right but that the pair the device hands out is
 * coherent: for a held key the reported rate is the instantaneous rate and is
 * the same however short the integration step happened to be.
 */

#include "catch_amalgamated.hpp"

#include "qwerty_device.h"
#include "qwerty_interface.h"

#include "math/m_api.h"
#include "xrt/xrt_defines.h"

#include <chrono>
#include <cmath>
#include <thread>

namespace {

//! Speeds, as the driver defines them: "per 60 Hz frame", so rate = value * 60.
constexpr float kFramesPerSecond = 60.0f;
constexpr float kControllerMovementPerFrame = 0.01f; // QWERTY_CONTROLLER_INITIAL_MOVEMENT_SPEED
constexpr float kControllerLookPerFrame = 0.05f;     // QWERTY_CONTROLLER_INITIAL_LOOK_SPEED
constexpr float kSprintBoost = 3.0517578125f;        // MOVEMENT_SPEED_STEP ^ SPRINT_STEPS = 1.25^5

constexpr float kControllerSpeed = kControllerMovementPerFrame * kFramesPerSecond; // 0.6 m/s
constexpr float kControllerLookRate = kControllerLookPerFrame * kFramesPerSecond;  // 3 rad/s
constexpr float kControllerSprintLookRate = kControllerLookRate * kSprintBoost;    // ~9.155 rad/s
constexpr float kControllerSprintSpeed = kControllerSpeed * kSprintBoost;          // ~1.83 m/s

//! The CTS SpaceOffsets auto-pass thresholds (test_SpaceOffsets.cpp:105-112).
constexpr float kCtsLinearCriterion = 0.5f;  // m/s on each of X, Y, Z
constexpr float kCtsAngularCriterion = 6.0f; // rad/s about each of X, Y, Z

xrt_quat
rot(float angle_rad, xrt_vec3 axis)
{
	xrt_quat q{};
	math_quat_from_angle_vector(angle_rad, &axis, &q);
	return q;
}

xrt_quat
mul(xrt_quat left, xrt_quat right)
{
	xrt_quat q{};
	math_quat_rotate(&left, &right, &q);
	return q;
}

//! Quaternions equal up to sign (q and -q are the same rotation).
void
check_same_rotation(xrt_quat got, xrt_quat want, float margin = 1e-4f)
{
	const float sign = (math_quat_dot(&got, &want) < 0) ? -1.0f : 1.0f;
	CHECK(sign * got.x == Catch::Approx(want.x).margin(margin));
	CHECK(sign * got.y == Catch::Approx(want.y).margin(margin));
	CHECK(sign * got.z == Catch::Approx(want.z).margin(margin));
	CHECK(sign * got.w == Catch::Approx(want.w).margin(margin));
}

/*!
 * Rebuild the end orientation from the reported angular velocity: the step's
 * base-frame rotation is exp((omega * dt) / 2) applied on the LEFT of the
 * start orientation.
 */
xrt_quat
integrate(xrt_vec3 angular, float dt_s, xrt_quat before)
{
	xrt_vec3 half{angular.x * dt_s * 0.5f, angular.y * dt_s * 0.5f, angular.z * dt_s * 0.5f};
	xrt_quat delta{};
	math_quat_exp(&half, &delta);
	math_quat_normalize(&delta);
	return mul(delta, before);
}

struct Devices
{
	xrt_device *devices[3]{};
	Devices()
	{
		REQUIRE(qwerty_create_devices(U_LOGGING_ERROR, &devices[0], &devices[1], &devices[2]) == XRT_SUCCESS);
	}
	~Devices()
	{
		for (int i = 2; i >= 0; --i) {
			if (devices[i] != nullptr) {
				devices[i]->destroy(devices[i]);
			}
		}
	}

	//! The left controller, which is what the CTS drives as /user/hand/left.
	struct qwerty_device *
	left()
	{
		return qwerty_device(devices[1]);
	}

	xrt_space_relation
	locate_left()
	{
		xrt_space_relation relation{};
		REQUIRE(xrt_device_get_tracked_pose(devices[1], XRT_INPUT_WMR_GRIP_POSE, 0, &relation) == XRT_SUCCESS);
		return relation;
	}
};

bool
has_both_velocities(const xrt_space_relation &relation)
{
	const uint32_t both = (uint32_t)XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT |
	                      (uint32_t)XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT;
	return ((uint32_t)relation.relation_flags & both) == both;
}

} // namespace


/*
 *
 * The step-velocity math.
 *
 */

TEST_CASE("A known translation over a known dt is that many metres per second", "[qwerty][velocity]")
{
	const xrt_quat identity = XRT_QUAT_IDENTITY;
	const xrt_vec3 pos_delta{0.02f, -0.01f, 0.005f};

	xrt_vec3 linear{};
	xrt_vec3 angular{};
	qwerty_step_velocity(&pos_delta, &identity, &identity, &identity, 0.02f, &linear, &angular);

	CHECK(linear.x == Catch::Approx(1.0f));
	CHECK(linear.y == Catch::Approx(-0.5f));
	CHECK(linear.z == Catch::Approx(0.25f));

	// No rotation applied: a real zero, not an unknown.
	CHECK(angular.x == Catch::Approx(0.0f).margin(1e-6f));
	CHECK(angular.y == Catch::Approx(0.0f).margin(1e-6f));
	CHECK(angular.z == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("A known yaw delta is a rad/s vector about base Y", "[qwerty][velocity]")
{
	const xrt_quat identity = XRT_QUAT_IDENTITY;
	const xrt_vec3 no_move = XRT_VEC3_ZERO;
	const float dt = 0.05f;
	const xrt_quat yaw = rot(0.1f, XRT_VEC3_UNIT_Y);

	xrt_vec3 linear{};
	xrt_vec3 angular{};
	qwerty_step_velocity(&no_move, &identity, &identity, &yaw, dt, &linear, &angular);

	CHECK(angular.x == Catch::Approx(0.0f).margin(1e-5f));
	CHECK(angular.y == Catch::Approx(2.0f).margin(1e-4f)); // 0.1 rad / 0.05 s
	CHECK(angular.z == Catch::Approx(0.0f).margin(1e-5f));
}

TEST_CASE("A device-local pitch is reported about the axis it really turned", "[qwerty][velocity]")
{
	const xrt_vec3 no_move = XRT_VEC3_ZERO;
	const xrt_quat identity = XRT_QUAT_IDENTITY;
	const float dt = 0.05f;
	const xrt_quat pitch = rot(0.1f, XRT_VEC3_UNIT_X);

	xrt_vec3 linear{};
	xrt_vec3 angular{};

	// From identity, the device's own X is the base X.
	qwerty_step_velocity(&no_move, &identity, &pitch, &identity, dt, &linear, &angular);
	CHECK(angular.x == Catch::Approx(2.0f).margin(1e-4f));
	CHECK(angular.y == Catch::Approx(0.0f).margin(1e-5f));
	CHECK(angular.z == Catch::Approx(0.0f).margin(1e-5f));

	// Turned 90 degrees about +Y first, the device's own X points down base -Z,
	// so the SAME local pitch is a rotation about base -Z. A finite difference
	// of two poses could not tell these two cases apart without the frame.
	const xrt_quat turned = rot(1.57079632679f, XRT_VEC3_UNIT_Y); // +90 degrees
	qwerty_step_velocity(&no_move, &turned, &pitch, &identity, dt, &linear, &angular);
	CHECK(angular.x == Catch::Approx(0.0f).margin(1e-4f));
	CHECK(angular.y == Catch::Approx(0.0f).margin(1e-4f));
	CHECK(angular.z == Catch::Approx(-2.0f).margin(1e-4f));
}

TEST_CASE("The composed pitch + roll + yaw step integrates back to the pose it produced", "[qwerty][velocity]")
{
	const xrt_vec3 pos_delta{0.004f, 0.002f, -0.006f};
	const float dt = 1.0f / 60.0f;

	// A deliberately awkward starting orientation, and all three axes at once.
	const xrt_quat before = mul(rot(0.7f, XRT_VEC3_UNIT_Y), rot(-0.4f, XRT_VEC3_UNIT_X));
	const xrt_quat local = mul(rot(0.05f, XRT_VEC3_UNIT_X), rot(0.03f, XRT_VEC3_UNIT_Z)); // pitch then roll
	const xrt_quat base = rot(-0.04f, XRT_VEC3_UNIT_Y);                                   // yaw

	// What the driver's integrator does: after = yaw * before * (pitch * roll).
	const xrt_quat after = mul(base, mul(before, local));

	xrt_vec3 linear{};
	xrt_vec3 angular{};
	qwerty_step_velocity(&pos_delta, &before, &local, &base, dt, &linear, &angular);

	CHECK(linear.x == Catch::Approx(pos_delta.x * kFramesPerSecond));
	CHECK(linear.y == Catch::Approx(pos_delta.y * kFramesPerSecond));
	CHECK(linear.z == Catch::Approx(pos_delta.z * kFramesPerSecond));

	check_same_rotation(integrate(angular, dt, before), after);
}

TEST_CASE("A zero-length step reports no velocity rather than dividing by it", "[qwerty][velocity]")
{
	const xrt_quat identity = XRT_QUAT_IDENTITY;
	const xrt_vec3 pos_delta{1.0f, 1.0f, 1.0f};

	xrt_vec3 linear{1, 2, 3};
	xrt_vec3 angular{4, 5, 6};
	qwerty_step_velocity(&pos_delta, &identity, &identity, &identity, 0.0f, &linear, &angular);

	CHECK(linear.x == 0.0f);
	CHECK(linear.y == 0.0f);
	CHECK(linear.z == 0.0f);
	CHECK(angular.x == 0.0f);
	CHECK(angular.y == 0.0f);
	CHECK(angular.z == 0.0f);
}


/*
 *
 * The device, end to end.
 *
 */

TEST_CASE("A held movement key reports the same rate however short the step", "[qwerty][velocity]")
{
	Devices d;

	qwerty_press_forward(d.left());

	// First poll: no previous timestamp, so the driver integrates one nominal
	// frame. Second poll: a real, and deliberately different, elapsed time.
	const xrt_space_relation first = d.locate_left();
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	const xrt_space_relation second = d.locate_left();

	for (const xrt_space_relation &relation : {first, second}) {
		CHECK(has_both_velocities(relation));
		CHECK(relation.linear_velocity.x == Catch::Approx(0.0f).margin(1e-4f));
		CHECK(relation.linear_velocity.y == Catch::Approx(0.0f).margin(1e-4f));
		CHECK(relation.linear_velocity.z == Catch::Approx(-kControllerSpeed).margin(1e-3f));
	}

	// 0.6 m/s clears the CTS criterion on its own axis.
	CHECK(std::fabs(second.linear_velocity.z) >= kCtsLinearCriterion);
}

TEST_CASE("Each movement key reaches the CTS linear criterion on its own base axis", "[qwerty][velocity]")
{
	Devices d;

	struct Case
	{
		void (*press)(struct qwerty_device *);
		void (*release)(struct qwerty_device *);
		int axis; // 0 = x, 1 = y, 2 = z
		float sign;
	};

	const Case cases[] = {
	    {qwerty_press_right, qwerty_release_right, 0, 1.0f},
	    {qwerty_press_left, qwerty_release_left, 0, -1.0f},
	    {qwerty_press_up, qwerty_release_up, 1, 1.0f},
	    {qwerty_press_down, qwerty_release_down, 1, -1.0f},
	    {qwerty_press_backward, qwerty_release_backward, 2, 1.0f},
	    {qwerty_press_forward, qwerty_release_forward, 2, -1.0f},
	};

	for (const Case &c : cases) {
		c.press(d.left());
		d.locate_left(); // arm the integrator's clock
		const xrt_space_relation relation = d.locate_left();
		c.release(d.left());

		const float v[3] = {relation.linear_velocity.x, relation.linear_velocity.y, relation.linear_velocity.z};
		CHECK(has_both_velocities(relation));
		CHECK(v[c.axis] == Catch::Approx(c.sign * kControllerSpeed).margin(1e-3f));
		CHECK(std::fabs(v[c.axis]) >= kCtsLinearCriterion);
	}
}

TEST_CASE("Sprint + the three rotation pairs reach the CTS angular criterion on every axis", "[qwerty][velocity]")
{
	Devices d;

	struct Case
	{
		void (*press)(struct qwerty_device *);
		void (*release)(struct qwerty_device *);
		int axis;
		float sign;
	};

	const Case cases[] = {
	    {qwerty_press_look_up, qwerty_release_look_up, 0, 1.0f},
	    {qwerty_press_look_down, qwerty_release_look_down, 0, -1.0f},
	    {qwerty_press_look_left, qwerty_release_look_left, 1, 1.0f},
	    {qwerty_press_look_right, qwerty_release_look_right, 1, -1.0f},
	    {qwerty_press_roll_left, qwerty_release_roll_left, 2, 1.0f},
	    {qwerty_press_roll_right, qwerty_release_roll_right, 2, -1.0f},
	};

	for (const Case &c : cases) {
		// Start each axis from the reset pose, so the device's own axes are
		// the base axes and the criterion is read on the axis under test.
		qwerty_reset_controller_pose(qwerty_controller(d.devices[1]));
		qwerty_press_sprint(d.left());
		c.press(d.left());
		d.locate_left(); // arm the integrator's clock
		const xrt_space_relation relation = d.locate_left();
		c.release(d.left());
		qwerty_release_sprint(d.left());

		const float w[3] = {relation.angular_velocity.x, relation.angular_velocity.y,
		                    relation.angular_velocity.z};
		CHECK(has_both_velocities(relation));
		CHECK(w[c.axis] == Catch::Approx(c.sign * kControllerSprintLookRate).margin(2e-2f));
		CHECK(std::fabs(w[c.axis]) >= kCtsAngularCriterion);
	}

	// Unboosted, the same keys fall short — which is why sprint boosts look.
	CHECK(kControllerLookRate < kCtsAngularCriterion);
	CHECK(kControllerSprintSpeed >= kCtsLinearCriterion);
}

TEST_CASE("A frame with no input reports zero velocity, still valid", "[qwerty][velocity]")
{
	Devices d;

	qwerty_press_forward(d.left());
	d.locate_left();
	qwerty_release_all(d.left());
	d.locate_left();

	const xrt_space_relation relation = d.locate_left();
	CHECK(has_both_velocities(relation));
	CHECK(relation.linear_velocity.x == Catch::Approx(0.0f).margin(1e-5f));
	CHECK(relation.linear_velocity.y == Catch::Approx(0.0f).margin(1e-5f));
	CHECK(relation.linear_velocity.z == Catch::Approx(0.0f).margin(1e-5f));
	CHECK(relation.angular_velocity.x == Catch::Approx(0.0f).margin(1e-5f));
	CHECK(relation.angular_velocity.y == Catch::Approx(0.0f).margin(1e-5f));
	CHECK(relation.angular_velocity.z == Catch::Approx(0.0f).margin(1e-5f));
}
