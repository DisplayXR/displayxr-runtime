// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pins the macOS monitor enumeration's pure parts: the CoreGraphics
 *         vendor-number byte order, the EDID <-> display match, and the
 *         shared EDID parser on a real external monitor's EDID (as IOKit
 *         publishes it on Apple Silicon), plus a live smoke check of the
 *         enumerator itself.
 */

#include "catch_amalgamated.hpp"

#include "os/os_display_edid.h"
#include "os/os_display_edid_parse.h"
#include "os/os_display_macos.h"

#include <cstring>

namespace {

/*
 * Samsung Odyssey G90XF base block, from `ioreg -l` (the `EDID` property of
 * IOPortTransportStateDisplayPort on an M1 Pro): vendor SAM, product 0x785A,
 * serial 911554135, first detailed timing 600x340 mm, and a 0xFC name that
 * fills all 13 bytes with no LF terminator.
 */
const uint8_t G90XF_EDID[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4c, 0x2d, 0x5a, 0x78, 0x57, 0x36, 0x55, 0x36, 0x32, 0x22, 0x01,
    0x03, 0x80, 0x3c, 0x22, 0x78, 0x2a, 0xe0, 0x95, 0xae, 0x4e, 0x3a, 0xbb, 0x25, 0x0c, 0x50, 0x54, 0x25, 0xcf, 0x00,
    0x71, 0x4f, 0x81, 0xc0, 0x81, 0x00, 0x81, 0x80, 0x95, 0x00, 0xa9, 0xc0, 0xb3, 0x00, 0xd1, 0xc0, 0x08, 0xe8, 0x00,
    0x30, 0xf2, 0x70, 0x5a, 0x80, 0xb0, 0x58, 0x8a, 0x00, 0x58, 0x54, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd,
    0x00, 0x1e, 0xa5, 0x1e, 0xff, 0xa3, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00,
    0x4f, 0x64, 0x79, 0x73, 0x73, 0x65, 0x79, 0x20, 0x47, 0x39, 0x30, 0x58, 0x46, 0x00, 0x00, 0x00, 0xff, 0x00, 0x48,
    0x33, 0x41, 0x58, 0x43, 0x30, 0x30, 0x31, 0x31, 0x35, 0x0a, 0x20, 0x20, 0x01, 0xc2,
};

// What CoreGraphics reports for the same monitor.
constexpr uint32_t G90XF_CG_VENDOR = 0x4C2D; // "SAM", big-endian PNP
constexpr uint32_t G90XF_CG_MODEL = 0x785A;
constexpr uint32_t G90XF_CG_SERIAL = 911554135u;

os_display_edid_parsed
ids(uint16_t mfr, uint16_t product, uint32_t serial)
{
	os_display_edid_parsed p = {};
	p.manufacturer_id = mfr;
	p.product_id = product;
	p.serial_number = serial;
	return p;
}

} // namespace

TEST_CASE("macOS EDID: CoreGraphics vendor numbers are byte-swapped EDID bytes 8-9", "[aux][edid][macos]")
{
	CHECK(os_display_macos_cg_vendor_to_edid_raw(G90XF_CG_VENDOR) == 0x2D4C);
	CHECK(os_display_macos_cg_vendor_to_edid_raw(0x0610) == 0x1006); // Apple built-in
	// High bits beyond the 16-bit PNP id are ignored.
	CHECK(os_display_macos_cg_vendor_to_edid_raw(0xABCD4C2Du) == 0x2D4C);
}

TEST_CASE("macOS EDID: the shared parser reads an IOKit EDID", "[aux][edid][macos]")
{
	os_display_edid_parsed p = {};
	REQUIRE(os_display_edid_parse(G90XF_EDID, sizeof(G90XF_EDID), &p));
	CHECK(p.manufacturer_id == os_display_macos_cg_vendor_to_edid_raw(G90XF_CG_VENDOR));
	CHECK(p.product_id == G90XF_CG_MODEL);
	CHECK(p.serial_number == G90XF_CG_SERIAL);
	CHECK(p.dtd_width_mm == 600);
	CHECK(p.dtd_height_mm == 340);
	CHECK(p.dtd_width_px == 3840);
	CHECK(p.dtd_height_px == 2160);
	// 13 name bytes, no LF: the whole field is the name.
	CHECK(std::strcmp(p.monitor_name, "Odyssey G90XF") == 0);
}

TEST_CASE("macOS EDID: 0xFC name is LF-terminated and space-trimmed", "[aux][edid][macos]")
{
	uint8_t e[128];
	std::memcpy(e, G90XF_EDID, sizeof(e));
	// Rewrite the 0xFC descriptor (bytes 90..107) to "DS1 " + LF + padding.
	const uint8_t name[13] = {'D', 'S', '1', ' ', 0x0a, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
	REQUIRE(e[90 + 3] == 0xfc);
	std::memcpy(&e[90 + 5], name, sizeof(name));
	os_display_edid_parsed p = {};
	REQUIRE(os_display_edid_parse(e, sizeof(e), &p));
	CHECK(std::strcmp(p.monitor_name, "DS1") == 0);

	// No 0xFC descriptor at all: empty, never garbage.
	e[90 + 3] = 0xfe;
	REQUIRE(os_display_edid_parse(e, sizeof(e), &p));
	CHECK(p.monitor_name[0] == '\0');
}

TEST_CASE("macOS EDID: match a display to its blob", "[aux][edid][macos]")
{
	const uint16_t sam = os_display_macos_cg_vendor_to_edid_raw(G90XF_CG_VENDOR);

	SECTION("vendor + product + serial")
	{
		const os_display_edid_parsed c[] = {
		    ids(0x1006, 0xA04E, 0),
		    ids(sam, G90XF_CG_MODEL, 1234),
		    ids(sam, G90XF_CG_MODEL, G90XF_CG_SERIAL),
		};
		CHECK(os_display_macos_match_edid(G90XF_CG_VENDOR, G90XF_CG_MODEL, G90XF_CG_SERIAL, c, nullptr, 3) ==
		      2);
	}

	SECTION("a different serial is a different monitor")
	{
		const os_display_edid_parsed c[] = {ids(sam, G90XF_CG_MODEL, 1234)};
		CHECK(os_display_macos_match_edid(G90XF_CG_VENDOR, G90XF_CG_MODEL, G90XF_CG_SERIAL, c, nullptr, 1) ==
		      -1);
	}

	SECTION("serial-less on either side falls back to vendor + product")
	{
		const os_display_edid_parsed c[] = {ids(sam, G90XF_CG_MODEL, 0)};
		CHECK(os_display_macos_match_edid(G90XF_CG_VENDOR, G90XF_CG_MODEL, G90XF_CG_SERIAL, c, nullptr, 1) ==
		      0);
		const os_display_edid_parsed d[] = {ids(sam, G90XF_CG_MODEL, 77)};
		CHECK(os_display_macos_match_edid(G90XF_CG_VENDOR, G90XF_CG_MODEL, 0, d, nullptr, 1) == 0);
	}

	SECTION("an exact serial wins over a serial-less candidate listed first")
	{
		const os_display_edid_parsed c[] = {
		    ids(sam, G90XF_CG_MODEL, 0),
		    ids(sam, G90XF_CG_MODEL, G90XF_CG_SERIAL),
		};
		CHECK(os_display_macos_match_edid(G90XF_CG_VENDOR, G90XF_CG_MODEL, G90XF_CG_SERIAL, c, nullptr, 2) ==
		      1);
	}

	SECTION("two identical serial-less monitors take one blob each")
	{
		const os_display_edid_parsed c[] = {ids(sam, G90XF_CG_MODEL, 0), ids(sam, G90XF_CG_MODEL, 0)};
		bool used[2] = {false, false};
		const int32_t a = os_display_macos_match_edid(G90XF_CG_VENDOR, G90XF_CG_MODEL, 0, c, used, 2);
		REQUIRE(a == 0);
		used[a] = true;
		CHECK(os_display_macos_match_edid(G90XF_CG_VENDOR, G90XF_CG_MODEL, 0, c, used, 2) == 1);
	}

	SECTION("wrong vendor or product never matches")
	{
		const os_display_edid_parsed c[] = {ids(sam, 0x1111, G90XF_CG_SERIAL),
		                                    ids(0x1006, G90XF_CG_MODEL, G90XF_CG_SERIAL)};
		CHECK(os_display_macos_match_edid(G90XF_CG_VENDOR, G90XF_CG_MODEL, G90XF_CG_SERIAL, c, nullptr, 2) ==
		      -1);
		CHECK(os_display_macos_match_edid(G90XF_CG_VENDOR, G90XF_CG_MODEL, G90XF_CG_SERIAL, nullptr, nullptr,
		                                  0) == -1);
	}
}

/*
 * Live: every Mac running this has at least one active display unless it is a
 * headless CI runner. Where there is one, the record must be self-consistent:
 * main display first at the origin, rects in points, the backing mode at least
 * as large, and the UUID as the platform key.
 */
TEST_CASE("macOS EDID: live enumeration is self-consistent", "[aux][edid][macos]")
{
	os_display_edid_list list = {};
	if (!os_display_edid_enumerate(&list)) {
		SKIP("no active CoreGraphics display (headless runner)");
	}
	REQUIRE(list.count >= 1);
	CHECK(list.monitors[0].is_primary);
	CHECK(list.monitors[0].screen_left == 0);
	CHECK(list.monitors[0].screen_top == 0);
	for (uint32_t i = 0; i < list.count; i++) {
		const os_display_edid_monitor &m = list.monitors[i];
		INFO("monitor " << i << " '" << m.display_name << "' " << m.pixel_width << "x" << m.pixel_height);
		CHECK(m.pixel_width > 0);
		CHECK(m.pixel_height > 0);
		CHECK(m.native_width >= m.pixel_width);
		CHECK(m.native_height >= m.pixel_height);
		CHECK(std::strlen(m.output_name) == 36); // display UUID
		CHECK((m.join == OS_EDID_JOIN_IOKIT_EDID || m.join == OS_EDID_JOIN_CG_IDS));
		CHECK((i == 0 || !m.is_primary));
	}
}
