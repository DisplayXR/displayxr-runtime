// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Linux evdev gamepad events -> @ref gamepad_state, with no I/O.
 * @ingroup drv_gamepad
 */

#include "gamepad_evdev_map.h"

#include <string.h>

#if defined(__linux__) && !defined(__ANDROID__) && __has_include(<linux/input-event-codes.h>)
// The local copies are the kernel's ABI; hold them to it where the header exists.
#include <linux/input-event-codes.h>
_Static_assert(GP_EV_KEY == EV_KEY && GP_EV_ABS == EV_ABS && GP_SYN_DROPPED == SYN_DROPPED, "evdev types");
_Static_assert(GP_BTN_SOUTH == BTN_SOUTH && GP_BTN_EAST == BTN_EAST && GP_BTN_0x133 == BTN_NORTH &&
                   GP_BTN_0x133 == BTN_X && GP_BTN_0x134 == BTN_WEST && GP_BTN_0x134 == BTN_Y,
               "face buttons");
_Static_assert(GP_BTN_TL == BTN_TL && GP_BTN_TR2 == BTN_TR2 && GP_BTN_SELECT == BTN_SELECT &&
                   GP_BTN_START == BTN_START && GP_BTN_MODE == BTN_MODE && GP_BTN_THUMBR == BTN_THUMBR,
               "other buttons");
_Static_assert(GP_BTN_DPAD_UP == BTN_DPAD_UP && GP_BTN_DPAD_RIGHT == BTN_DPAD_RIGHT, "d-pad");
_Static_assert(GP_ABS_RX == ABS_RX && GP_ABS_RZ == ABS_RZ && GP_ABS_GAS == ABS_GAS && GP_ABS_BRAKE == ABS_BRAKE &&
                   GP_ABS_HAT0Y == ABS_HAT0Y && GP_ABS_HAT2X == ABS_HAT2X && GP_ABS_HAT2Y == ABS_HAT2Y,
               "axes");
#endif

enum gamepad_evdev_layout
gamepad_evdev_layout_for(uint16_t vendor, const char *driver)
{
	if (driver != NULL && strcmp(driver, "xpad") == 0) {
		return GAMEPAD_EVDEV_LAYOUT_XPAD;
	}
	if (vendor == GAMEPAD_EVDEV_VID_MICROSOFT || vendor == GAMEPAD_EVDEV_VID_VALVE) {
		return GAMEPAD_EVDEV_LAYOUT_XPAD;
	}
	return GAMEPAD_EVDEV_LAYOUT_SPEC;
}

int
gamepad_evdev_rank(uint16_t vendor, uint16_t product)
{
	if (vendor == GAMEPAD_EVDEV_VID_VALVE) {
		return product == GAMEPAD_EVDEV_PID_STEAM_VIRTUAL ? 3 : 1;
	}
	return 2;
}

void
gamepad_evdev_pad_init(struct gamepad_evdev_pad *pad, enum gamepad_evdev_layout layout)
{
	memset(pad, 0, sizeof(*pad));
	pad->layout = layout;
}

void
gamepad_evdev_set_abs(struct gamepad_evdev_pad *pad, uint16_t code, int32_t min, int32_t max, int32_t value)
{
	if (code >= GAMEPAD_EVDEV_ABS_COUNT) {
		return;
	}
	pad->abs_present[code] = true;
	pad->abs_min[code] = min;
	pad->abs_max[code] = max;
	pad->abs_value[code] = value;
}

uint32_t
gamepad_evdev_button_for_key(uint16_t code, enum gamepad_evdev_layout layout)
{
	const bool xpad = layout == GAMEPAD_EVDEV_LAYOUT_XPAD;
	switch (code) {
	case GP_BTN_SOUTH: return GAMEPAD_BTN_A;
	case GP_BTN_EAST: return GAMEPAD_BTN_B;
	case GP_BTN_0x133: return xpad ? GAMEPAD_BTN_X : GAMEPAD_BTN_Y;
	case GP_BTN_0x134: return xpad ? GAMEPAD_BTN_Y : GAMEPAD_BTN_X;
	case GP_BTN_TL: return GAMEPAD_BTN_SHOULDER_LEFT;
	case GP_BTN_TR: return GAMEPAD_BTN_SHOULDER_RIGHT;
	case GP_BTN_THUMBL: return GAMEPAD_BTN_THUMB_LEFT;
	case GP_BTN_THUMBR: return GAMEPAD_BTN_THUMB_RIGHT;
	case GP_BTN_START: return GAMEPAD_BTN_MENU;
	case GP_BTN_SELECT: return GAMEPAD_BTN_VIEW;
	case GP_BTN_MODE: return GAMEPAD_BTN_GUIDE;
	case GP_BTN_DPAD_UP: return GAMEPAD_BTN_DPAD_UP;
	case GP_BTN_DPAD_DOWN: return GAMEPAD_BTN_DPAD_DOWN;
	case GP_BTN_DPAD_LEFT: return GAMEPAD_BTN_DPAD_LEFT;
	case GP_BTN_DPAD_RIGHT: return GAMEPAD_BTN_DPAD_RIGHT;
	default: return 0;
	}
}

void
gamepad_evdev_apply(struct gamepad_evdev_pad *pad, uint16_t type, uint16_t code, int32_t value)
{
	if (type == GP_EV_KEY) {
		const bool down = value != 0;
		if (code == GP_BTN_TL2) {
			pad->key_tl2 = down;
			return;
		}
		if (code == GP_BTN_TR2) {
			pad->key_tr2 = down;
			return;
		}
		const uint32_t bit = gamepad_evdev_button_for_key(code, pad->layout);
		if (bit == 0) {
			return;
		}
		if (down) {
			pad->key_buttons |= bit;
		} else {
			pad->key_buttons &= ~bit;
		}
	} else if (type == GP_EV_ABS && code < GAMEPAD_EVDEV_ABS_COUNT) {
		pad->abs_value[code] = value;
	}
}

float
gamepad_evdev_norm_stick(int32_t v, int32_t min, int32_t max)
{
	if (max <= min) {
		return 0.0f;
	}
	const double centre = ((double)min + (double)max) * 0.5;
	const double half = ((double)max - (double)min) * 0.5;
	double r = ((double)v - centre) / half;
	r = r < -1.0 ? -1.0 : (r > 1.0 ? 1.0 : r);
	return (float)r;
}

float
gamepad_evdev_norm_trigger(int32_t v, int32_t min, int32_t max)
{
	if (max <= min) {
		return 0.0f;
	}
	double r = ((double)v - (double)min) / ((double)max - (double)min);
	r = r < 0.0 ? 0.0 : (r > 1.0 ? 1.0 : r);
	return (float)r;
}

static float
stick(const struct gamepad_evdev_pad *pad, uint16_t code)
{
	if (!pad->abs_present[code]) {
		return 0.0f;
	}
	return gamepad_evdev_norm_stick(pad->abs_value[code], pad->abs_min[code], pad->abs_max[code]);
}

static float
trigger(const struct gamepad_evdev_pad *pad, uint16_t primary, uint16_t steam, uint16_t sim, bool digital)
{
	const uint16_t codes[3] = {primary, steam, sim};
	for (int i = 0; i < 3; i++) {
		const uint16_t c = codes[i];
		if (pad->abs_present[c]) {
			return gamepad_evdev_norm_trigger(pad->abs_value[c], pad->abs_min[c], pad->abs_max[c]);
		}
	}
	return digital ? 1.0f : 0.0f;
}

void
gamepad_evdev_fill(const struct gamepad_evdev_pad *pad, struct gamepad_state *out)
{
	uint32_t b = pad->key_buttons;

	// A hat d-pad (xpad, Steam's virtual pad): negative = left / up.
	if (pad->abs_present[GP_ABS_HAT0X]) {
		const int32_t hx = pad->abs_value[GP_ABS_HAT0X];
		b |= hx < 0 ? GAMEPAD_BTN_DPAD_LEFT : (hx > 0 ? GAMEPAD_BTN_DPAD_RIGHT : 0u);
	}
	if (pad->abs_present[GP_ABS_HAT0Y]) {
		const int32_t hy = pad->abs_value[GP_ABS_HAT0Y];
		b |= hy < 0 ? GAMEPAD_BTN_DPAD_UP : (hy > 0 ? GAMEPAD_BTN_DPAD_DOWN : 0u);
	}

	out->connected = true;
	out->buttons = b;
	// evdev Y grows downward; the provider's (XInput's) grows upward.
	out->left_x = stick(pad, GP_ABS_X);
	out->left_y = -stick(pad, GP_ABS_Y);
	out->right_x = stick(pad, GP_ABS_RX);
	out->right_y = -stick(pad, GP_ABS_RY);
	// hid-steam reports the Deck's triggers on HAT2Y (left) / HAT2X (right).
	out->left_trigger = trigger(pad, GP_ABS_Z, GP_ABS_HAT2Y, GP_ABS_BRAKE, pad->key_tl2);
	out->right_trigger = trigger(pad, GP_ABS_RZ, GP_ABS_HAT2X, GP_ABS_GAS, pad->key_tr2);
}
