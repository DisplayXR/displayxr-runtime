// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "qwerty_device.h"
#include "qwerty_interface.h"
#include "util/u_camera_profile.h"
#include "math/m_api.h"

namespace {
struct Devices
{
	xrt_device *devices[3]{};
	Devices()
	{
		REQUIRE(qwerty_create_devices(U_LOGGING_ERROR, &devices[0], &devices[1], &devices[2]) == XRT_SUCCESS);
	}
	~Devices()
	{
		for (int i = 2; i >= 0; --i)
			if (devices[i])
				devices[i]->destroy(devices[i]);
	}
	qwerty_system *
	system()
	{
		return qwerty_device(devices[0])->sys;
	}
	qwerty_view_state
	state()
	{
		qwerty_view_state result{};
		REQUIRE(qwerty_get_view_state(devices, 3, &result));
		return result;
	}
};
} // namespace

TEST_CASE("An instance camera profile seeds once and resets tuning without taking navigation",
          "[qwerty][camera-profile]")
{
	Devices d;
	u_camera_profile profile{.7f, .3f, 1.0f, .6f, .5f};
	qwerty_device(d.devices[0])->pose.position = {3, 4, 5};
	REQUIRE(qwerty_set_camera_profile(d.devices, 3, &profile));
	CHECK(d.state().ipd_factor == .7f);
	CHECK(d.state().parallax_factor == .3f);
	CHECK(d.state().m2v == .5f);
	CHECK(qwerty_device(d.devices[0])->pose.position.x == 3);
	CHECK_FALSE(qwerty_set_camera_profile(d.devices, 3, &profile));
	CHECK(d.state().m2v == .5f);

	qwerty_adjust_convergence(d.system(), -1);
	CHECK(d.state().inv_convergence_distance < 1);
	qwerty_adjust_view_factor(d.system(), .5f);
	CHECK(d.state().ipd_factor != .7f);
	qwerty_reset_view_state(d.system());
	CHECK(d.state().ipd_factor == .7f);
	CHECK(d.state().parallax_factor == .3f);
	CHECK(d.state().inv_convergence_distance == 1);
	CHECK(d.state().half_tan_vfov == .6f);
	CHECK(d.state().m2v == .5f);

	// A distinct qwerty system (the next instance) has its own defaults/seed.
	Devices fresh;
	CHECK(fresh.state().ipd_factor == 1);
	CHECK(fresh.state().parallax_factor == 1);
	CHECK(fresh.state().inv_convergence_distance == .5f);
	CHECK(fresh.state().half_tan_vfov == .3249f);
	CHECK(fresh.state().m2v == 1);
	REQUIRE(qwerty_set_camera_profile(fresh.devices, 3, &profile));
	CHECK(d.state().m2v == .5f);
}

TEST_CASE("Profile seeding leaves a display-mode qwerty system alone", "[qwerty][camera-profile]")
{
	Devices d;
	qwerty_toggle_camera_mode(d.system());
	const auto previous = d.state();
	REQUIRE_FALSE(previous.camera_mode);
	u_camera_profile profile{.7f, .3f, 1, .6f, .5f};
	CHECK_FALSE(qwerty_set_camera_profile(d.devices, 3, &profile));
	CHECK_FALSE(d.state().camera_mode);
	CHECK(d.state().virtual_display_height == previous.virtual_display_height);
}

TEST_CASE("The mouse hand sits on the cursor's ray and aims along it", "[qwerty][mouse-hand]")
{
	xrt_pose pose{};
	// Centre of the view: straight ahead at the requested depth, looking forward.
	qwerty_mouse_hand_pose(0, 0, 1.6f, 0.5f, 0.8f, &pose);
	CHECK(pose.position.x == Catch::Approx(0).margin(1e-6));
	CHECK(pose.position.y == Catch::Approx(0).margin(1e-6));
	CHECK(pose.position.z == Catch::Approx(-0.8));
	CHECK(pose.orientation.w == Catch::Approx(1));

	// Top-right corner: on the frustum edge, so it projects onto the corner.
	qwerty_mouse_hand_pose(1, 1, 1.6f, 0.5f, 2.0f, &pose);
	CHECK(pose.position.x == Catch::Approx(1 * 0.5 * 1.6 * 2.0));
	CHECK(pose.position.y == Catch::Approx(1 * 0.5 * 2.0));
	CHECK(pose.position.z == Catch::Approx(-2.0));
	// Its aim (-Z rotated) points along the same ray.
	xrt_vec3 aim{0, 0, -1};
	math_quat_rotate_vec3(&pose.orientation, &aim, &aim);
	const float len = std::sqrt(pose.position.x * pose.position.x + pose.position.y * pose.position.y +
	                            pose.position.z * pose.position.z);
	CHECK(aim.x == Catch::Approx(pose.position.x / len));
	CHECK(aim.y == Catch::Approx(pose.position.y / len));
	CHECK(aim.z == Catch::Approx(pose.position.z / len));
}
