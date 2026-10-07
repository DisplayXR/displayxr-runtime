// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Multi-screen M1: one registry monitor -> its xrt_screen_info.
 *
 * Pins the three rules of u_screen_info_resolve() (util/u_screen_info.h):
 *  1. the system-default screen reports the system info verbatim;
 *  2. otherwise the owning plug-in's get_display_info_for_monitor slot;
 *  3. otherwise EDID-derived defaults at the system panel's vertical FOV.
 * Plus the ABI gate: a plug-in whose struct_size predates the slot is never
 * called through it.
 */

#include "catch_amalgamated.hpp"

#include "util/u_screen_info.h"

#include <cstddef>
#include <cstring>

namespace {

struct fake_slot_state
{
	int calls = 0;
	bool answer = true;
	uint32_t seen_mm_w = 0;
	uint64_t seen_monitor = 0;
} g_slot;

bool
fake_get_display_info_for_monitor(struct xrt_plugin_instance * /*inst*/,
                                  const struct xrt_display_descriptor *display,
                                  const struct xrt_display_physical *physical,
                                  struct xrt_plugin_display_info *out)
{
	g_slot.calls++;
	g_slot.seen_monitor = display->monitor_id;
	g_slot.seen_mm_w = physical != nullptr ? physical->physical_width_mm : 0;
	if (!g_slot.answer) {
		return false;
	}
	out->display_width_m = 0.5f;
	out->display_height_m = 0.3f;
	out->nominal_viewer_y_m = 0.1f;
	out->nominal_viewer_z_m = 0.9f;
	out->display_pixel_width = 2560;
	out->display_pixel_height = 1440;
	out->supported_eye_tracking_modes = 2u; // MANUAL
	out->default_eye_tracking_mode = 1u;
	// recommended_view_scale left 0: "let the runtime derive".
	return true;
}

struct fixture
{
	xrt_plugin_iface iface{};
	xrt_display_descriptor desc{};
	xrt_screen_info system{};
	u_screen_info_inputs in{};

	fixture()
	{
		g_slot = fake_slot_state{};

		iface.struct_size = (uint32_t)sizeof(iface);
		iface.get_display_info_for_monitor = fake_get_display_info_for_monitor;

		desc.struct_size = (uint32_t)sizeof(desc);
		desc.monitor_id = 0x1234;
		desc.pixel_width = 3456;
		desc.pixel_height = 2160;

		// A 0.344 x 0.193 m panel viewed from 0.6 m — the reference FOV.
		system.width_m = 0.344f;
		system.height_m = 0.193f;
		system.nominal_viewer_z_m = 0.6f;
		system.recommended_view_scale_x = 0.5f;
		system.recommended_view_scale_y = 0.5f;
		system.pixel_width = 3840;
		system.pixel_height = 2160;
		system.supported_eye_tracking_modes = 1u;
		system.source = XRT_SCREEN_INFO_SOURCE_SYSTEM;

		in.desc = &desc;
		in.physical.struct_size = (uint32_t)sizeof(in.physical);
		in.physical.physical_width_mm = 300;
		in.physical.physical_height_mm = 190;
		in.physical.native_pixel_width = 2880;
		in.physical.native_pixel_height = 1800;
		in.desktop_width = 3456;
		in.desktop_height = 2160;
		in.iface = &iface;
		in.system_info = &system;
	}
};

} // namespace

TEST_CASE("system-default screen reports the system info verbatim, never the slot")
{
	fixture f;
	f.in.is_system_default = true;

	xrt_screen_info out{};
	CHECK(u_screen_info_resolve(&f.in, &out) == XRT_SCREEN_INFO_SOURCE_SYSTEM);
	CHECK(g_slot.calls == 0);
	CHECK(out.width_m == f.system.width_m);
	CHECK(out.nominal_viewer_z_m == f.system.nominal_viewer_z_m);
	CHECK(out.recommended_view_scale_x == 0.5f);
	CHECK(out.supported_eye_tracking_modes == 1u);
}

TEST_CASE("non-default screen with the plug-in slot: the plug-in answers")
{
	fixture f;

	xrt_screen_info out{};
	CHECK(u_screen_info_resolve(&f.in, &out) == XRT_SCREEN_INFO_SOURCE_PLUGIN_MONITOR);
	CHECK(g_slot.calls == 1);
	CHECK(g_slot.seen_monitor == 0x1234);
	CHECK(g_slot.seen_mm_w == 300); // the EDID mm reach the plug-in
	CHECK(out.width_m == 0.5f);
	CHECK(out.height_m == 0.3f);
	CHECK(out.nominal_viewer_y_m == 0.1f);
	CHECK(out.nominal_viewer_z_m == 0.9f);
	CHECK(out.pixel_width == 2560);
	CHECK(out.supported_eye_tracking_modes == 2u);
	CHECK(out.default_eye_tracking_mode == 1u);
	// "0 = let the runtime derive" resolves to native for a non-default screen.
	CHECK(out.recommended_view_scale_x == 1.0f);
	CHECK(out.recommended_view_scale_y == 1.0f);
}

TEST_CASE("non-default screen without the slot: EDID-derived at the reference vertical FOV")
{
	fixture f;
	f.iface.get_display_info_for_monitor = nullptr;

	xrt_screen_info out{};
	CHECK(u_screen_info_resolve(&f.in, &out) == XRT_SCREEN_INFO_SOURCE_DERIVED);
	CHECK(out.width_m == Catch::Approx(0.300f));
	CHECK(out.height_m == Catch::Approx(0.190f));
	CHECK(out.pixel_width == 2880); // the connector's device mode, not the desktop's
	CHECK(out.pixel_height == 1800);
	CHECK(out.recommended_view_scale_x == 1.0f);
	CHECK(out.supported_eye_tracking_modes == 0u);
	CHECK(out.nominal_viewer_x_m == 0.0f);
	CHECK(out.nominal_viewer_y_m == 0.0f);
	// Same vertical FOV: z / h equal on both screens.
	CHECK(out.nominal_viewer_z_m == Catch::Approx(0.190f * 0.6f / 0.193f));
	CHECK(out.nominal_viewer_z_m / out.height_m == Catch::Approx(f.system.nominal_viewer_z_m / f.system.height_m));
}

TEST_CASE("slot that declines falls back to the EDID derivation")
{
	fixture f;
	g_slot.answer = false;

	xrt_screen_info out{};
	CHECK(u_screen_info_resolve(&f.in, &out) == XRT_SCREEN_INFO_SOURCE_DERIVED);
	CHECK(g_slot.calls == 1);
	CHECK(out.width_m == Catch::Approx(0.300f));
}

TEST_CASE("a plug-in built before the slot existed is never called through it")
{
	fixture f;
	f.iface.struct_size = (uint32_t)offsetof(xrt_plugin_iface, get_display_info_for_monitor);

	xrt_screen_info out{};
	CHECK(u_screen_info_resolve(&f.in, &out) == XRT_SCREEN_INFO_SOURCE_DERIVED);
	CHECK(g_slot.calls == 0);
}

TEST_CASE("unknown mm and no device mode: desktop pixels, reference viewer distance")
{
	fixture f;
	f.iface.get_display_info_for_monitor = nullptr;
	f.in.physical.physical_width_mm = 0;
	f.in.physical.physical_height_mm = 0;
	f.in.physical.native_pixel_width = 0;
	f.in.physical.native_pixel_height = 0;

	xrt_screen_info out{};
	CHECK(u_screen_info_resolve(&f.in, &out) == XRT_SCREEN_INFO_SOURCE_DERIVED);
	CHECK(out.width_m == 0.0f);
	CHECK(out.pixel_width == 3456);
	CHECK(out.pixel_height == 2160);
	CHECK(out.nominal_viewer_z_m == Catch::Approx(0.6f));
}

TEST_CASE("no system info at all: default viewer distance, system-default flag cannot copy")
{
	fixture f;
	f.iface.get_display_info_for_monitor = nullptr;
	f.in.system_info = nullptr;
	f.in.is_system_default = true;

	xrt_screen_info out{};
	CHECK(u_screen_info_resolve(&f.in, &out) == XRT_SCREEN_INFO_SOURCE_DERIVED);
	CHECK(out.nominal_viewer_z_m == Catch::Approx(U_SCREEN_INFO_DEFAULT_VIEWER_Z_M));
}

TEST_CASE("xrt_screen_list_find: by id, never id 0")
{
	xrt_screen_list list{};
	list.count = 2;
	list.screens[0].id = 7;
	list.screens[1].id = 9;
	CHECK(xrt_screen_list_find(&list, 9) == &list.screens[1]);
	CHECK(xrt_screen_list_find(&list, 8) == nullptr);
	CHECK(xrt_screen_list_find(&list, 0) == nullptr);
	CHECK(xrt_screen_list_find(nullptr, 7) == nullptr);
}
