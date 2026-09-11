// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "qwerty_device.h"
#include "qwerty_interface.h"
#include "util/u_camera_profile.h"

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
