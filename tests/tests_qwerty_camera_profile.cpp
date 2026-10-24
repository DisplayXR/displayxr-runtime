// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "qwerty_device.h"
#include "qwerty_interface.h"
#include "util/u_camera_profile.h"

#include <cmath>

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

TEST_CASE("A profile pitch tilts the starting camera and is the Space reset target", "[qwerty][camera-profile]")
{
	Devices d;
	xrt_pose &pose = qwerty_device(d.devices[0])->pose;
	pose.position = {3, 4, 5};
	u_camera_profile profile = U_CAMERA_PROFILE_DEFAULT;
	profile.pitch_rad = -0.5235987756f; // 30 degrees down
	REQUIRE(qwerty_set_camera_profile(d.devices, 3, &profile));
	CHECK(pose.orientation.x == Catch::Approx(std::sin(-0.2617993878)));
	CHECK(pose.orientation.y == 0);
	CHECK(pose.orientation.z == 0);
	CHECK(pose.orientation.w == Catch::Approx(std::cos(-0.2617993878)));
	CHECK(pose.position.x == 3); // the starting look is framing; position stays the user's

	pose.orientation = {0, 0, 0, 1};
	qwerty_reset_view_state(d.system());
	CHECK(pose.orientation.x == Catch::Approx(std::sin(-0.2617993878)));

	// Without a pitch the camera keeps starting level.
	Devices level;
	xrt_pose &level_pose = qwerty_device(level.devices[0])->pose;
	u_camera_profile flat = U_CAMERA_PROFILE_DEFAULT;
	REQUIRE(qwerty_set_camera_profile(level.devices, 3, &flat));
	CHECK(level_pose.orientation.x == 0);
	CHECK(level_pose.orientation.w == 1);
}

TEST_CASE("A profile pullback moves the starting camera back along its look", "[qwerty][camera-profile]")
{
	Devices d;
	xrt_pose &pose = qwerty_device(d.devices[0])->pose;
	pose.position = {0, 1.6f, 0.6f};
	u_camera_profile profile = U_CAMERA_PROFILE_DEFAULT;
	profile.pitch_rad = -0.5235987756f; // 30 degrees down
	profile.pullback_m = 1.0f;
	REQUIRE(qwerty_set_camera_profile(d.devices, 3, &profile));
	// Backwards along a look tilted down: back (+Z) and up (+Y).
	CHECK(pose.position.x == 0);
	CHECK(pose.position.y == Catch::Approx(1.6 + 0.5));
	CHECK(pose.position.z == Catch::Approx(0.6 + std::cos(0.5235987756)));
}
