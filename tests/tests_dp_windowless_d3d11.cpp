// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The windowless D3D11 DP rule (ADR-060 decision 4), shared by the
 *         native texture tier and the hub's client-presents strategy (#2370).
 *
 * Pins the three gates in order: the plug-in's HINT (no create when absent),
 * the per-screen factory's answer, and the instance's
 * PHASE_FROM_PRESENT_ORIGIN declaration (a DP without it is destroyed, never
 * returned) — and that the screen binding handed to the factory is the
 * registry entry's screen, with no window.
 */

#include "catch_amalgamated.hpp"

#include "util/comp_dp_windowless_d3d11.h"

#include <cstdio>
#include <cstring>

namespace {

int g_creates = 0;
int g_destroys = 0;
bool g_factory_ok = true;
uint32_t g_flags = 0;
void *g_seen_window = reinterpret_cast<void *>(0x1);
struct xrt_screen_binding g_seen_binding;

bool
fake_caps(struct xrt_display_processor_d3d11 *xdp, struct xrt_dp_scanout_caps *out_caps)
{
	(void)xdp;
	out_caps->flags = g_flags;
	return true;
}

void
fake_destroy(struct xrt_display_processor_d3d11 *xdp)
{
	g_destroys++;
	delete xdp;
}

xrt_result_t
fake_create_for_screen(struct xrt_plugin_instance *inst,
                       void *device,
                       void *context,
                       void *window_handle,
                       const struct xrt_screen_binding *binding,
                       struct xrt_display_processor_d3d11 **out_xdp)
{
	(void)inst;
	(void)device;
	(void)context;
	g_creates++;
	g_seen_window = window_handle;
	g_seen_binding = *binding;
	if (!g_factory_ok) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	auto *dp = new xrt_display_processor_d3d11();
	std::memset(dp, 0, sizeof(*dp));
	dp->struct_size = sizeof(*dp);
	dp->get_scanout_caps = fake_caps;
	dp->destroy = fake_destroy;
	*out_xdp = dp;
	return XRT_SUCCESS;
}

struct fixture
{
	struct xrt_plugin_iface iface;
	struct xrt_dp_registry_entry entry;

	fixture()
	{
		std::memset(&iface, 0, sizeof(iface));
		iface.struct_size = sizeof(iface);
		iface.create_dp_d3d11_for_screen = fake_create_for_screen;
		std::memset(&entry, 0, sizeof(entry));
		entry.owning_iface = &iface;
		entry.monitor_id = 0x1234;
		entry.screen_left = -3840;
		entry.screen_top = 0;
		entry.pixel_width = 3840;
		entry.pixel_height = 2160;
		entry.claim_apis = XRT_DP_API_BIT_D3D11 | XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE;
		std::snprintf(entry.serial, sizeof(entry.serial), "%s", "SN1");
		g_creates = 0;
		g_destroys = 0;
		g_factory_ok = true;
		g_flags = XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN;
		g_seen_window = reinterpret_cast<void *>(0x1);
	}
};

} // namespace

TEST_CASE("dp_windowless_d3d11: a hinted screen whose DP declares the flag yields it, windowless")
{
	fixture f;
	struct xrt_display_processor_d3d11 *dp = nullptr;
	const char *why = "unset";
	REQUIRE(comp_dp_make_windowless_d3d11(&f.entry, nullptr, nullptr, &dp, &why, nullptr));
	CHECK(dp != nullptr);
	CHECK(why == nullptr);
	CHECK(g_creates == 1);
	CHECK(g_seen_window == nullptr); // never a window
	CHECK(g_seen_binding.monitor_id == 0x1234);
	CHECK(g_seen_binding.desktop_left == -3840);
	CHECK(g_seen_binding.desktop_width == 3840);
	CHECK(std::strcmp(g_seen_binding.serial, "SN1") == 0);
	xrt_display_processor_d3d11_destroy(&dp);
	CHECK(g_destroys == 1);
}

TEST_CASE("dp_windowless_d3d11: no hint, no create")
{
	fixture f;
	f.entry.claim_apis = XRT_DP_API_BIT_D3D11;
	struct xrt_display_processor_d3d11 *dp = nullptr;
	const char *why = nullptr;
	CHECK_FALSE(comp_dp_make_windowless_d3d11(&f.entry, nullptr, nullptr, &dp, &why, nullptr));
	CHECK(dp == nullptr);
	CHECK(std::strcmp(why, COMP_DP_WINDOWLESS_NO_HINT) == 0);
	CHECK(g_creates == 0);
}

TEST_CASE("dp_windowless_d3d11: a DP without PHASE_FROM_PRESENT_ORIGIN is destroyed, not returned")
{
	fixture f;
	g_flags = XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE;
	struct xrt_display_processor_d3d11 *dp = nullptr;
	const char *why = nullptr;
	CHECK_FALSE(comp_dp_make_windowless_d3d11(&f.entry, nullptr, nullptr, &dp, &why, nullptr));
	CHECK(dp == nullptr);
	CHECK(std::strcmp(why, COMP_DP_WINDOWLESS_NO_FLAG) == 0);
	CHECK(g_creates == 1);
	CHECK(g_destroys == 1);
}

TEST_CASE("dp_windowless_d3d11: factory refusal and missing factory are named")
{
	fixture f;
	g_factory_ok = false;
	struct xrt_display_processor_d3d11 *dp = nullptr;
	const char *why = nullptr;
	xrt_result_t ret = XRT_SUCCESS;
	CHECK_FALSE(comp_dp_make_windowless_d3d11(&f.entry, nullptr, nullptr, &dp, &why, &ret));
	CHECK(std::strcmp(why, COMP_DP_WINDOWLESS_REFUSED) == 0);
	CHECK(ret == XRT_ERROR_DEVICE_CREATION_FAILED);

	f.iface.create_dp_d3d11_for_screen = nullptr;
	CHECK_FALSE(comp_dp_make_windowless_d3d11(&f.entry, nullptr, nullptr, &dp, &why, nullptr));
	CHECK(std::strcmp(why, COMP_DP_WINDOWLESS_NO_FACTORY) == 0);

	CHECK_FALSE(comp_dp_make_windowless_d3d11(nullptr, nullptr, nullptr, &dp, &why, nullptr));
	CHECK(std::strcmp(why, COMP_DP_WINDOWLESS_NO_FACTORY) == 0);
}
