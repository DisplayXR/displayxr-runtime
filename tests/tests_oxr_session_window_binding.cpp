// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "oxr_session_window_binding.h"

TEST_CASE("Final session metadata retains a graphics-backend external binding", "[oxr][window-binding]")
{
	// Vulkan Xlib and Wayland unpack their app-owned handles in create_impl,
	// after the common xrt_session_info was formed. populate_vk_native reports
	// external=true, while all three common pointer slots are still empty.
	// The outer constructor formerly overwrote that classification with false,
	// enabling native legacy camera-profile seeding for an external-only app.
	xrt_session_info common{};
	const bool populated_external = true;
	REQUIRE(common.external_window_handle == nullptr);
	REQUIRE(common.readback_callback == nullptr);
	REQUIRE(common.shared_texture_handle == nullptr);
	CHECK(oxr_session_has_external_binding(populated_external, &common));

	// A runtime-owned native window has neither an app binding nor a populated
	// external classification and remains eligible for the camera profile.
	CHECK_FALSE(oxr_session_has_external_binding(false, &common));
}

TEST_CASE("Common window, readback and shared-texture bindings remain external", "[oxr][window-binding]")
{
	int handle;
	xrt_session_info window{};
	window.external_window_handle = &handle;
	CHECK(oxr_session_has_external_binding(false, &window));
	xrt_session_info readback{};
	readback.readback_callback = [](const uint8_t *, uint32_t, uint32_t, void *) {};
	CHECK(oxr_session_has_external_binding(false, &readback));
	xrt_session_info shared{};
	shared.shared_texture_handle = &handle;
	CHECK(oxr_session_has_external_binding(false, &shared));
}
