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

#include "os/os_display_desktop.h"
#include "os/os_display_edid.h"

#include "target_plugin_loader.h"
#include "target_screen_pin.h"
#include "target_screens.h"

#include <cstdio>
#include <cstring>
#include <string>

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

TEST_CASE("a FALLBACK claim's metres give way to the EDID's, the default screen's too (ADR-051)")
{
	fixture f;
	// The fallback plug-in is the active one and its panel is the laptop, at
	// its hard-coded 0.344 x 0.194 m; the EDID says 300 x 190 mm.
	f.sys.info.width_m = 0.344f;
	f.sys.info.height_m = 0.194f;
	f.sys.desktop_left = 0;
	f.sys.desktop_width = 2880;
	f.sys.desktop_height = 1800;
	std::snprintf(f.sys.device_name, sizeof(f.sys.device_name), "eDP-1");
	xrt_screen_list list{};
	target_screens_build(&f.reg, "fallback", &f.sys, &list);

	REQUIRE(list.count == 3);
	const xrt_screen &def = list.screens[0];
	CHECK(def.id == f.descs[0].monitor_id);
	CHECK(def.info.source == XRT_SCREEN_INFO_SOURCE_SYSTEM);
	CHECK(def.info.width_m == Catch::Approx(0.300f));
	CHECK(def.info.height_m == Catch::Approx(0.190f));
	CHECK(def.info.nominal_viewer_z_m == Catch::Approx(0.6f)); // the rest is the plug-in's

	// The vendor's EDID-confidence screens are not touched.
	const xrt_screen *hdmi = xrt_screen_list_find(&list, f.descs[1].monitor_id);
	REQUIRE(hdmi != nullptr);
	CHECK(hdmi->info.source == XRT_SCREEN_INFO_SOURCE_DERIVED);
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

TEST_CASE("monitor id: survives a rearrangement where the connector is known, separates identical panels")
{
	os_display_edid_list edid{};
	edid.count = 2;
	for (int i = 0; i < 2; i++) {
		os_display_edid_monitor &m = edid.monitors[i];
		m.manufacturer_id = 0x0472;
		m.product_id = 0x0001;
		m.serial_number = 0x322EF05E;
		m.pixel_width = 3840;
		m.pixel_height = 2160;
		m.screen_left = i * 3840;
		std::snprintf(m.connector, sizeof(m.connector), "%s", i == 0 ? "card1-HDMI-A-1" : "card1-DP-1");
	}
	xrt_display_descriptor a[2]{};
	REQUIRE(target_plugin_build_descriptors(&edid, a, 2) == 2);
	CHECK(a[0].monitor_id != a[1].monitor_id); // identical panels, different ports

	// The user swaps the two monitors' desktop positions.
	edid.monitors[0].screen_left = 3840;
	edid.monitors[1].screen_left = 0;
	xrt_display_descriptor b[2]{};
	REQUIRE(target_plugin_build_descriptors(&edid, b, 2) == 2);
	CHECK(b[0].monitor_id == a[0].monitor_id);
	CHECK(b[1].monitor_id == a[1].monitor_id);

	// A different panel on the same port is a different display.
	edid.monitors[0].serial_number = 0x1;
	xrt_display_descriptor c[2]{};
	REQUIRE(target_plugin_build_descriptors(&edid, c, 2) == 2);
	CHECK(c[0].monitor_id != a[0].monitor_id);

	// Same connector name on a second GPU: the card prefix keeps them apart.
	edid.monitors[0].serial_number = edid.monitors[1].serial_number;
	std::snprintf(edid.monitors[0].connector, sizeof(edid.monitors[0].connector), "%s", "card0-DP-1");
	xrt_display_descriptor e[2]{};
	REQUIRE(target_plugin_build_descriptors(&edid, e, 2) == 2);
	CHECK(e[0].monitor_id != e[1].monitor_id);

	// No connector name (Windows): position still separates identical panels.
	edid.monitors[0].connector[0] = '\0';
	edid.monitors[1].connector[0] = '\0';
	edid.monitors[0].serial_number = edid.monitors[1].serial_number;
	xrt_display_descriptor d[2]{};
	REQUIRE(target_plugin_build_descriptors(&edid, d, 2) == 2);
	CHECK(d[0].monitor_id != d[1].monitor_id);
}

/*
 * ADR-051 D6: a mid-reseat rebuild sees the output still listed at its rect
 * but its EDID unreadable. It must keep the identity (monitor id + screen key)
 * the previous build read there, not publish a removal plus an EDID-less
 * "new" screen and then the reverse.
 */
namespace {

void
reseat_monitor(os_display_edid_monitor &m, const char *output, const char *connector, int32_t left)
{
	std::memset(&m, 0, sizeof(m));
	m.manufacturer_id = 0x0472;
	m.product_id = 0x0001;
	m.serial_number = 0x322EF05E;
	m.screen_left = left;
	m.pixel_width = 3840;
	m.pixel_height = 2160;
	m.refresh_hz = 60;
	m.physical_width_mm = 344;
	m.physical_height_mm = 193;
	m.native_width = 3840;
	m.native_height = 2160;
	m.join = OS_EDID_JOIN_NAME;
	std::snprintf(m.output_name, sizeof(m.output_name), "%s", output);
	std::snprintf(m.connector, sizeof(m.connector), "%s", connector);
}

//! The EDID half of a record gone, as mid-reseat: only the placement is left.
void
lose_edid(os_display_edid_monitor &m)
{
	m.manufacturer_id = 0;
	m.product_id = 0;
	m.serial_number = 0;
	m.physical_width_mm = 0;
	m.physical_height_mm = 0;
	m.connector[0] = '\0';
	m.join = OS_EDID_JOIN_NONE;
}

std::string
key_of(uint64_t monitor_id)
{
	char key[TARGET_SCREEN_KEY_MAX] = {};
	REQUIRE(target_plugin_get_monitor_key(monitor_id, key, sizeof(key), nullptr, 0));
	return key;
}

} // namespace

TEST_CASE("EDID unreadable on a known output and rect keeps the monitor id and screen key")
{
	os_display_edid_list edid{};
	edid.count = 1;
	reseat_monitor(edid.monitors[0], "HDMI-7", "card1-HDMI-A-7", 3456);
	xrt_display_descriptor a[1]{};
	REQUIRE(target_plugin_build_descriptors(&edid, a, 1) == 1);
	const std::string key_a = key_of(a[0].monitor_id);
	CHECK(key_a.rfind("???", 0) != 0);

	lose_edid(edid.monitors[0]);
	xrt_display_descriptor b[1]{};
	REQUIRE(target_plugin_build_descriptors(&edid, b, 1) == 1);
	CHECK(b[0].monitor_id == a[0].monitor_id);
	CHECK(b[0].edid_manufacturer == a[0].edid_manufacturer);
	CHECK(key_of(b[0].monitor_id) == key_a);
	os_display_edid_monitor rec{};
	REQUIRE(target_plugin_get_monitor_record(b[0].monitor_id, nullptr, &rec));
	CHECK(rec.physical_width_mm == 344);

	// Still unreadable on the next rebuild (carried from a carried record).
	xrt_display_descriptor c[1]{};
	REQUIRE(target_plugin_build_descriptors(&edid, c, 1) == 1);
	CHECK(c[0].monitor_id == a[0].monitor_id);

	// The real EDID is back: same identity, nothing moved.
	reseat_monitor(edid.monitors[0], "HDMI-7", "card1-HDMI-A-7", 3456);
	xrt_display_descriptor d[1]{};
	REQUIRE(target_plugin_build_descriptors(&edid, d, 1) == 1);
	CHECK(d[0].monitor_id == a[0].monitor_id);
	CHECK(key_of(d[0].monitor_id) == key_a);
}

TEST_CASE("EDID unreadable on an output never seen before gets the ??? key")
{
	os_display_edid_list edid{};
	edid.count = 1;
	reseat_monitor(edid.monitors[0], "DP-8", "card1-DP-8", 7000);
	lose_edid(edid.monitors[0]);
	xrt_display_descriptor a[1]{};
	REQUIRE(target_plugin_build_descriptors(&edid, a, 1) == 1);
	CHECK(a[0].edid_manufacturer == 0);
	CHECK(key_of(a[0].monitor_id) == "???-0000-00000000@DP-8");

	// A known output whose rect moved is not the same monitor either.
	reseat_monitor(edid.monitors[0], "DP-8", "card1-DP-8", 7000);
	xrt_display_descriptor b[1]{};
	REQUIRE(target_plugin_build_descriptors(&edid, b, 1) == 1);
	lose_edid(edid.monitors[0]);
	// Not 0: on Windows the key's device suffix is the GDI name of whatever
	// real monitor sits at the origin, and CI's runner has one at (0,0).
	edid.monitors[0].screen_left = 9000;
	xrt_display_descriptor c[1]{};
	REQUIRE(target_plugin_build_descriptors(&edid, c, 1) == 1);
	CHECK(c[0].monitor_id != b[0].monitor_id);
	CHECK(key_of(c[0].monitor_id) == "???-0000-00000000@DP-8");
}

TEST_CASE("a different real EDID on the same output is a real replacement")
{
	os_display_edid_list edid{};
	edid.count = 1;
	reseat_monitor(edid.monitors[0], "HDMI-6", "card1-HDMI-A-6", 3456);
	xrt_display_descriptor a[1]{};
	REQUIRE(target_plugin_build_descriptors(&edid, a, 1) == 1);
	const std::string key_a = key_of(a[0].monitor_id);

	edid.monitors[0].manufacturer_id = 0x4C2D;
	edid.monitors[0].product_id = 0x7155;
	edid.monitors[0].serial_number = 0x1;
	xrt_display_descriptor b[1]{};
	REQUIRE(target_plugin_build_descriptors(&edid, b, 1) == 1);
	CHECK(b[0].monitor_id != a[0].monitor_id);
	CHECK(key_of(b[0].monitor_id) != key_a);
}

TEST_CASE("EDID carry: the pure rule, including the window")
{
	os_display_edid_monitor prev{};
	reseat_monitor(prev, "HDMI-5", "card1-HDMI-A-5", 0);
	const uint64_t t0 = 1000;

	os_display_edid_monitor cur = prev;
	lose_edid(cur);
	CHECK(target_plugin_edid_carry(&prev, t0, &cur, t0 + TARGET_EDID_CARRY_WINDOW_NS) == TARGET_EDID_CARRY_KEPT);
	CHECK(cur.manufacturer_id == prev.manufacturer_id);
	CHECK(cur.serial_number == prev.serial_number);
	CHECK(std::strcmp(cur.connector, prev.connector) == 0);
	CHECK(cur.physical_width_mm == prev.physical_width_mm);

	cur = prev;
	lose_edid(cur);
	CHECK(target_plugin_edid_carry(&prev, t0, &cur, t0 + TARGET_EDID_CARRY_WINDOW_NS + 1) ==
	      TARGET_EDID_CARRY_EXPIRED);
	CHECK(cur.manufacturer_id == 0); // nothing copied

	// Matched by the connector alone when the output name is gone too.
	cur = prev;
	cur.manufacturer_id = 0;
	cur.product_id = 0;
	cur.serial_number = 0;
	cur.output_name[0] = '\0';
	CHECK(target_plugin_edid_carry(&prev, t0, &cur, t0) == TARGET_EDID_CARRY_KEPT);

	// Another output, a moved / resized rect, a real EDID, or a previous
	// record without one: no carry.
	cur = prev;
	lose_edid(cur);
	std::snprintf(cur.output_name, sizeof(cur.output_name), "%s", "HDMI-4");
	CHECK(target_plugin_edid_carry(&prev, t0, &cur, t0) == TARGET_EDID_CARRY_NONE);
	cur = prev;
	lose_edid(cur);
	cur.pixel_width = 1920;
	CHECK(target_plugin_edid_carry(&prev, t0, &cur, t0) == TARGET_EDID_CARRY_NONE);
	cur = prev;
	CHECK(target_plugin_edid_carry(&prev, t0, &cur, t0) == TARGET_EDID_CARRY_NONE);
	os_display_edid_monitor blank = prev;
	lose_edid(blank);
	cur = blank;
	CHECK(target_plugin_edid_carry(&blank, t0, &cur, t0) == TARGET_EDID_CARRY_NONE);
}

/*
 * The claim rule's input (displayxr-leia-plugin#430): which registry entries
 * may name the panel's monitor. Never an unclaimed, synthesized, fallback,
 * not-ready or position-less one.
 */
namespace {

uint32_t
fake_probe_displays(
    struct xrt_plugin_instance *, const struct xrt_display_descriptor *, uint32_t, struct xrt_display_claim *, uint32_t)
{
	return 0;
}

} // namespace

TEST_CASE("panel claims: only the active plug-in's own probe_displays claims, at EDID or above")
{
	fixture f;
	f.vendor.probe_displays = fake_probe_displays;
	f.reg.entries[2].confidence = (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED;

	os_display_panel_claim claims[8] = {};
	os_display_edid_monitor mons[8] = {};
	REQUIRE(target_screens_collect_panel_claims(&f.reg, "vendor", claims, mons, 8) == 2);
	CHECK(claims[0].left == 2880);
	CHECK(claims[0].width == 3840);
	CHECK_FALSE(claims[0].verified);
	CHECK_FALSE(claims[0].is_primary);
	CHECK(std::strcmp(mons[0].output_name, "HDMI-1") == 0);
	CHECK(claims[1].left == 6720);
	CHECK(claims[1].verified);
	CHECK(std::strcmp(mons[1].output_name, "DP-1") == 0);

	// A capacity of one keeps the first.
	CHECK(target_screens_collect_panel_claims(&f.reg, "vendor", claims, nullptr, 1) == 1);
}

TEST_CASE("panel claims: a FALLBACK claim (sim-display's 'anything unclaimed') is never one")
{
	fixture f;
	f.fallback.probe_displays = fake_probe_displays;
	os_display_panel_claim claims[8] = {};
	CHECK(target_screens_collect_panel_claims(&f.reg, "fallback", claims, nullptr, 8) == 0);
}

TEST_CASE("panel claims: the loader's SYNTHESIZED claim (no probe_displays) is evidence of nothing")
{
	fixture f; // vendor has no probe_displays
	os_display_panel_claim claims[8] = {};
	CHECK(target_screens_collect_panel_claims(&f.reg, "vendor", claims, nullptr, 8) == 0);
}

TEST_CASE("panel claims: other plug-ins' and not-ready entries are skipped")
{
	fixture f;
	f.vendor.probe_displays = fake_probe_displays;
	f.reg.entries[1].not_ready = true;
	os_display_panel_claim claims[8] = {};
	REQUIRE(target_screens_collect_panel_claims(&f.reg, "vendor", claims, nullptr, 8) == 1);
	CHECK(claims[0].left == 6720);
	CHECK(target_screens_collect_panel_claims(&f.reg, "someone-else", claims, nullptr, 8) == 0);
	CHECK(target_screens_collect_panel_claims(&f.reg, "", claims, nullptr, 8) == 0);
	CHECK(target_screens_collect_panel_claims(nullptr, "vendor", claims, nullptr, 8) == 0);
}

TEST_CASE("panel claims: a monitor with no desktop position (DRM-only record) is never one")
{
	fixture f;
	f.vendor.probe_displays = fake_probe_displays;

	// Re-enumerate with the third monitor position-less, as a pure-Wayland
	// DRM-only record is.
	static os_display_edid_list edid;
	std::memset(&edid, 0, sizeof(edid));
	edid.count = 3;
	for (uint32_t i = 0; i < 3; i++) {
		REQUIRE(target_plugin_get_monitor_record(f.descs[i].monitor_id, nullptr, &edid.monitors[i]));
	}
	edid.monitors[2].origin_unknown = true;
	xrt_display_descriptor descs[3]{};
	REQUIRE(target_plugin_build_descriptors(&edid, descs, 3) == 3);
	for (int i = 0; i < 3; i++) {
		f.reg.entries[i].monitor_id = descs[i].monitor_id;
	}

	os_display_panel_claim claims[8] = {};
	REQUIRE(target_screens_collect_panel_claims(&f.reg, "vendor", claims, nullptr, 8) == 1);
	CHECK(claims[0].left == 2880);
}

TEST_CASE("panel from claims: a claim the OS cannot resolve by identity never fabricates a rect")
{
	// The fixture's output names do not exist on the test host, and there
	// is no HMONITOR: the claim rule must decline (the caller then runs the
	// origin / size / primary rules), not hand back the claim's coordinates.
	fixture f;
	f.vendor.probe_displays = fake_probe_displays;
	os_display_panel_hint h = {};
	h.pixel_width = 3840;
	h.pixel_height = 2160;
	os_display_desktop_info d = {};
	bool overrides = true;
	CHECK_FALSE(target_screens_panel_from_claims(&f.reg, "vendor", &h, &d, &overrides));
	CHECK_FALSE(overrides);
	CHECK(d.width == 0);
}
