// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Sony pad HID reports — pure parsers and output-report builders.
 *
 * See `gamepad_hid_report.h` for the contract. Depends on nothing but the
 * C library, so the unit test compiles it straight in.
 *
 * @ingroup drv_gamepad
 */

#include "gamepad_hid_report.h"

#include <string.h>


/*
 *
 * Shared field decoding.
 *
 */

//! One stick byte (0..255, 128 = centre) → −1..+1; the low side divides by 128 so 0 reaches −1 exactly.
static float
stick_byte(uint8_t v)
{
	const int c = (int)v - 128;
	return c < 0 ? (float)c / 128.0f : (float)c / 127.0f;
}

//! The same with the sign flipped: HID Y is 0 = up, @ref gamepad_state is +Y up.
static float
stick_byte_y(uint8_t v)
{
	return -stick_byte(v);
}

//! The hat in the low nibble of the face-button byte: 0 = N, clockwise to 7 = NW, 8+ = released.
static uint32_t
hat_bits(uint8_t hat)
{
	switch (hat & 0x0F) {
	case 0: return GAMEPAD_BTN_DPAD_UP;
	case 1: return GAMEPAD_BTN_DPAD_UP | GAMEPAD_BTN_DPAD_RIGHT;
	case 2: return GAMEPAD_BTN_DPAD_RIGHT;
	case 3: return GAMEPAD_BTN_DPAD_DOWN | GAMEPAD_BTN_DPAD_RIGHT;
	case 4: return GAMEPAD_BTN_DPAD_DOWN;
	case 5: return GAMEPAD_BTN_DPAD_DOWN | GAMEPAD_BTN_DPAD_LEFT;
	case 6: return GAMEPAD_BTN_DPAD_LEFT;
	case 7: return GAMEPAD_BTN_DPAD_UP | GAMEPAD_BTN_DPAD_LEFT;
	default: return 0;
	}
}

/*!
 * The three button bytes both families share the bit layout of:
 * b1 = hat | square 0x10 | cross 0x20 | circle 0x40 | triangle 0x80,
 * b2 = L1 0x01 | R1 0x02 | (L2 0x04 | R2 0x08 digital, unmapped) |
 *      Create/Share 0x10 | Options 0x20 | L3 0x40 | R3 0x80,
 * b3 = PS 0x01 | (touchpad 0x02, DualSense mute 0x04, unmapped).
 */
static uint32_t
button_bits(uint8_t b1, uint8_t b2, uint8_t b3)
{
	uint32_t b = hat_bits(b1);
	if (b1 & 0x10) {
		b |= GAMEPAD_BTN_X; // square
	}
	if (b1 & 0x20) {
		b |= GAMEPAD_BTN_A; // cross
	}
	if (b1 & 0x40) {
		b |= GAMEPAD_BTN_B; // circle
	}
	if (b1 & 0x80) {
		b |= GAMEPAD_BTN_Y; // triangle
	}
	if (b2 & 0x01) {
		b |= GAMEPAD_BTN_SHOULDER_LEFT;
	}
	if (b2 & 0x02) {
		b |= GAMEPAD_BTN_SHOULDER_RIGHT;
	}
	if (b2 & 0x10) {
		b |= GAMEPAD_BTN_VIEW; // Create (DualSense) / Share (DualShock 4)
	}
	if (b2 & 0x20) {
		b |= GAMEPAD_BTN_MENU; // Options
	}
	if (b2 & 0x40) {
		b |= GAMEPAD_BTN_THUMB_LEFT;
	}
	if (b2 & 0x80) {
		b |= GAMEPAD_BTN_THUMB_RIGHT;
	}
	if (b3 & 0x01) {
		b |= GAMEPAD_BTN_GUIDE; // PS
	}
	return b;
}

static void
fill(const uint8_t sticks[4], uint8_t l2, uint8_t r2, uint8_t b1, uint8_t b2, uint8_t b3, struct gamepad_state *out)
{
	out->connected = true;
	out->buttons = button_bits(b1, b2, b3);
	out->left_x = stick_byte(sticks[0]);
	out->left_y = stick_byte_y(sticks[1]);
	out->right_x = stick_byte(sticks[2]);
	out->right_y = stick_byte_y(sticks[3]);
	out->left_trigger = (float)l2 / 255.0f;
	out->right_trigger = (float)r2 / 255.0f;
}


/*
 *
 * Exported functions.
 *
 */

enum gamepad_hid_pad
gamepad_hid_match(uint16_t vid, uint16_t pid)
{
	if (vid != GAMEPAD_HID_VID_SONY) {
		return GAMEPAD_HID_PAD_NONE;
	}
	switch (pid) {
	case 0x0CE6: return GAMEPAD_HID_PAD_DUALSENSE;
	case 0x0DF2: return GAMEPAD_HID_PAD_DUALSENSE_EDGE;
	case 0x05C4:
	case 0x09CC: return GAMEPAD_HID_PAD_DUALSHOCK4;
	default: return GAMEPAD_HID_PAD_NONE;
	}
}

const char *
gamepad_hid_pad_name(enum gamepad_hid_pad pad)
{
	switch (pad) {
	case GAMEPAD_HID_PAD_DUALSENSE: return "DualSense";
	case GAMEPAD_HID_PAD_DUALSENSE_EDGE: return "DualSense Edge";
	case GAMEPAD_HID_PAD_DUALSHOCK4: return "DualShock 4";
	default: return "none";
	}
}

bool
gamepad_hid_parse_dualsense(const uint8_t *buf, size_t len, struct gamepad_state *out)
{
	if (buf == NULL || out == NULL || len < 1) {
		return false;
	}
	size_t o; // offset of LX
	if (buf[0] == 0x01) {
		o = 1; // USB
	} else if (buf[0] == 0x31) {
		o = 2; // Bluetooth: one sequence byte before the payload
	} else {
		return false;
	}
	// LX LY RX RY L2 R2 seq b1 b2 b3 — ten bytes from LX.
	if (len < o + 10) {
		return false;
	}
	const uint8_t *p = buf + o;
	fill(p, p[4], p[5], p[7], p[8], p[9], out);
	return true;
}

bool
gamepad_hid_parse_dualshock4(const uint8_t *buf, size_t len, struct gamepad_state *out)
{
	if (buf == NULL || out == NULL || len < 1) {
		return false;
	}
	size_t o; // offset of LX
	if (buf[0] == 0x01) {
		o = 1; // USB, or Bluetooth's simple report
	} else if (buf[0] == 0x11) {
		o = 3; // Bluetooth full report: two header bytes before the payload
	} else {
		return false;
	}
	// LX LY RX RY b1 b2 b3 L2 R2 — nine bytes from LX.
	if (len < o + 9) {
		return false;
	}
	const uint8_t *p = buf + o;
	fill(p, p[7], p[8], p[4], p[5], p[6], out);
	return true;
}

bool
gamepad_hid_parse(enum gamepad_hid_pad pad, bool bluetooth, const uint8_t *buf, size_t len, struct gamepad_state *out)
{
	switch (pad) {
	case GAMEPAD_HID_PAD_DUALSENSE:
	case GAMEPAD_HID_PAD_DUALSENSE_EDGE:
		if (bluetooth && buf != NULL && len >= 1 && buf[0] == 0x01) {
			// Bluetooth simple mode: DualShock-4-shaped report.
			return gamepad_hid_parse_dualshock4(buf, len, out);
		}
		return gamepad_hid_parse_dualsense(buf, len, out);
	case GAMEPAD_HID_PAD_DUALSHOCK4: return gamepad_hid_parse_dualshock4(buf, len, out);
	default: return false;
	}
}

uint32_t
gamepad_hid_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

static uint8_t
motor_byte(float v)
{
	if (!(v == v) || v <= 0.0f) {
		return 0;
	}
	if (v >= 1.0f) {
		return 255;
	}
	return (uint8_t)(v * 255.0f + 0.5f);
}

//! Bluetooth output reports end in a CRC-32 over the 0xA2 "output" header byte + the report.
static void
bt_crc_trailer(uint8_t *buf, size_t len)
{
	const uint8_t seed = 0xA2;
	uint32_t crc = gamepad_hid_crc32(0, &seed, 1);
	crc = gamepad_hid_crc32(crc, buf, len - 4);
	buf[len - 4] = (uint8_t)(crc & 0xFF);
	buf[len - 3] = (uint8_t)((crc >> 8) & 0xFF);
	buf[len - 2] = (uint8_t)((crc >> 16) & 0xFF);
	buf[len - 1] = (uint8_t)((crc >> 24) & 0xFF);
}

size_t
gamepad_hid_build_rumble(enum gamepad_hid_pad pad,
                         bool bluetooth,
                         uint8_t seq,
                         float low_frequency,
                         float high_frequency,
                         uint8_t *buf,
                         size_t cap)
{
	const uint8_t low = motor_byte(low_frequency);
	const uint8_t high = motor_byte(high_frequency);

	switch (pad) {
	case GAMEPAD_HID_PAD_DUALSENSE:
	case GAMEPAD_HID_PAD_DUALSENSE_EDGE: {
		const size_t len = bluetooth ? 78 : 48;
		if (buf == NULL || cap < len) {
			return 0;
		}
		memset(buf, 0, len);
		size_t c; // start of the common output block
		if (bluetooth) {
			buf[0] = 0x31;
			buf[1] = (uint8_t)((seq & 0x0F) << 4);
			buf[2] = 0x10; // output tag
			c = 3;
		} else {
			buf[0] = 0x02;
			c = 1;
		}
		// valid_flag0: rumble emulation (0x01) + haptics select (0x02). Firmware
		// 2.21+ moved the emulation enable to valid_flag2 bit 0x04 (block +38);
		// set both, the unknown bit is ignored on older firmware.
		buf[c + 0] = 0x03;
		buf[c + 2] = high; // right motor: high frequency
		buf[c + 3] = low;  // left motor: low frequency
		buf[c + 38] = 0x04;
		if (bluetooth) {
			bt_crc_trailer(buf, len);
		}
		return len;
	}
	case GAMEPAD_HID_PAD_DUALSHOCK4: {
		const size_t len = bluetooth ? 78 : 32;
		if (buf == NULL || cap < len) {
			return 0;
		}
		memset(buf, 0, len);
		if (bluetooth) {
			buf[0] = 0x11;
			buf[1] = 0xC0; // HID output + CRC present
			buf[3] = 0x01; // valid: motors only
			buf[6] = high; // small / right
			buf[7] = low;  // big / left
			bt_crc_trailer(buf, len);
		} else {
			buf[0] = 0x05;
			buf[1] = 0x01; // valid: motors only (light bar untouched)
			buf[4] = high;
			buf[5] = low;
		}
		return len;
	}
	default: return 0;
	}
}
