// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Linux evdev pad mapping (drivers/gamepad/gamepad_evdev_map.c):
 *         layouts, axes, triggers, the d-pad hat and the SteamOS device
 *         preference. Pure; runs on every platform.
 */

#include "catch_amalgamated.hpp"

#include "gamepad_evdev_map.h"

namespace {

//! Steam Input's virtual pad (an Xbox 360 pad over uinput): xpad layout,
//! sticks -32768..32767, triggers 0..255, d-pad on the hat.
gamepad_evdev_pad
steam_virtual_pad()
{
	gamepad_evdev_pad p;
	gamepad_evdev_pad_init(&p, gamepad_evdev_layout_for(GAMEPAD_EVDEV_VID_VALVE, ""));
	for (uint16_t a : {GP_ABS_X, GP_ABS_Y, GP_ABS_RX, GP_ABS_RY}) {
		gamepad_evdev_set_abs(&p, a, -32768, 32767, 0);
	}
	gamepad_evdev_set_abs(&p, GP_ABS_Z, 0, 255, 0);
	gamepad_evdev_set_abs(&p, GP_ABS_RZ, 0, 255, 0);
	gamepad_evdev_set_abs(&p, GP_ABS_HAT0X, -1, 1, 0);
	gamepad_evdev_set_abs(&p, GP_ABS_HAT0Y, -1, 1, 0);
	return p;
}

gamepad_state
fill(const gamepad_evdev_pad &p)
{
	gamepad_state s = {};
	gamepad_evdev_fill(&p, &s);
	return s;
}

} // namespace

TEST_CASE("evdev rank: Steam Input's virtual pad beats a raw Valve device and any other pad")
{
	const int virt = gamepad_evdev_rank(GAMEPAD_EVDEV_VID_VALVE, GAMEPAD_EVDEV_PID_STEAM_VIRTUAL);
	const int deck = gamepad_evdev_rank(GAMEPAD_EVDEV_VID_VALVE, 0x1205); // hid-steam, Deck controls
	const int xbox = gamepad_evdev_rank(GAMEPAD_EVDEV_VID_MICROSOFT, 0x0B12);
	const int sony = gamepad_evdev_rank(0x054C, 0x0CE6);
	CHECK(virt > xbox);
	CHECK(xbox == sony);
	CHECK(xbox > deck);
	CHECK(deck > 0);
}

TEST_CASE("evdev layout: xpad by driver or by Microsoft/Valve vendor, the spec otherwise")
{
	CHECK(gamepad_evdev_layout_for(0x2DC8, "xpad") == GAMEPAD_EVDEV_LAYOUT_XPAD); // third-party Xbox pad
	CHECK(gamepad_evdev_layout_for(GAMEPAD_EVDEV_VID_MICROSOFT, nullptr) == GAMEPAD_EVDEV_LAYOUT_XPAD);
	CHECK(gamepad_evdev_layout_for(GAMEPAD_EVDEV_VID_VALVE, "hid-steam") == GAMEPAD_EVDEV_LAYOUT_XPAD);
	CHECK(gamepad_evdev_layout_for(0x054C, "playstation") == GAMEPAD_EVDEV_LAYOUT_SPEC);
	CHECK(gamepad_evdev_layout_for(0x057E, "") == GAMEPAD_EVDEV_LAYOUT_SPEC);
}

TEST_CASE("evdev buttons: X and Y land on the Xbox positions in both layouts")
{
	// xpad: 0x133 is the LEFT button (X), 0x134 the TOP one (Y).
	CHECK(gamepad_evdev_button_for_key(GP_BTN_0x133, GAMEPAD_EVDEV_LAYOUT_XPAD) == GAMEPAD_BTN_X);
	CHECK(gamepad_evdev_button_for_key(GP_BTN_0x134, GAMEPAD_EVDEV_LAYOUT_XPAD) == GAMEPAD_BTN_Y);
	// spec: 0x133 is BTN_NORTH (top, Y), 0x134 BTN_WEST (left, X).
	CHECK(gamepad_evdev_button_for_key(GP_BTN_0x133, GAMEPAD_EVDEV_LAYOUT_SPEC) == GAMEPAD_BTN_Y);
	CHECK(gamepad_evdev_button_for_key(GP_BTN_0x134, GAMEPAD_EVDEV_LAYOUT_SPEC) == GAMEPAD_BTN_X);
	for (auto l : {GAMEPAD_EVDEV_LAYOUT_XPAD, GAMEPAD_EVDEV_LAYOUT_SPEC}) {
		CHECK(gamepad_evdev_button_for_key(GP_BTN_SOUTH, l) == GAMEPAD_BTN_A);
		CHECK(gamepad_evdev_button_for_key(GP_BTN_EAST, l) == GAMEPAD_BTN_B);
		CHECK(gamepad_evdev_button_for_key(GP_BTN_START, l) == GAMEPAD_BTN_MENU);
		CHECK(gamepad_evdev_button_for_key(GP_BTN_SELECT, l) == GAMEPAD_BTN_VIEW);
		CHECK(gamepad_evdev_button_for_key(GP_BTN_MODE, l) == GAMEPAD_BTN_GUIDE);
		CHECK(gamepad_evdev_button_for_key(GP_BTN_THUMBR, l) == GAMEPAD_BTN_THUMB_RIGHT);
		CHECK(gamepad_evdev_button_for_key(GP_BTN_DPAD_LEFT, l) == GAMEPAD_BTN_DPAD_LEFT);
		CHECK(gamepad_evdev_button_for_key(0x110 /* BTN_LEFT, a mouse */, l) == 0);
	}
}

TEST_CASE("evdev normalise: sticks about the centre, triggers from the minimum, both clamped")
{
	CHECK(gamepad_evdev_norm_stick(0, -32768, 32767) == Catch::Approx(0.0f).margin(1e-4));
	CHECK(gamepad_evdev_norm_stick(32767, -32768, 32767) == Catch::Approx(1.0f));
	CHECK(gamepad_evdev_norm_stick(-32768, -32768, 32767) == Catch::Approx(-1.0f));
	CHECK(gamepad_evdev_norm_stick(128, 0, 255) == Catch::Approx(0.0f).margin(0.01)); // unsigned 0..255 stick
	CHECK(gamepad_evdev_norm_stick(99999, -100, 100) == 1.0f);
	CHECK(gamepad_evdev_norm_stick(5, 7, 7) == 0.0f); // degenerate range
	CHECK(gamepad_evdev_norm_trigger(0, 0, 255) == 0.0f);
	CHECK(gamepad_evdev_norm_trigger(255, 0, 255) == 1.0f);
	CHECK(gamepad_evdev_norm_trigger(-5, 0, 255) == 0.0f);
	CHECK(gamepad_evdev_norm_trigger(3, 9, 2) == 0.0f);
}

TEST_CASE("evdev fill: the Steam virtual pad, event by event")
{
	gamepad_evdev_pad p = steam_virtual_pad();

	gamepad_state s = fill(p);
	CHECK(s.connected);
	CHECK(s.buttons == 0);
	CHECK(s.left_x == Catch::Approx(0.0f).margin(1e-4));

	// Left stick pushed UP: evdev Y goes negative; the provider's goes positive.
	gamepad_evdev_apply(&p, GP_EV_ABS, GP_ABS_Y, -32768);
	gamepad_evdev_apply(&p, GP_EV_ABS, GP_ABS_RX, 32767);
	gamepad_evdev_apply(&p, GP_EV_ABS, GP_ABS_RZ, 255);
	gamepad_evdev_apply(&p, GP_EV_KEY, GP_BTN_SOUTH, 1);
	gamepad_evdev_apply(&p, GP_EV_KEY, GP_BTN_0x134, 2); // autorepeat = held
	gamepad_evdev_apply(&p, GP_EV_ABS, GP_ABS_HAT0X, -1);
	gamepad_evdev_apply(&p, GP_EV_ABS, GP_ABS_HAT0Y, 1);
	s = fill(p);
	CHECK(s.left_y == Catch::Approx(1.0f));
	CHECK(s.right_x == Catch::Approx(1.0f));
	CHECK(s.right_trigger == Catch::Approx(1.0f));
	CHECK(s.left_trigger == 0.0f);
	CHECK(s.buttons == (GAMEPAD_BTN_A | GAMEPAD_BTN_Y | GAMEPAD_BTN_DPAD_LEFT | GAMEPAD_BTN_DPAD_DOWN));

	gamepad_evdev_apply(&p, GP_EV_KEY, GP_BTN_SOUTH, 0);
	gamepad_evdev_apply(&p, GP_EV_ABS, GP_ABS_HAT0X, 0);
	gamepad_evdev_apply(&p, GP_EV_ABS, GP_ABS_HAT0Y, 0);
	s = fill(p);
	CHECK(s.buttons == GAMEPAD_BTN_Y);
}

TEST_CASE("evdev fill: the Deck's raw hid-steam device reports its triggers on HAT2Y / HAT2X")
{
	gamepad_evdev_pad p;
	gamepad_evdev_pad_init(&p, GAMEPAD_EVDEV_LAYOUT_XPAD);
	gamepad_evdev_set_abs(&p, GP_ABS_X, -32767, 32767, 0);
	gamepad_evdev_set_abs(&p, GP_ABS_Y, -32767, 32767, 0);
	gamepad_evdev_set_abs(&p, GP_ABS_HAT2Y, 0, 32767, 32767); // left trigger, fully pulled
	gamepad_evdev_set_abs(&p, GP_ABS_HAT2X, 0, 32767, 0);
	gamepad_evdev_apply(&p, GP_EV_KEY, GP_BTN_DPAD_UP, 1); // d-pad as keys, no hat
	const gamepad_state s = fill(p);
	CHECK(s.left_trigger == Catch::Approx(1.0f));
	CHECK(s.right_trigger == 0.0f);
	CHECK(s.buttons == GAMEPAD_BTN_DPAD_UP);
}

TEST_CASE("evdev fill: a pad with digital triggers only reads 0 or 1")
{
	gamepad_evdev_pad p;
	gamepad_evdev_pad_init(&p, GAMEPAD_EVDEV_LAYOUT_SPEC);
	gamepad_evdev_set_abs(&p, GP_ABS_X, 0, 255, 128);
	gamepad_evdev_set_abs(&p, GP_ABS_Y, 0, 255, 128);
	gamepad_evdev_apply(&p, GP_EV_KEY, GP_BTN_TR2, 1);
	gamepad_state s = fill(p);
	CHECK(s.right_trigger == 1.0f);
	CHECK(s.left_trigger == 0.0f);
	CHECK(s.buttons == 0); // the trigger is not a button
	gamepad_evdev_apply(&p, GP_EV_KEY, GP_BTN_TR2, 0);
	s = fill(p);
	CHECK(s.right_trigger == 0.0f);
}

TEST_CASE("evdev apply: unknown types and out-of-range axes are ignored")
{
	gamepad_evdev_pad p = steam_virtual_pad();
	gamepad_evdev_apply(&p, 0x02 /* EV_REL */, 0, 5);
	gamepad_evdev_apply(&p, GP_EV_ABS, 0x3F /* ABS_MAX */, 5);
	gamepad_evdev_apply(&p, GP_EV_SYN, GP_SYN_DROPPED, 0);
	const gamepad_state s = fill(p);
	CHECK(s.buttons == 0);
	CHECK(s.left_x == Catch::Approx(0.0f).margin(1e-4));
}
