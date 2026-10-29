// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Sony pad HID report parsers and rumble builders (#2472).
 *
 * `gamepad_hid_report.c` is pure (no OS, no device), so this compiles it
 * straight in and needs no pad. It pins the byte layouts the Windows HID
 * reader trusts — DualSense USB 0x01 / Bluetooth 0x31, DualShock 4 USB 0x01 /
 * Bluetooth 0x11 — and the mapping onto the Xbox-layout @ref gamepad_state:
 * cross = A, circle = B, square = X, triangle = Y, Create / Share = View,
 * Options = Menu, PS = Guide, hat = d-pad, sticks −1..+1 with +Y UP (HID
 * 0 = up), triggers 0..1.
 */

#include "catch_amalgamated.hpp"

#include "gamepad_hid_report.h"

#include <cstring>
#include <limits>
#include <vector>


namespace {

constexpr float kEps = 1e-6f;

// Offsets of LX within each report, and of the fields relative to LX.
struct layout
{
	uint8_t id;
	size_t len;
	size_t lx;     // LX offset in the report
	size_t l2, r2; // relative to LX
	size_t b1;     // hat + face buttons, relative to LX
	size_t b2, b3; // relative to LX
};

constexpr layout kDualSenseUsb = {0x01, 64, 1, 4, 5, 7, 8, 9};
constexpr layout kDualSenseBt = {0x31, 78, 2, 4, 5, 7, 8, 9};
constexpr layout kDs4Usb = {0x01, 64, 1, 7, 8, 4, 5, 6};
constexpr layout kDs4Bt = {0x11, 78, 3, 7, 8, 4, 5, 6};

std::vector<uint8_t>
report(const layout &l,
       uint8_t lx,
       uint8_t ly,
       uint8_t rx,
       uint8_t ry,
       uint8_t l2,
       uint8_t r2,
       uint8_t b1,
       uint8_t b2,
       uint8_t b3)
{
	std::vector<uint8_t> r(l.len, 0);
	r[0] = l.id;
	r[l.lx + 0] = lx;
	r[l.lx + 1] = ly;
	r[l.lx + 2] = rx;
	r[l.lx + 3] = ry;
	r[l.lx + l.l2] = l2;
	r[l.lx + l.r2] = r2;
	r[l.lx + l.b1] = b1;
	r[l.lx + l.b2] = b2;
	r[l.lx + l.b3] = b3;
	return r;
}

using parse_fn = bool (*)(const uint8_t *, size_t, struct gamepad_state *);

void
check_layout(const layout &l, parse_fn parse)
{
	struct gamepad_state s = {};

	// Rest: sticks centred, triggers up, hat released (8), nothing pressed.
	auto rest = report(l, 128, 128, 128, 128, 0, 0, 0x08, 0, 0);
	REQUIRE(parse(rest.data(), rest.size(), &s));
	CHECK(s.connected);
	CHECK(s.buttons == 0u);
	CHECK(s.left_x == Catch::Approx(0.0f).margin(kEps));
	CHECK(s.left_y == Catch::Approx(0.0f).margin(kEps));
	CHECK(s.right_x == Catch::Approx(0.0f).margin(kEps));
	CHECK(s.right_y == Catch::Approx(0.0f).margin(kEps));
	CHECK(s.left_trigger == Catch::Approx(0.0f).margin(kEps));
	CHECK(s.right_trigger == Catch::Approx(0.0f).margin(kEps));

	// Extremes, one corner: left stick up-left (HID 0,0), right stick
	// down-right (255,255), triggers full.
	auto ext = report(l, 0, 0, 255, 255, 255, 255, 0x08, 0, 0);
	REQUIRE(parse(ext.data(), ext.size(), &s));
	CHECK(s.left_x == Catch::Approx(-1.0f).margin(kEps));
	CHECK(s.left_y == Catch::Approx(+1.0f).margin(kEps)); // HID 0 = up = +Y
	CHECK(s.right_x == Catch::Approx(+1.0f).margin(kEps));
	CHECK(s.right_y == Catch::Approx(-1.0f).margin(kEps));
	CHECK(s.left_trigger == Catch::Approx(1.0f).margin(kEps));
	CHECK(s.right_trigger == Catch::Approx(1.0f).margin(kEps));

	// The other corner, and a half trigger.
	auto ext2 = report(l, 255, 255, 0, 0, 0, 128, 0x08, 0, 0);
	REQUIRE(parse(ext2.data(), ext2.size(), &s));
	CHECK(s.left_x == Catch::Approx(+1.0f).margin(kEps));
	CHECK(s.left_y == Catch::Approx(-1.0f).margin(kEps));
	CHECK(s.right_x == Catch::Approx(-1.0f).margin(kEps));
	CHECK(s.right_y == Catch::Approx(+1.0f).margin(kEps));
	CHECK(s.left_trigger == Catch::Approx(0.0f).margin(kEps));
	CHECK(s.right_trigger == Catch::Approx(128.0f / 255.0f).margin(kEps));

	// Face buttons, one at a time.
	struct
	{
		uint8_t b1, b2, b3;
		uint32_t want;
	} const cases[] = {
	    {0x18, 0, 0, GAMEPAD_BTN_X}, // square
	    {0x28, 0, 0, GAMEPAD_BTN_A}, // cross
	    {0x48, 0, 0, GAMEPAD_BTN_B}, // circle
	    {0x88, 0, 0, GAMEPAD_BTN_Y}, // triangle
	    {0x08, 0x01, 0, GAMEPAD_BTN_SHOULDER_LEFT},
	    {0x08, 0x02, 0, GAMEPAD_BTN_SHOULDER_RIGHT},
	    {0x08, 0x04, 0, 0u},               // digital L2: unmapped (the analogue trigger carries it)
	    {0x08, 0x08, 0, 0u},               // digital R2
	    {0x08, 0x10, 0, GAMEPAD_BTN_VIEW}, // Create / Share
	    {0x08, 0x20, 0, GAMEPAD_BTN_MENU}, // Options
	    {0x08, 0x40, 0, GAMEPAD_BTN_THUMB_LEFT},
	    {0x08, 0x80, 0, GAMEPAD_BTN_THUMB_RIGHT},
	    {0x08, 0, 0x01, GAMEPAD_BTN_GUIDE}, // PS
	    {0x08, 0, 0x02, 0u},                // touchpad click: unmapped
	    // Hat: N, NE, E, SE, S, SW, W, NW, released.
	    {0x00, 0, 0, GAMEPAD_BTN_DPAD_UP},
	    {0x01, 0, 0, GAMEPAD_BTN_DPAD_UP | GAMEPAD_BTN_DPAD_RIGHT},
	    {0x02, 0, 0, GAMEPAD_BTN_DPAD_RIGHT},
	    {0x03, 0, 0, GAMEPAD_BTN_DPAD_DOWN | GAMEPAD_BTN_DPAD_RIGHT},
	    {0x04, 0, 0, GAMEPAD_BTN_DPAD_DOWN},
	    {0x05, 0, 0, GAMEPAD_BTN_DPAD_DOWN | GAMEPAD_BTN_DPAD_LEFT},
	    {0x06, 0, 0, GAMEPAD_BTN_DPAD_LEFT},
	    {0x07, 0, 0, GAMEPAD_BTN_DPAD_UP | GAMEPAD_BTN_DPAD_LEFT},
	    {0x0F, 0, 0, 0u},
	};
	for (const auto &c : cases) {
		auto r = report(l, 128, 128, 128, 128, 0, 0, c.b1, c.b2, c.b3);
		REQUIRE(parse(r.data(), r.size(), &s));
		INFO("b1=" << int(c.b1) << " b2=" << int(c.b2) << " b3=" << int(c.b3));
		CHECK(s.buttons == c.want);
	}

	// Everything at once: no bit leaks into another.
	auto all = report(l, 128, 128, 128, 128, 0, 0, 0xF0 | 0x00, 0xF3, 0x01);
	REQUIRE(parse(all.data(), all.size(), &s));
	CHECK(s.buttons == (GAMEPAD_BTN_A | GAMEPAD_BTN_B | GAMEPAD_BTN_X | GAMEPAD_BTN_Y | GAMEPAD_BTN_SHOULDER_LEFT |
	                    GAMEPAD_BTN_SHOULDER_RIGHT | GAMEPAD_BTN_VIEW | GAMEPAD_BTN_MENU | GAMEPAD_BTN_THUMB_LEFT |
	                    GAMEPAD_BTN_THUMB_RIGHT | GAMEPAD_BTN_GUIDE | GAMEPAD_BTN_DPAD_UP));

	// Truncated: one byte short of the last field is refused, out untouched.
	struct gamepad_state untouched = {};
	untouched.buttons = 0xDEAD;
	const size_t need = l.lx + (l.b3 > l.r2 ? l.b3 : l.r2) + 1;
	CHECK_FALSE(parse(rest.data(), need - 1, &untouched));
	CHECK(untouched.buttons == 0xDEADu);
	CHECK(parse(rest.data(), need, &untouched));
}

} // namespace


TEST_CASE("DualSense USB report 0x01", "[gamepad][hid]")
{
	check_layout(kDualSenseUsb, gamepad_hid_parse_dualsense);
}

TEST_CASE("DualSense Bluetooth report 0x31", "[gamepad][hid]")
{
	check_layout(kDualSenseBt, gamepad_hid_parse_dualsense);
}

TEST_CASE("DualShock 4 USB report 0x01", "[gamepad][hid]")
{
	check_layout(kDs4Usb, gamepad_hid_parse_dualshock4);
}

TEST_CASE("DualShock 4 Bluetooth report 0x11", "[gamepad][hid]")
{
	check_layout(kDs4Bt, gamepad_hid_parse_dualshock4);
}

TEST_CASE("Unknown report ids are refused", "[gamepad][hid]")
{
	struct gamepad_state s = {};
	std::vector<uint8_t> r(78, 0);
	for (uint8_t id : {0x00, 0x02, 0x05, 0x11, 0x20}) {
		r[0] = id;
		CHECK_FALSE(gamepad_hid_parse_dualsense(r.data(), r.size(), &s));
	}
	for (uint8_t id : {0x00, 0x02, 0x05, 0x31}) {
		r[0] = id;
		CHECK_FALSE(gamepad_hid_parse_dualshock4(r.data(), r.size(), &s));
	}
	CHECK_FALSE(gamepad_hid_parse_dualsense(nullptr, 64, &s));
	CHECK_FALSE(gamepad_hid_parse_dualsense(r.data(), 0, &s));
}

TEST_CASE("Router: Bluetooth DualSense simple mode uses the DualShock 4 layout", "[gamepad][hid]")
{
	struct gamepad_state s = {};
	// Simple report: cross pressed, L2 full — at the DS4 offsets.
	auto simple = report(kDs4Usb, 128, 128, 128, 128, 255, 0, 0x28, 0, 0);
	simple.resize(78, 0);
	REQUIRE(gamepad_hid_parse(GAMEPAD_HID_PAD_DUALSENSE, true, simple.data(), simple.size(), &s));
	CHECK(s.buttons == GAMEPAD_BTN_A);
	CHECK(s.left_trigger == Catch::Approx(1.0f).margin(kEps));

	// The same bytes over USB are read as the full DualSense layout: its b1 (byte
	// 8) is the simple report's L2 = 0xFF → hat released + all four face buttons.
	REQUIRE(gamepad_hid_parse(GAMEPAD_HID_PAD_DUALSENSE, false, simple.data(), 64, &s));
	CHECK(s.buttons == (GAMEPAD_BTN_A | GAMEPAD_BTN_B | GAMEPAD_BTN_X | GAMEPAD_BTN_Y));

	auto full = report(kDualSenseBt, 128, 128, 128, 128, 0, 0, 0x48, 0, 0);
	REQUIRE(gamepad_hid_parse(GAMEPAD_HID_PAD_DUALSENSE_EDGE, true, full.data(), full.size(), &s));
	CHECK(s.buttons == GAMEPAD_BTN_B);
	CHECK_FALSE(gamepad_hid_parse(GAMEPAD_HID_PAD_NONE, false, full.data(), full.size(), &s));
}

TEST_CASE("VID:PID matching", "[gamepad][hid]")
{
	CHECK(gamepad_hid_match(0x054C, 0x0CE6) == GAMEPAD_HID_PAD_DUALSENSE);
	CHECK(gamepad_hid_match(0x054C, 0x0DF2) == GAMEPAD_HID_PAD_DUALSENSE_EDGE);
	CHECK(gamepad_hid_match(0x054C, 0x05C4) == GAMEPAD_HID_PAD_DUALSHOCK4);
	CHECK(gamepad_hid_match(0x054C, 0x09CC) == GAMEPAD_HID_PAD_DUALSHOCK4);
	CHECK(gamepad_hid_match(0x054C, 0x0268) == GAMEPAD_HID_PAD_NONE); // DualShock 3
	CHECK(gamepad_hid_match(0x045E, 0x0CE6) == GAMEPAD_HID_PAD_NONE); // wrong vendor
}

TEST_CASE("CRC-32 is the standard IEEE one", "[gamepad][hid]")
{
	const char *check = "123456789";
	CHECK(gamepad_hid_crc32(0, reinterpret_cast<const uint8_t *>(check), 9) == 0xCBF43926u);
	// Chaining equals one pass.
	const uint32_t a = gamepad_hid_crc32(0, reinterpret_cast<const uint8_t *>(check), 4);
	CHECK(gamepad_hid_crc32(a, reinterpret_cast<const uint8_t *>(check) + 4, 5) == 0xCBF43926u);
}

namespace {

uint32_t
trailer(const uint8_t *buf, size_t len)
{
	return uint32_t(buf[len - 4]) | (uint32_t(buf[len - 3]) << 8) | (uint32_t(buf[len - 2]) << 16) |
	       (uint32_t(buf[len - 1]) << 24);
}

uint32_t
bt_crc(const uint8_t *buf, size_t len)
{
	const uint8_t seed = 0xA2;
	return gamepad_hid_crc32(gamepad_hid_crc32(0, &seed, 1), buf, len - 4);
}

} // namespace

TEST_CASE("Rumble output reports", "[gamepad][hid]")
{
	uint8_t buf[96];

	SECTION("DualSense USB")
	{
		REQUIRE(gamepad_hid_build_rumble(GAMEPAD_HID_PAD_DUALSENSE, false, 0, 1.0f, 0.5f, buf, sizeof(buf)) ==
		        48);
		CHECK(buf[0] == 0x02);
		CHECK(buf[1] == 0x03);
		CHECK(buf[3] == 128); // right / high frequency
		CHECK(buf[4] == 255); // left / low frequency
		CHECK(buf[39] == 0x04);
	}
	SECTION("DualSense Bluetooth")
	{
		REQUIRE(gamepad_hid_build_rumble(GAMEPAD_HID_PAD_DUALSENSE, true, 3, 0.0f, 1.0f, buf, sizeof(buf)) ==
		        78);
		CHECK(buf[0] == 0x31);
		CHECK(buf[1] == 0x30);
		CHECK(buf[2] == 0x10);
		CHECK(buf[3] == 0x03);
		CHECK(buf[5] == 255);
		CHECK(buf[6] == 0);
		CHECK(trailer(buf, 78) == bt_crc(buf, 78));
	}
	SECTION("DualShock 4 USB")
	{
		REQUIRE(gamepad_hid_build_rumble(GAMEPAD_HID_PAD_DUALSHOCK4, false, 0, 0.25f, 0.75f, buf,
		                                 sizeof(buf)) == 32);
		CHECK(buf[0] == 0x05);
		CHECK(buf[1] == 0x01);
		CHECK(buf[4] == 191); // small / right
		CHECK(buf[5] == 64);  // big / left
	}
	SECTION("DualShock 4 Bluetooth")
	{
		REQUIRE(gamepad_hid_build_rumble(GAMEPAD_HID_PAD_DUALSHOCK4, true, 0, 1.0f, 0.0f, buf, sizeof(buf)) ==
		        78);
		CHECK(buf[0] == 0x11);
		CHECK(buf[1] == 0xC0);
		CHECK(buf[3] == 0x01);
		CHECK(buf[6] == 0);
		CHECK(buf[7] == 255);
		CHECK(trailer(buf, 78) == bt_crc(buf, 78));
	}
	SECTION("Clamping, NaN, too small, unknown pad")
	{
		const float nan = std::numeric_limits<float>::quiet_NaN();
		REQUIRE(gamepad_hid_build_rumble(GAMEPAD_HID_PAD_DUALSENSE, false, 0, nan, 7.0f, buf, sizeof(buf)) ==
		        48);
		CHECK(buf[3] == 255);
		CHECK(buf[4] == 0);
		CHECK(gamepad_hid_build_rumble(GAMEPAD_HID_PAD_DUALSENSE, false, 0, 1, 1, buf, 47) == 0);
		CHECK(gamepad_hid_build_rumble(GAMEPAD_HID_PAD_NONE, false, 0, 1, 1, buf, sizeof(buf)) == 0);
	}
}
