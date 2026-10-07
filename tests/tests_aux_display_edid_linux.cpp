// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pins the desktop-Linux EDID enumeration (multi-screen M0, #69): the
 *         EDID parser, the DRM sysfs reader and the RandR <-> DRM join, on
 *         fixtures modelled on the DS1 box (laptop eDP-1 + Acer DS1 on HDMI).
 */

#include "catch_amalgamated.hpp"

#include "os/os_display_edid_linux.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

/*
 * The Acer SpatialLabs DS1's EDID base block as the kernel exposes it on
 * HDMI-A-1 (`/sys/class/drm/card1-HDMI-A-1/edid`, first 128 bytes): vendor
 * ACR, product 0x0001, serial 0x322EF05E, 34x19 cm, first detailed timing
 * 3840x2160@60 at 344x193 mm.
 */
const uint8_t DS1_EDID[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x04, 0x72, 0x01, 0x00, 0x5e, 0xf0, 0x2e, 0x32, 0x16, 0x21, 0x01,
    0x03, 0x80, 0x22, 0x13, 0x78, 0x2a, 0xce, 0xb5, 0xa6, 0x54, 0x34, 0xb6, 0x25, 0x0e, 0x50, 0x54, 0xbd, 0xef, 0x00,
    0x71, 0x4f, 0x81, 0x40, 0x81, 0x80, 0x81, 0xc0, 0x95, 0x00, 0xb3, 0x00, 0xd1, 0xc0, 0x01, 0x01, 0x08, 0xe8, 0x00,
    0x30, 0xf2, 0x70, 0x5a, 0x80, 0xb0, 0x58, 0x8a, 0x00, 0x58, 0xc1, 0x10, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfe,
    0x00, 0x46, 0x46, 0x52, 0x32, 0x41, 0x41, 0x41, 0x30, 0x30, 0x31, 0x0a, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00,
    0x44, 0x53, 0x31, 0x5f, 0x31, 0x35, 0x36, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x17,
    0x4c, 0x1e, 0x87, 0x3c, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x71,
};

/*!
 * A laptop eDP panel in the shape this box's has: vendor SDC, no serial in
 * bytes 12-15, 30x19 cm, and NO detailed timing in the base block (the timing
 * lives in a DisplayID extension), so the cm values are all there is.
 */
std::vector<uint8_t>
edp_no_serial_edid()
{
	std::vector<uint8_t> e(128, 0);
	const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
	std::memcpy(e.data(), header, 8);
	e[8] = 0x4c; // "SDC"
	e[9] = 0x83;
	e[10] = 0x3f;
	e[11] = 0x42;
	// bytes 12-15: serial 0
	e[18] = 1;
	e[19] = 4;
	e[21] = 30; // cm
	e[22] = 19;
	// byte 54..71: first descriptor left as a display descriptor (clock 0)
	e[54 + 3] = 0xfd;
	return e;
}

constexpr uint16_t ACR_RAW = 0x7204; // bytes 08 72 loaded little-endian ("ACR")
constexpr uint16_t SDC_RAW = 0x834c;

os_display_desktop_info
randr_mon(const char *name, int32_t left, uint32_t w, uint32_t h, uint32_t mm_w, uint32_t mm_h, bool primary)
{
	os_display_desktop_info m = {};
	m.left = left;
	m.width = w;
	m.height = h;
	m.width_in_caller_dpi = w;
	m.height_in_caller_dpi = h;
	m.is_primary = primary;
	m.physical_width_mm = mm_w;
	m.physical_height_mm = mm_h;
	std::snprintf(m.device_name, sizeof(m.device_name), "%s", name);
	return m;
}

os_display_drm_connector
drm_conn(const char *name, const uint8_t *edid, size_t len, uint32_t mode_w, uint32_t mode_h, bool enabled = true)
{
	os_display_drm_connector c = {};
	std::snprintf(c.name, sizeof(c.name), "%s", name);
	c.enabled = enabled;
	if (edid != nullptr) {
		c.has_edid = os_display_edid_parse(edid, len, &c.edid);
	}
	c.mode_w[0] = mode_w;
	c.mode_h[0] = mode_h;
	c.mode_count = 1;
	return c;
}

//! The DS1 box's two monitors as RandR reports them under XWayland.
void
box_randr(os_display_desktop_info out[2], const char *n0 = "eDP-1", const char *n1 = "HDMI-1")
{
	out[0] = randr_mon(n0, 0, 3456, 2160, 300, 190, true);
	out[0].native_width = 2880;
	out[0].native_height = 1800;
	out[0].native_source = OS_DISPLAY_NATIVE_SOURCE_COMPOSITOR;
	out[1] = randr_mon(n1, 3456, 3840, 2160, 340, 190, false);
	out[1].native_width = 3840;
	out[1].native_height = 2160;
	out[1].native_source = OS_DISPLAY_NATIVE_SOURCE_COMPOSITOR;
}

void
box_drm(os_display_drm_connector out[2])
{
	const std::vector<uint8_t> edp = edp_no_serial_edid();
	out[0] = drm_conn("card1-eDP-1", edp.data(), edp.size(), 2880, 1800);
	out[1] = drm_conn("card1-HDMI-A-1", DS1_EDID, sizeof(DS1_EDID), 3840, 2160);
}

} // namespace

TEST_CASE("edid: parses the DS1 base block")
{
	os_display_edid_parsed p = {};
	REQUIRE(os_display_edid_parse(DS1_EDID, sizeof(DS1_EDID), &p));
	CHECK(p.manufacturer_id == ACR_RAW);
	CHECK(p.product_id == 0x0001);
	CHECK(p.serial_number == 0x322EF05Eu);
	CHECK(p.cm_width_mm == 340);
	CHECK(p.cm_height_mm == 190);
	CHECK(p.dtd_width_px == 3840);
	CHECK(p.dtd_height_px == 2160);
	CHECK(p.dtd_width_mm == 344);
	CHECK(p.dtd_height_mm == 193);
	CHECK(p.dtd_refresh_mhz == 60000);

	uint32_t w = 0, h = 0;
	os_display_edid_parsed_mm(&p, &w, &h);
	CHECK(w == 344); // the detailed timing's mm beat the base block's cm
	CHECK(h == 193);

	// The PNP letters, decoded the way the CLI does.
	const uint16_t v = (uint16_t)((p.manufacturer_id >> 8) | (p.manufacturer_id << 8));
	CHECK((char)(((v >> 10) & 0x1F) + 'A' - 1) == 'A');
	CHECK((char)(((v >> 5) & 0x1F) + 'A' - 1) == 'C');
	CHECK((char)((v & 0x1F) + 'A' - 1) == 'R');
}

TEST_CASE("edid: an eDP with no serial and no detailed timing falls back to cm")
{
	const std::vector<uint8_t> e = edp_no_serial_edid();
	os_display_edid_parsed p = {};
	REQUIRE(os_display_edid_parse(e.data(), e.size(), &p));
	CHECK(p.manufacturer_id == SDC_RAW);
	CHECK(p.product_id == 0x423f);
	CHECK(p.serial_number == 0);
	CHECK(p.dtd_width_px == 0);
	CHECK(p.dtd_refresh_mhz == 0);
	uint32_t w = 0, h = 0;
	os_display_edid_parsed_mm(&p, &w, &h);
	CHECK(w == 300);
	CHECK(h == 190);
}

TEST_CASE("edid: aspect-ratio encoding in bytes 21/22 is not a size")
{
	std::vector<uint8_t> e = edp_no_serial_edid();
	e[21] = 0x4f; // landscape aspect ratio, EDID 1.4
	e[22] = 0;
	os_display_edid_parsed p = {};
	REQUIRE(os_display_edid_parse(e.data(), e.size(), &p));
	CHECK(p.cm_width_mm == 0);
	CHECK(p.cm_height_mm == 0);
}

TEST_CASE("edid: rejects short and headerless blobs")
{
	os_display_edid_parsed p = {};
	CHECK_FALSE(os_display_edid_parse(DS1_EDID, 127, &p));
	CHECK_FALSE(os_display_edid_parse(nullptr, 128, &p));
	std::vector<uint8_t> bad(DS1_EDID, DS1_EDID + 128);
	bad[0] = 0x01;
	CHECK_FALSE(os_display_edid_parse(bad.data(), bad.size(), &p));
	CHECK(p.manufacturer_id == 0);
}

TEST_CASE("join: RandR output names tie to DRM connectors by name")
{
	os_display_desktop_info randr[2];
	box_randr(randr);
	os_display_drm_connector drm[2];
	box_drm(drm);

	os_display_edid_monitor out[4] = {};
	REQUIRE(os_display_edid_linux_join(randr, nullptr, 2, drm, 2, out, 4) == 2);

	CHECK(out[0].join == OS_EDID_JOIN_NAME);
	CHECK(out[0].manufacturer_id == SDC_RAW);
	CHECK(out[0].serial_number == 0);
	CHECK(out[0].is_primary);
	CHECK(std::string(out[0].connector) == "card1-eDP-1");
	CHECK(out[0].physical_width_mm == 300);
	CHECK(out[0].native_width == 2880);
	CHECK(out[0].refresh_hz == 0); // no detailed timing to read it from

	CHECK(out[1].join == OS_EDID_JOIN_NAME);
	CHECK(out[1].manufacturer_id == ACR_RAW);
	CHECK(out[1].product_id == 0x0001);
	CHECK(out[1].serial_number == 0x322EF05Eu);
	CHECK(std::string(out[1].connector) == "card1-HDMI-A-1");
	CHECK(std::string(out[1].output_name) == "HDMI-1");
	CHECK(out[1].screen_left == 3456);
	CHECK(out[1].pixel_width == 3840);
	CHECK(out[1].physical_width_mm == 344);
	CHECK(out[1].physical_height_mm == 193);
	CHECK(out[1].refresh_hz == 60);
	CHECK_FALSE(out[1].is_primary);
	CHECK_FALSE(out[1].origin_unknown);
}

TEST_CASE("join: unrelated output names fall back to physical size")
{
	os_display_desktop_info randr[2];
	box_randr(randr, "XWAYLAND0", "XWAYLAND1");
	os_display_drm_connector drm[2];
	box_drm(drm);

	os_display_edid_monitor out[2] = {};
	REQUIRE(os_display_edid_linux_join(randr, nullptr, 2, drm, 2, out, 2) == 2);
	CHECK(out[0].join == OS_EDID_JOIN_MM);
	CHECK(out[0].manufacturer_id == SDC_RAW);
	CHECK(out[1].join == OS_EDID_JOIN_MM); // RandR 340x190 vs DTD 344x193: within tolerance
	CHECK(out[1].manufacturer_id == ACR_RAW);
}

TEST_CASE("join: no names and no mm fall back to the pixel mode")
{
	os_display_desktop_info randr[2];
	box_randr(randr, "XWAYLAND0", "XWAYLAND1");
	for (auto &m : randr) {
		m.physical_width_mm = 0;
		m.physical_height_mm = 0;
	}
	os_display_drm_connector drm[2];
	box_drm(drm);

	os_display_edid_monitor out[2] = {};
	REQUIRE(os_display_edid_linux_join(randr, nullptr, 2, drm, 2, out, 2) == 2);
	CHECK(out[0].join == OS_EDID_JOIN_MODE); // device mode 2880x1800
	CHECK(out[0].manufacturer_id == SDC_RAW);
	CHECK(out[1].join == OS_EDID_JOIN_MODE);
	CHECK(out[1].manufacturer_id == ACR_RAW);
}

TEST_CASE("join: two identical panels with no name match stay unjoined, never guessed")
{
	os_display_desktop_info randr[2] = {
	    randr_mon("XWAYLAND0", 0, 3840, 2160, 340, 190, true),
	    randr_mon("XWAYLAND1", 3840, 3840, 2160, 340, 190, false),
	};
	os_display_drm_connector drm[2] = {
	    drm_conn("card1-HDMI-A-1", DS1_EDID, sizeof(DS1_EDID), 3840, 2160),
	    drm_conn("card1-HDMI-A-2", DS1_EDID, sizeof(DS1_EDID), 3840, 2160),
	};

	os_display_edid_monitor out[2] = {};
	REQUIRE(os_display_edid_linux_join(randr, nullptr, 2, drm, 2, out, 2) == 2);
	CHECK(out[0].join == OS_EDID_JOIN_NONE);
	CHECK(out[0].manufacturer_id == 0);
	CHECK(out[1].join == OS_EDID_JOIN_NONE);
	CHECK(out[0].pixel_width == 3840); // still listed, with its placement
}

TEST_CASE("join: a connector is used at most once")
{
	// Both RandR monitors would match HDMI-A-1 by mm; the name pass takes it
	// first, so the second must not reuse it.
	os_display_desktop_info randr[2] = {
	    randr_mon("HDMI-1", 0, 3840, 2160, 340, 190, true),
	    randr_mon("XWAYLAND1", 3840, 3840, 2160, 340, 190, false),
	};
	os_display_drm_connector drm[1] = {drm_conn("card1-HDMI-A-1", DS1_EDID, sizeof(DS1_EDID), 3840, 2160)};

	os_display_edid_monitor out[2] = {};
	REQUIRE(os_display_edid_linux_join(randr, nullptr, 2, drm, 1, out, 2) == 2);
	CHECK(out[0].join == OS_EDID_JOIN_NAME);
	CHECK(out[1].join == OS_EDID_JOIN_NONE);
}

TEST_CASE("join: no X server gives DRM-only records with an unknown origin")
{
	os_display_drm_connector drm[3];
	box_drm(drm);
	drm[2] = drm_conn("card1-DP-1", DS1_EDID, sizeof(DS1_EDID), 1920, 1080, /*enabled=*/false);

	os_display_edid_monitor out[4] = {};
	REQUIRE(os_display_edid_linux_join(nullptr, nullptr, 0, drm, 3, out, 4) == 2); // disabled DP-1 skipped
	for (int i = 0; i < 2; i++) {
		CHECK(out[i].join == OS_EDID_JOIN_DRM_ONLY);
		CHECK(out[i].origin_unknown);
		CHECK_FALSE(out[i].is_primary);
		CHECK(out[i].screen_left == 0);
		CHECK(out[i].output_name[0] == '\0');
	}
	CHECK(out[1].manufacturer_id == ACR_RAW);
	CHECK(out[1].pixel_width == 3840);
	CHECK(out[1].native_width == 3840);
	CHECK(std::string(out[1].connector) == "card1-HDMI-A-1");
}

TEST_CASE("join: NVIDIA's off-by-one output names do not tie to the wrong connector")
{
	// The NVIDIA X driver names outputs from 0, nvidia-drm from 1. RandR DP-1
	// is really the panel on card0-DP-2; card0-DP-1 is a different monitor.
	const std::vector<uint8_t> edp = edp_no_serial_edid();
	os_display_desktop_info randr[2] = {
	    randr_mon("DP-0", 0, 2880, 1800, 300, 190, true),
	    randr_mon("DP-1", 2880, 3840, 2160, 340, 190, false),
	};
	os_display_drm_connector drm[2] = {
	    drm_conn("card0-DP-1", edp.data(), edp.size(), 2880, 1800),
	    drm_conn("card0-DP-2", DS1_EDID, sizeof(DS1_EDID), 3840, 2160),
	};

	os_display_edid_monitor out[2] = {};
	REQUIRE(os_display_edid_linux_join(randr, nullptr, 2, drm, 2, out, 2) == 2);
	// RandR DP-1 must NOT take card0-DP-1 (wrong mode, wrong mm): it falls
	// through to the mm rule and finds the DS1 on card0-DP-2.
	CHECK(out[1].manufacturer_id == ACR_RAW);
	CHECK(std::string(out[1].connector) == "card0-DP-2");
	CHECK(out[1].join == OS_EDID_JOIN_MM);
	CHECK(out[0].manufacturer_id == SDC_RAW);
	CHECK(std::string(out[0].connector) == "card0-DP-1");
}

TEST_CASE("join: the same connector name on two cards picks the one that agrees physically")
{
	const std::vector<uint8_t> edp = edp_no_serial_edid();
	os_display_desktop_info randr[1] = {randr_mon("HDMI-1", 0, 3840, 2160, 340, 190, true)};
	os_display_drm_connector drm[2] = {
	    drm_conn("card0-HDMI-A-1", edp.data(), edp.size(), 2880, 1800),
	    drm_conn("card1-HDMI-A-1", DS1_EDID, sizeof(DS1_EDID), 3840, 2160),
	};

	os_display_edid_monitor out[1] = {};
	REQUIRE(os_display_edid_linux_join(randr, nullptr, 1, drm, 2, out, 1) == 1);
	CHECK(out[0].join == OS_EDID_JOIN_NAME);
	CHECK(std::string(out[0].connector) == "card1-HDMI-A-1");
	CHECK(out[0].manufacturer_id == ACR_RAW);
}

TEST_CASE("join: a connected but disabled connector never joins by mm or mode")
{
	os_display_desktop_info randr[1] = {randr_mon("XWAYLAND0", 0, 3840, 2160, 340, 190, true)};
	os_display_drm_connector drm[2] = {
	    drm_conn("card1-DP-1", DS1_EDID, sizeof(DS1_EDID), 3840, 2160, /*enabled=*/false),
	    drm_conn("card1-HDMI-A-1", DS1_EDID, sizeof(DS1_EDID), 3840, 2160),
	};

	os_display_edid_monitor out[1] = {};
	REQUIRE(os_display_edid_linux_join(randr, nullptr, 1, drm, 2, out, 1) == 1);
	// Two identical EDIDs, but only one is lit: unambiguous.
	CHECK(out[0].join == OS_EDID_JOIN_MM);
	CHECK(std::string(out[0].connector) == "card1-HDMI-A-1");

	// With the enabled one gone, the disabled one is still not used.
	REQUIRE(os_display_edid_linux_join(randr, nullptr, 1, drm, 1, out, 1) == 1);
	CHECK(out[0].join == OS_EDID_JOIN_NONE);
	CHECK(out[0].connector[0] == '\0');
}

TEST_CASE("join: the X server's own EDID property is the identity when published")
{
	// Native X: names that tie nothing ("default") and no mm, but the server
	// publishes each output's EDID.
	const std::vector<uint8_t> edp = edp_no_serial_edid();
	os_display_desktop_info randr[2] = {
	    randr_mon("default0", 0, 2880, 1800, 0, 0, true),
	    randr_mon("default1", 2880, 3840, 2160, 0, 0, false),
	};
	os_display_randr_identity ids[2] = {};
	REQUIRE(os_display_edid_parse(edp.data(), edp.size(), &ids[0].edid));
	ids[0].valid = true;
	REQUIRE(os_display_edid_parse(DS1_EDID, sizeof(DS1_EDID), &ids[1].edid));
	ids[1].valid = true;
	os_display_drm_connector drm[2] = {
	    drm_conn("card0-DP-1", DS1_EDID, sizeof(DS1_EDID), 3840, 2160),
	    drm_conn("card0-eDP-1", edp.data(), edp.size(), 2880, 1800),
	};

	os_display_edid_monitor out[2] = {};
	REQUIRE(os_display_edid_linux_join(randr, ids, 2, drm, 2, out, 2) == 2);
	CHECK(out[0].join == OS_EDID_JOIN_RANDR_EDID);
	CHECK(out[0].manufacturer_id == SDC_RAW);
	CHECK(std::string(out[0].connector) == "card0-eDP-1");
	CHECK(out[1].join == OS_EDID_JOIN_RANDR_EDID);
	CHECK(out[1].serial_number == 0x322EF05Eu);
	CHECK(std::string(out[1].connector) == "card0-DP-1");
	CHECK(out[1].physical_width_mm == 344);
}

TEST_CASE("join: X server EDID with two identical connectors uses the name to choose")
{
	os_display_desktop_info randr[1] = {randr_mon("HDMI-2", 0, 3840, 2160, 340, 190, true)};
	os_display_randr_identity ids[1] = {};
	REQUIRE(os_display_edid_parse(DS1_EDID, sizeof(DS1_EDID), &ids[0].edid));
	ids[0].valid = true;
	os_display_drm_connector drm[2] = {
	    drm_conn("card1-HDMI-A-1", DS1_EDID, sizeof(DS1_EDID), 3840, 2160),
	    drm_conn("card1-HDMI-A-2", DS1_EDID, sizeof(DS1_EDID), 3840, 2160),
	};

	os_display_edid_monitor out[1] = {};
	REQUIRE(os_display_edid_linux_join(randr, ids, 1, drm, 2, out, 1) == 1);
	CHECK(out[0].join == OS_EDID_JOIN_RANDR_EDID);
	CHECK(std::string(out[0].connector) == "card1-HDMI-A-2");
	CHECK(out[0].manufacturer_id == ACR_RAW);
}

namespace {

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

} // namespace

TEST_CASE("sysfs: reads connected connectors, their EDID, enabled state and modes")
{
	char tmpl[] = "/tmp/dxr_drm_fixture_XXXXXX";
	REQUIRE(mkdtemp(tmpl) != nullptr);
	const std::string root = tmpl;

	const std::string hdmi = root + "/card1-HDMI-A-1";
	const std::string dp = root + "/card1-DP-1";
	const std::string edp = root + "/card1-eDP-1";
	REQUIRE(mkdir(hdmi.c_str(), 0700) == 0);
	REQUIRE(mkdir(dp.c_str(), 0700) == 0);
	REQUIRE(mkdir(edp.c_str(), 0700) == 0);
	REQUIRE(mkdir((root + "/card1").c_str(), 0700) == 0);
	write_text(root + "/version", "drm 1.1.0\n");

	write_text(hdmi + "/status", "connected\n");
	write_text(hdmi + "/enabled", "enabled\n");
	write_file(hdmi + "/edid", DS1_EDID, sizeof(DS1_EDID));
	write_text(hdmi + "/modes", "3840x2160\n3840x2160\n1920x1080\n");

	write_text(dp + "/status", "disconnected\n");
	write_text(dp + "/enabled", "disabled\n");
	write_file(dp + "/edid", nullptr, 0);

	const std::vector<uint8_t> e = edp_no_serial_edid();
	write_text(edp + "/status", "connected\n");
	write_text(edp + "/enabled", "disabled\n");
	write_file(edp + "/edid", e.data(), e.size());
	write_text(edp + "/modes", "2880x1800\n");

	os_display_drm_connector c[8] = {};
	const uint32_t n = os_display_drm_read_connectors(root.c_str(), c, 8);
	REQUIRE(n == 2);

	// Sorted by name, card prefix kept: "card1-HDMI-A-1" < "card1-eDP-1".
	const os_display_drm_connector *h = &c[0];
	const os_display_drm_connector *l = &c[1];
	CHECK(std::string(h->name) == "card1-HDMI-A-1");
	CHECK(h->enabled);
	CHECK(h->has_edid);
	CHECK(h->edid.serial_number == 0x322EF05Eu);
	REQUIRE(h->mode_count == 3);
	CHECK(h->mode_w[0] == 3840);
	CHECK(h->mode_h[2] == 1080);
	CHECK(std::string(l->name) == "card1-eDP-1");
	CHECK_FALSE(l->enabled);
	CHECK(l->has_edid);

	// Clean up the fixture tree.
	for (const std::string &d : {hdmi, dp, edp}) {
		for (const char *f : {"/status", "/enabled", "/edid", "/modes"}) {
			(void)unlink((d + f).c_str());
		}
		(void)rmdir(d.c_str());
	}
	(void)rmdir((root + "/card1").c_str());
	(void)unlink((root + "/version").c_str());
	(void)rmdir(root.c_str());
}
