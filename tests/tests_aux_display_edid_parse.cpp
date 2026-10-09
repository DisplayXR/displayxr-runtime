// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pins the portable EDID physical-size derivation shared by the
 *         Windows, desktop-Linux and macOS enumerators (os_display_edid_parse.h):
 *         first detailed timing's mm, else base-block cm, else 0.
 */

#include "catch_amalgamated.hpp"

#include "os/os_display_edid_parse.h"

#include <cstring>
#include <vector>

namespace {

// A 128-byte base block with a valid header, 34x19 cm in bytes 21/22 and a
// detailed timing at 3840x2160 whose image size is 344x193 mm.
std::vector<uint8_t>
base_edid()
{
	std::vector<uint8_t> e(128, 0);
	const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
	std::memcpy(e.data(), header, sizeof(header));
	e[8] = 0x04;
	e[9] = 0x72;
	e[10] = 0x01;
	e[21] = 34;
	e[22] = 19;
	uint8_t *d = &e[54];
	d[0] = 0x08; // pixel clock 0x00e8 x 10 kHz (non-zero => a timing, not a descriptor)
	d[1] = 0x00;
	d[2] = 0x00; // h_active 3840 = 0xF00
	d[4] = 0xF0;
	d[5] = 0x70; // v_active 2160 = 0x870
	d[7] = 0x80;
	d[12] = 344 & 0xFF; // 344 = 0x158
	d[13] = 193;        // 0xC1
	d[14] = (uint8_t)(((344 >> 8) << 4) | (193 >> 8));
	return e;
}

void
mm_of(const std::vector<uint8_t> &e, uint32_t &w, uint32_t &h)
{
	os_display_edid_parsed p = {};
	REQUIRE(os_display_edid_parse(e.data(), e.size(), &p));
	os_display_edid_parsed_mm(&p, &w, &h);
}

} // namespace

TEST_CASE("edid parse: detailed-timing mm wins over base-block cm")
{
	uint32_t w = 0, h = 0;
	mm_of(base_edid(), w, h);
	CHECK(w == 344);
	CHECK(h == 193);
}

TEST_CASE("edid parse: DTD size 0 falls back to bytes 21/22 in cm x10")
{
	std::vector<uint8_t> e = base_edid();
	e[54 + 12] = 0;
	e[54 + 13] = 0;
	e[54 + 14] = 0;
	uint32_t w = 0, h = 0;
	mm_of(e, w, h);
	CHECK(w == 340);
	CHECK(h == 190);
}

TEST_CASE("edid parse: no DTD size and no base-block size is 0 mm")
{
	std::vector<uint8_t> e = base_edid();
	e[54 + 12] = 0;
	e[54 + 13] = 0;
	e[54 + 14] = 0;
	e[21] = 0;
	e[22] = 0;
	uint32_t w = 99, h = 99;
	mm_of(e, w, h);
	CHECK(w == 0);
	CHECK(h == 0);
}

TEST_CASE("edid parse: a headerless blob is rejected")
{
	std::vector<uint8_t> e = base_edid();
	e[1] = 0;
	os_display_edid_parsed p = {};
	CHECK_FALSE(os_display_edid_parse(e.data(), e.size(), &p));
}
