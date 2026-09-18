// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  One source of truth for the ACTIVE rendering mode's per-view scale.
 *
 * `xrt_system_compositor_info::recommended_view_scale_*` used to be BOTH the
 * display-level baseline and a cache of the active mode's scale, refreshed by
 * hand at three mode-change sites. The cache is gone; the derivation is
 * @ref xrt_device_get_active_mode_view_scale and that is what is pinned here.
 *
 * The accessor is a pure function of a device struct, so unlike the mode-change
 * WIRING (which needs an instance, a system, a head device and a plug-in) it is
 * fully host-drivable. What is NOT pinned here, and stays on the Windows/Leia
 * hardware leg: that XrDisplayInfoDXR and the MCP dump actually route through
 * it, and that a real mode change moves the answer.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_device.h"

namespace {

//! A head device with `count` modes, mode i scaled (0.1*(i+1), 0.2*(i+1)).
struct fake_head
{
	xrt_device xdev{};
	xrt_hmd_parts hmd{};

	explicit fake_head(uint32_t count, uint32_t active = 0)
	{
		xdev.rendering_mode_count = count;
		for (uint32_t i = 0; i < count && i < XRT_MAX_RENDERING_MODES; i++) {
			xdev.rendering_modes[i].mode_index = i;
			xdev.rendering_modes[i].view_count = (i == 0) ? 1 : 2;
			xdev.rendering_modes[i].view_scale_x = 0.1f * (float)(i + 1);
			xdev.rendering_modes[i].view_scale_y = 0.2f * (float)(i + 1);
		}
		hmd.active_rendering_mode_index = active;
		xdev.hmd = &hmd;
	}
};

} // namespace

TEST_CASE("active mode view scale: reads the ACTIVE index, not index 0")
{
	fake_head h(3, /* active */ 2);

	float x = -1.0f, y = -1.0f;
	REQUIRE(xrt_device_get_active_mode_view_scale(&h.xdev, &x, &y));
	CHECK(x == Catch::Approx(0.3f));
	CHECK(y == Catch::Approx(0.6f));

	// Moving the active index is the WHOLE update — nothing else to refresh.
	h.hmd.active_rendering_mode_index = 0;
	REQUIRE(xrt_device_get_active_mode_view_scale(&h.xdev, &x, &y));
	CHECK(x == Catch::Approx(0.1f));
	CHECK(y == Catch::Approx(0.2f));
}

TEST_CASE("active mode view scale: failure leaves the caller's fallback intact")
{
	// Every caller seeds its output with the display-level baseline and lets the
	// accessor overwrite it, so a false MUST NOT touch the outputs.
	const float base_x = 0.75f, base_y = 0.875f;

	SECTION("no device")
	{
		float x = base_x, y = base_y;
		CHECK_FALSE(xrt_device_get_active_mode_view_scale(nullptr, &x, &y));
		CHECK(x == base_x);
		CHECK(y == base_y);
	}

	SECTION("non-HMD device (no hmd parts)")
	{
		fake_head h(2);
		h.xdev.hmd = nullptr;
		float x = base_x, y = base_y;
		CHECK_FALSE(xrt_device_get_active_mode_view_scale(&h.xdev, &x, &y));
		CHECK(x == base_x);
		CHECK(y == base_y);
	}

	SECTION("empty mode table")
	{
		fake_head h(0);
		float x = base_x, y = base_y;
		CHECK_FALSE(xrt_device_get_active_mode_view_scale(&h.xdev, &x, &y));
		CHECK(x == base_x);
		CHECK(y == base_y);
	}

	SECTION("active index past the end of the table")
	{
		fake_head h(2, /* active */ 7);
		float x = base_x, y = base_y;
		CHECK_FALSE(xrt_device_get_active_mode_view_scale(&h.xdev, &x, &y));
		CHECK(x == base_x);
		CHECK(y == base_y);
	}

	SECTION("active mode declares no scale — a driver that fills only the plug-in value")
	{
		fake_head h(2, /* active */ 1);
		h.xdev.rendering_modes[1].view_scale_x = 0.0f;
		h.xdev.rendering_modes[1].view_scale_y = 0.0f;
		float x = base_x, y = base_y;
		CHECK_FALSE(xrt_device_get_active_mode_view_scale(&h.xdev, &x, &y));
		CHECK(x == base_x);
		CHECK(y == base_y);
	}

	SECTION("one axis missing is still a refusal — never a half-written pair")
	{
		fake_head h(2, /* active */ 1);
		h.xdev.rendering_modes[1].view_scale_y = -1.0f;
		float x = base_x, y = base_y;
		CHECK_FALSE(xrt_device_get_active_mode_view_scale(&h.xdev, &x, &y));
		CHECK(x == base_x);
		CHECK(y == base_y);
	}
}
