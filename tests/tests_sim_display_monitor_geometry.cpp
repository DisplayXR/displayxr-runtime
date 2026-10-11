// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pins which real monitor the simulated panel adopts on desktop Linux
 *         (the #369 fix's twin): device mode, not the scaled desktop size;
 *         EDID millimetres; primary, else the internal panel, else the first.
 *         Fixtures model the DS1 box: laptop eDP-1 2880x1800 (300x190 mm) at
 *         167 % + the Acer DS1 on HDMI-A-1.
 */

#include "catch_amalgamated.hpp"

#include "os/os_display_edid_linux.h"
#include "sim_display_monitor_geometry.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

//! Acer DS1 EDID base block (HDMI-A-1): ACR 0x0001, 34x19 cm, DTD 3840x2160 at 344x193 mm.
const uint8_t DS1_EDID[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x04, 0x72, 0x01, 0x00, 0x5e, 0xf0, 0x2e, 0x32, 0x16, 0x21, 0x01,
    0x03, 0x80, 0x22, 0x13, 0x78, 0x2a, 0xce, 0xb5, 0xa6, 0x54, 0x34, 0xb6, 0x25, 0x0e, 0x50, 0x54, 0xbd, 0xef, 0x00,
    0x71, 0x4f, 0x81, 0x40, 0x81, 0x80, 0x81, 0xc0, 0x95, 0x00, 0xb3, 0x00, 0xd1, 0xc0, 0x01, 0x01, 0x08, 0xe8, 0x00,
    0x30, 0xf2, 0x70, 0x5a, 0x80, 0xb0, 0x58, 0x8a, 0x00, 0x58, 0xc1, 0x10, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfe,
    0x00, 0x46, 0x46, 0x52, 0x32, 0x41, 0x41, 0x41, 0x30, 0x30, 0x31, 0x0a, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00,
    0x44, 0x53, 0x31, 0x5f, 0x31, 0x35, 0x36, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x17,
    0x4c, 0x1e, 0x87, 0x3c, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x71,
};

//! A laptop eDP EDID like this box's: SDC, no serial, 30x19 cm, no detailed timing.
std::vector<uint8_t>
edp_edid()
{
	std::vector<uint8_t> e(128, 0);
	const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
	std::memcpy(e.data(), header, 8);
	e[8] = 0x4c;
	e[9] = 0x83;
	e[10] = 0x3f;
	e[11] = 0x42;
	e[18] = 1;
	e[19] = 4;
	e[21] = 30;
	e[22] = 19;
	e[54 + 3] = 0xfd;
	return e;
}

void
write_file(const std::string &path, const void *data, size_t len)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	REQUIRE(f != nullptr);
	if (len > 0) {
		REQUIRE(std::fwrite(data, 1, len, f) == len);
	}
	std::fclose(f);
}

void
write_text(const std::string &path, const char *text)
{
	write_file(path, text, std::strlen(text));
}

/*!
 * A temp-dir `/sys/class/drm` holding the DS1 box's two connectors, removed on
 * destruction.
 */
struct FakeDrmSysfs
{
	std::string root, edp, hdmi;

	FakeDrmSysfs()
	{
		char tmpl[] = "/tmp/dxr_sim_geo_drm_XXXXXX";
		REQUIRE(mkdtemp(tmpl) != nullptr);
		root = tmpl;
		edp = root + "/card1-eDP-1";
		hdmi = root + "/card1-HDMI-A-1";
		REQUIRE(mkdir(edp.c_str(), 0700) == 0);
		REQUIRE(mkdir(hdmi.c_str(), 0700) == 0);

		const std::vector<uint8_t> e = edp_edid();
		write_text(edp + "/status", "connected\n");
		write_text(edp + "/enabled", "enabled\n");
		write_file(edp + "/edid", e.data(), e.size());
		write_text(edp + "/modes", "2880x1800\n1920x1200\n");

		write_text(hdmi + "/status", "connected\n");
		write_text(hdmi + "/enabled", "enabled\n");
		write_file(hdmi + "/edid", DS1_EDID, sizeof(DS1_EDID));
		write_text(hdmi + "/modes", "3840x2160\n1920x1080\n");
	}

	~FakeDrmSysfs()
	{
		for (const std::string &d : {edp, hdmi}) {
			for (const char *f : {"/status", "/enabled", "/edid", "/modes"}) {
				(void)unlink((d + f).c_str());
			}
			(void)rmdir(d.c_str());
		}
		(void)rmdir(root.c_str());
	}

	uint32_t
	read(os_display_drm_connector *out, uint32_t max) const
	{
		return os_display_drm_read_connectors(root.c_str(), out, max);
	}
};

os_display_desktop_info
randr_mon(const char *name, int32_t left, uint32_t w, uint32_t h, uint32_t nw, uint32_t nh, bool primary)
{
	os_display_desktop_info m = {};
	m.left = left;
	m.width = w;
	m.height = h;
	m.width_in_caller_dpi = w;
	m.height_in_caller_dpi = h;
	m.is_primary = primary;
	m.native_width = nw;
	m.native_height = nh;
	m.native_source = OS_DISPLAY_NATIVE_SOURCE_COMPOSITOR;
	std::snprintf(m.device_name, sizeof(m.device_name), "%s", name);
	return m;
}

//! Join @p randr (may be empty) against the fake sysfs, as os_display_edid_enumerate does.
os_display_edid_list
enumerate(const FakeDrmSysfs &fs, const std::vector<os_display_desktop_info> &randr)
{
	os_display_drm_connector drm[OS_DISPLAY_DRM_MAX_CONNECTORS] = {};
	const uint32_t dn = fs.read(drm, OS_DISPLAY_DRM_MAX_CONNECTORS);
	REQUIRE(dn == 2);
	os_display_edid_list list = {};
	list.count = os_display_edid_linux_join(randr.empty() ? nullptr : randr.data(), nullptr, (uint32_t)randr.size(),
	                                        drm, dn, list.monitors, OS_DISPLAY_EDID_MAX_MONITORS);
	return list;
}

} // namespace

TEST_CASE("sim geometry: the DS1 box under XWayland adopts eDP-1's device mode, not the 167% desktop")
{
	FakeDrmSysfs fs;
	// RandR under XWayland at 167 %: the laptop's logical rect is 3456x2160,
	// Mutter reports its current mode 2880x1800. DS1 to its right, unscaled.
	const std::vector<os_display_desktop_info> randr = {
	    randr_mon("eDP-1", 0, 3456, 2160, 2880, 1800, true),
	    randr_mon("HDMI-1", 3456, 3840, 2160, 3840, 2160, false),
	};
	const os_display_edid_list list = enumerate(fs, randr);
	REQUIRE(list.count == 2);

	sim_display_monitor_geometry g = {};
	REQUIRE(sim_display_pick_monitor_geometry(&list, &g));
	CHECK(g.index == 0);
	CHECK(std::string(g.reason) == "primary");
	CHECK(g.pixel_width == 2880);
	CHECK(g.pixel_height == 1800);
	CHECK(g.pixel_is_native);
	CHECK(g.physical_width_mm == 300);
	CHECK(g.physical_height_mm == 190);
	CHECK(std::string(list.monitors[g.index].connector) == "card1-eDP-1");
}

TEST_CASE("sim geometry: with no X server (DRM-only, nothing primary) the internal panel wins")
{
	FakeDrmSysfs fs;
	const os_display_edid_list list = enumerate(fs, {});
	REQUIRE(list.count == 2);
	// Connector-name order puts HDMI first ("card1-HDMI-A-1" < "card1-eDP-1").
	REQUIRE(std::string(list.monitors[0].connector) == "card1-HDMI-A-1");

	sim_display_monitor_geometry g = {};
	REQUIRE(sim_display_pick_monitor_geometry(&list, &g));
	CHECK(g.index == 1);
	CHECK(std::string(g.reason) == "internal");
	CHECK(g.pixel_width == 2880);
	CHECK(g.pixel_height == 1800);
	CHECK(g.physical_width_mm == 300);
	CHECK(g.physical_height_mm == 190);
}

TEST_CASE("sim geometry: a primary DS1 is adopted with its own mode and EDID size")
{
	FakeDrmSysfs fs;
	const std::vector<os_display_desktop_info> randr = {
	    randr_mon("eDP-1", 0, 3456, 2160, 2880, 1800, false),
	    randr_mon("HDMI-1", 3456, 3840, 2160, 3840, 2160, true),
	};
	const os_display_edid_list list = enumerate(fs, randr);

	sim_display_monitor_geometry g = {};
	REQUIRE(sim_display_pick_monitor_geometry(&list, &g));
	CHECK(g.index == 1);
	CHECK(g.pixel_width == 3840);
	CHECK(g.pixel_height == 2160);
	CHECK(g.physical_width_mm == 344); // detailed-timing mm
	CHECK(g.physical_height_mm == 193);
}

TEST_CASE("sim geometry: falls back to the desktop mode, and skips monitors with no size")
{
	os_display_edid_list list = {};
	list.count = 2;
	// A primary with no pixels at all is not adoptable.
	list.monitors[0].is_primary = true;
	// No native mode, no mm: the desktop mode, physical size unknown.
	list.monitors[1].pixel_width = 2560;
	list.monitors[1].pixel_height = 1440;

	sim_display_monitor_geometry g = {};
	REQUIRE(sim_display_pick_monitor_geometry(&list, &g));
	CHECK(g.index == 1);
	CHECK(std::string(g.reason) == "first");
	CHECK(g.pixel_width == 2560);
	CHECK(g.pixel_height == 1440);
	CHECK_FALSE(g.pixel_is_native);
	CHECK(g.physical_width_mm == 0);
	CHECK(g.physical_height_mm == 0);
}

TEST_CASE("sim geometry: half-known physical size is reported as unknown")
{
	os_display_edid_list list = {};
	list.count = 1;
	list.monitors[0].pixel_width = 1920;
	list.monitors[0].pixel_height = 1080;
	list.monitors[0].physical_width_mm = 520;

	sim_display_monitor_geometry g = {};
	REQUIRE(sim_display_pick_monitor_geometry(&list, &g));
	CHECK(g.physical_width_mm == 0);
	CHECK(g.physical_height_mm == 0);
}

TEST_CASE("sim geometry: nothing to adopt keeps the defaults")
{
	os_display_edid_list list = {};
	sim_display_monitor_geometry g = {};
	CHECK_FALSE(sim_display_pick_monitor_geometry(&list, &g));
	CHECK_FALSE(sim_display_pick_monitor_geometry(nullptr, &g));
}

TEST_CASE("sim geometry: internal connector names")
{
	CHECK(sim_display_connector_is_internal("card1-eDP-1"));
	CHECK(sim_display_connector_is_internal("card0-LVDS-1"));
	CHECK(sim_display_connector_is_internal("card0-DSI-1"));
	CHECK(sim_display_connector_is_internal("eDP-1"));
	CHECK_FALSE(sim_display_connector_is_internal("card1-HDMI-A-1"));
	CHECK_FALSE(sim_display_connector_is_internal("card1-DP-1"));
	CHECK_FALSE(sim_display_connector_is_internal("HDMI-1"));
	CHECK_FALSE(sim_display_connector_is_internal(""));
	CHECK_FALSE(sim_display_connector_is_internal(nullptr));
}
