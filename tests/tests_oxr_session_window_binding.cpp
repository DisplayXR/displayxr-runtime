// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "oxr_session_window_binding.h"

TEST_CASE("Profile eligibility uses desktop Linux's late app-window binding", "[oxr][window-binding]")
{
	// Vulkan Xlib and Wayland unpack their app-owned handles in create_impl,
	// after the common xrt_session_info was formed. populate_vk_native reports
	// external=true, while all three common pointer slots are still empty.
	// The profile gate must see this evidence without changing shared session
	// rendering, input or reference-space classification.
	xrt_session_info common{};
	const bool populated_external = true;
	REQUIRE(common.external_window_handle == nullptr);
	REQUIRE(common.readback_callback == nullptr);
	REQUIRE(common.shared_texture_handle == nullptr);
	CHECK(oxr_camera_profile_has_external_binding(true, populated_external, &common));

	// A runtime-owned native window has neither an app binding nor a populated
	// external classification and remains eligible for the camera profile.
	CHECK_FALSE(oxr_camera_profile_has_external_binding(true, false, &common));
	// Android's backend also reports a handle for a runtime-created hosted
	// window. That is not an app binding and must not exclude its profile.
	CHECK_FALSE(oxr_camera_profile_has_external_binding(false, populated_external, &common));
}

TEST_CASE("Common window, readback and shared-texture bindings remain external", "[oxr][window-binding]")
{
	int handle;
	xrt_session_info window{};
	window.external_window_handle = &handle;
	CHECK(oxr_camera_profile_has_external_binding(false, false, &window));
	CHECK(oxr_camera_profile_has_external_binding(true, false, &window));
	xrt_session_info readback{};
	readback.readback_callback = [](const uint8_t *, uint32_t, uint32_t, void *) {};
	CHECK(oxr_camera_profile_has_external_binding(false, false, &readback));
	xrt_session_info shared{};
	shared.shared_texture_handle = &handle;
	CHECK(oxr_camera_profile_has_external_binding(false, false, &shared));
}
