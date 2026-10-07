// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Multi-screen M1: target_screens_build — registry -> xrt_screen_list.
 *
 * Drives the real builder against a hand-made registry. The loader's monitor
 * side table is filled through target_plugin_build_descriptors() from a fake
 * EDID list, exactly as build_dp_registry does, so the owning plug-in's
 * get_display_info_for_monitor slot is reachable.
 *
 * The case that motivated it: the active plug-in owns TWO monitors, the panel
 * resolver (#1301) placed the panel on the second, and the builder used to
 * take entries[0] as the system default and paint the second monitor's rect
 * onto it — one monitor's id, another's rect.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_screen.h"

#include "os/os_display_edid.h"

#include "target_plugin_loader.h"
#include "target_screens.h"

#include <cstdio>
#include <cstring>

namespace {

int g_slot_calls = 0;

bool
fake_slot(struct xrt_plugin_instance *,
          const xrt_display_descriptor *d,
          const xrt_display_physical *p,
          xrt_plugin_display_info *out)
{
	g_slot_calls++;
	out->display_width_m = (float)p->physical_width_mm / 1000.0f;
	out->display_height_m = (float)p->physical_height_mm / 1000.0f;
	out->nominal_viewer_z_m = 0.7f;
	out->display_pixel_width = d->pixel_width;
	out->display_pixel_height = d->pixel_height;
	return true;
}

/*!
 * Three monitors, left to right: A (laptop, primary), B and C (two identical
 * panels the "vendor" plug-in owns).
 */
struct fixture
{
	xrt_plugin_iface vendor{};
	xrt_plugin_iface fallback{};
	xrt_display_descriptor descs[3]{};
	xrt_dp_factory_registry reg{};
	target_screens_system sys{};

	fixture()
	{
		g_slot_calls = 0;
		vendor.struct_size = (uint32_t)sizeof(vendor);
		vendor.id = "vendor";
		fallback.struct_size = (uint32_t)sizeof(fallback);
		fallback.id = "fallback";
		fallback.get_display_info_for_monitor = fake_slot;

		static os_display_edid_list edid;
		std::memset(&edid, 0, sizeof(edid));
		edid.count = 3;
		const struct
		{
			uint16_t mfr, product;
			int32_t left;
			uint32_t w, h, mm_w, mm_h;
			bool primary;
			const char *name;
		} mons[3] = {
		    {0x4C83, 0x423F, 0, 2880, 1800, 300, 190, true, "eDP-1"},
		    {0x0472, 0x0001, 2880, 3840, 2160, 344, 193, false, "HDMI-1"},
		    {0x0472, 0x0001, 6720, 3840, 2160, 344, 193, false, "DP-1"},
		};
		for (int i = 0; i < 3; i++) {
			os_display_edid_monitor &m = edid.monitors[i];
			m.manufacturer_id = mons[i].mfr;
			m.product_id = mons[i].product;
			m.screen_left = mons[i].left;
			m.pixel_width = mons[i].w;
			m.pixel_height = mons[i].h;
			m.refresh_hz = 60;
			m.is_primary = mons[i].primary;
			m.physical_width_mm = mons[i].mm_w;
			m.physical_height_mm = mons[i].mm_h;
			m.native_width = mons[i].w;
			m.native_height = mons[i].h;
			std::snprintf(m.output_name, sizeof(m.output_name), "%s", mons[i].name);
			std::snprintf(m.connector, sizeof(m.connector), "%s", mons[i].name);
		}
		REQUIRE(target_plugin_build_descriptors(&edid, descs, 3) == 3);

		reg.entry_count = 3;
		for (int i = 0; i < 3; i++) {
			xrt_dp_registry_entry &e = reg.entries[i];
			e.monitor_id = descs[i].monitor_id;
			e.screen_left = descs[i].screen_left;
			e.screen_top = descs[i].screen_top;
			e.pixel_width = descs[i].pixel_width;
			e.pixel_height = descs[i].pixel_height;
			const xrt_plugin_iface *owner = i == 0 ? &fallback : &vendor;
			std::snprintf(e.plugin_id, sizeof(e.plugin_id), "%s", owner->id);
			e.owning_iface = owner;
			e.confidence = i == 0 ? 10 : 50;
		}

		// The system panel: the vendor's, resolved onto the THIRD monitor.
		sys.info_valid = true;
		sys.info.width_m = 0.344f;
		sys.info.height_m = 0.193f;
		sys.info.nominal_viewer_z_m = 0.6f;
		sys.info.pixel_width = 3840;
		sys.info.pixel_height = 2160;
		sys.info.supported_eye_tracking_modes = 1u;
		sys.desktop_left = 6720;
		sys.desktop_top = 0;
		sys.desktop_width = 3840;
		sys.desktop_height = 2160;
		std::snprintf(sys.device_name, sizeof(sys.device_name), "DP-1");
	}
};

} // namespace

TEST_CASE("system default = the monitor the panel resolver picked, not entries[0] of the active plug-in")
{
	fixture f;
	xrt_screen_list list{};
	target_screens_build(&f.reg, "vendor", &f.sys, &list);

	REQUIRE(list.count == 3);
	const xrt_screen &def = list.screens[0];
	CHECK((def.flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0);
	CHECK(def.id == f.descs[2].monitor_id); // DP-1, where the panel is
	CHECK(def.desktop_left == 6720);        // its own rect, consistent with its id
	CHECK(std::strcmp(def.device_name, "DP-1") == 0);
	CHECK(def.info.source == XRT_SCREEN_INFO_SOURCE_SYSTEM);
	CHECK((def.flags & XRT_SCREEN_FLAG_TRACKED) != 0);

	// Exactly one default; every other screen keeps its own registry rect.
	int defaults = 0;
	for (uint32_t i = 0; i < list.count; i++) {
		defaults += (list.screens[i].flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0;
		if (i > 0) {
			const xrt_screen &s = list.screens[i];
			const xrt_dp_registry_entry *e = nullptr;
			for (uint32_t k = 0; k < f.reg.entry_count; k++) {
				if (f.reg.entries[k].monitor_id == s.id) {
					e = &f.reg.entries[k];
				}
			}
			REQUIRE(e != nullptr);
			CHECK(s.desktop_left == e->screen_left);
		}
	}
	CHECK(defaults == 1);
}

TEST_CASE("resolved origin on no monitor: primary_entry fallback, and its rect is NOT overwritten")
{
	fixture f;
	f.sys.desktop_left = -5000; // a rect no entry contains
	f.sys.desktop_width = 1234;
	xrt_screen_list list{};
	target_screens_build(&f.reg, "vendor", &f.sys, &list);

	REQUIRE(list.count == 3);
	const xrt_screen &def = list.screens[0];
	CHECK(def.id == f.descs[1].monitor_id); // primary_entry: first entry the active plug-in won
	CHECK(def.desktop_left == 2880);        // its own rect
	CHECK(def.desktop_width == 3840);
	CHECK(std::strcmp(def.device_name, "HDMI-1") == 0);
	CHECK(def.info.source == XRT_SCREEN_INFO_SOURCE_SYSTEM);
}

TEST_CASE("pick_default prefers the active plug-in's entry and reports whether the rect matched")
{
	fixture f;
	bool matched = false;
	CHECK(target_screens_pick_default(&f.reg, "vendor", &f.sys, &matched) == &f.reg.entries[2]);
	CHECK(matched);

	f.sys.desktop_width = 0; // nothing resolved
	CHECK(target_screens_pick_default(&f.reg, "vendor", &f.sys, &matched) == &f.reg.entries[1]);
	CHECK_FALSE(matched);

	xrt_dp_factory_registry empty{};
	CHECK(target_screens_pick_default(&empty, "vendor", &f.sys, &matched) == nullptr);
}

TEST_CASE("non-default screens: the owning plug-in's slot, else EDID-derived")
{
	fixture f;
	xrt_screen_list list{};
	target_screens_build(&f.reg, "vendor", &f.sys, &list);

	const xrt_screen *laptop = xrt_screen_list_find(&list, f.descs[0].monitor_id);
	const xrt_screen *hdmi = xrt_screen_list_find(&list, f.descs[1].monitor_id);
	REQUIRE(laptop != nullptr);
	REQUIRE(hdmi != nullptr);

	// eDP-1 is owned by the plug-in that implements the slot.
	CHECK(g_slot_calls == 1);
	CHECK(laptop->info.source == XRT_SCREEN_INFO_SOURCE_PLUGIN_MONITOR);
	CHECK(laptop->info.width_m == Catch::Approx(0.300f));
	CHECK(laptop->info.nominal_viewer_z_m == Catch::Approx(0.7f));
	CHECK((laptop->flags & XRT_SCREEN_FLAG_PRIMARY) != 0);
	CHECK(laptop->physical_width_mm == 300);
	CHECK(std::strcmp(laptop->plugin_id, "fallback") == 0);

	// HDMI-1 is the vendor's but not the system panel, and the vendor has no slot.
	CHECK(hdmi->info.source == XRT_SCREEN_INFO_SOURCE_DERIVED);
	CHECK(hdmi->info.supported_eye_tracking_modes == 0u);
	CHECK((hdmi->flags & XRT_SCREEN_FLAG_TRACKED) == 0);
	CHECK(hdmi->info.nominal_viewer_z_m == Catch::Approx(0.6f)); // same height -> same distance
}

TEST_CASE("empty registry: one synthesized system screen, or nothing when the system knows nothing")
{
	fixture f;
	xrt_dp_factory_registry empty{};
	xrt_screen_list list{};
	target_screens_build(&empty, "vendor", &f.sys, &list);
	REQUIRE(list.count == 1);
	CHECK(list.screens[0].id == XRT_SCREEN_ID_SYNTHETIC_DEFAULT);
	CHECK((list.screens[0].flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0);
	CHECK(list.screens[0].desktop_left == 6720);

	f.sys.info_valid = false;
	target_screens_build(&empty, "vendor", &f.sys, &list);
	CHECK(list.count == 0);
}
