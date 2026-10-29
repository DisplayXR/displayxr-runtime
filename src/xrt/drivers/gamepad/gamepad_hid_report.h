// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Sony pad HID reports — pure parsers and output-report builders.
 *
 * The byte layouts of the DualSense (and DualSense Edge) and DualShock 4
 * input / output reports, as pure functions of a buffer: no OS, no device,
 * no thread. The Windows HID backend (`gamepad_backend_hid_win.c`) feeds them
 * what `ReadFile` returns; `tests/tests_gamepad_hid_report.cpp` feeds them
 * synthetic reports. Nothing here is Windows-specific, so an evdev / hidraw
 * backend can reuse it.
 *
 * Every parser maps onto @ref gamepad_state's Xbox-layout convention (the
 * one the XInput and GameController backends produce): cross = A,
 * circle = B, square = X, triangle = Y, L1 / R1 = shoulders, L3 / R3 =
 * thumb clicks, Create / Share = View, Options = Menu, PS = Guide, the hat
 * = the d-pad; sticks −1..+1 with +X right and +Y UP (HID reports 0 = up,
 * so Y is inverted), triggers 0..1. Buttons with no Xbox counterpart (the
 * touchpad click, mute, the digital L2 / R2 bits, the Edge's Fn and back
 * paddles) are not mapped.
 *
 * @ingroup drv_gamepad
 */

#pragma once

#include "gamepad_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Sony's USB vendor id.
#define GAMEPAD_HID_VID_SONY 0x054C

//! The pad families this unit knows the reports of.
enum gamepad_hid_pad
{
	GAMEPAD_HID_PAD_NONE = 0,
	GAMEPAD_HID_PAD_DUALSENSE,      //!< 054C:0CE6
	GAMEPAD_HID_PAD_DUALSENSE_EDGE, //!< 054C:0DF2 (same reports as the DualSense)
	GAMEPAD_HID_PAD_DUALSHOCK4,     //!< 054C:05C4 (v1) and 054C:09CC (v2)
};

//! Which family a VID:PID is, or @ref GAMEPAD_HID_PAD_NONE.
enum gamepad_hid_pad
gamepad_hid_match(uint16_t vid, uint16_t pid);

//! Short name of a family for logs ("DualSense", "DualSense Edge", "DualShock 4").
const char *
gamepad_hid_pad_name(enum gamepad_hid_pad pad);

/*!
 * DualSense full input report: USB report 0x01 (LX at byte 1) or Bluetooth
 * report 0x31 (same payload one byte later, LX at byte 2). Fills every field
 * of @p out except `timestamp_ns`; `connected` = true. False (and @p out
 * untouched) for any other report id or a short buffer.
 *
 * Bluetooth's *simple* report 0x01 (sent until the full mode is requested)
 * has the DualShock 4 layout instead — @ref gamepad_hid_parse routes it.
 */
bool
gamepad_hid_parse_dualsense(const uint8_t *buf, size_t len, struct gamepad_state *out);

/*!
 * DualShock 4 input report: USB (and Bluetooth simple) report 0x01 (LX at
 * byte 1) or Bluetooth report 0x11 (LX at byte 3). Same contract as
 * @ref gamepad_hid_parse_dualsense.
 */
bool
gamepad_hid_parse_dualshock4(const uint8_t *buf, size_t len, struct gamepad_state *out);

/*!
 * The router a backend calls: picks the parser for @p pad, and on a
 * Bluetooth DualSense sends report 0x01 (the simple, DualShock-4-shaped one)
 * to the DualShock 4 parser.
 */
bool
gamepad_hid_parse(enum gamepad_hid_pad pad, bool bluetooth, const uint8_t *buf, size_t len, struct gamepad_state *out);

/*!
 * Standard CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) — the
 * checksum Sony's Bluetooth output reports carry. Pass 0 as @p crc to start;
 * chain by passing the previous result.
 */
uint32_t
gamepad_hid_crc32(uint32_t crc, const uint8_t *data, size_t len);

/*!
 * Build the rumble output report for @p pad into @p buf (zeroed first).
 *
 * - DualSense USB: report 0x02, 48 bytes — flags enable rumble emulation,
 *   [3] = right (high-frequency) motor, [4] = left (low-frequency) motor.
 * - DualSense Bluetooth: report 0x31, 78 bytes — sequence tag, 0x10 tag, the
 *   same block from byte 3, CRC-32 over 0xA2 + bytes 0..73 at 74..77.
 * - DualShock 4 USB: report 0x05, 32 bytes — [1] = 0x01 (motors only, so the
 *   light bar is left alone), [4] = small / right, [5] = big / left.
 * - DualShock 4 Bluetooth: report 0x11, 78 bytes — [1] = 0xC0 (HID + CRC),
 *   [3] = 0x01, [6] = small, [7] = big, CRC as above.
 *
 * @p low_frequency / @p high_frequency are 0..1 (NaN = 0, clamped). @p seq is
 * the DualSense Bluetooth sequence counter (low 4 bits used). Returns the
 * report length the pad expects (the backend sends at least that many bytes,
 * padded to the device's output report length), or 0 if @p cap is too small
 * or @p pad unknown.
 */
size_t
gamepad_hid_build_rumble(enum gamepad_hid_pad pad,
                         bool bluetooth,
                         uint8_t seq,
                         float low_frequency,
                         float high_frequency,
                         uint8_t *buf,
                         size_t cap);

#ifdef __cplusplus
}
#endif
