// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Linux evdev gamepad events -> @ref gamepad_state, with no I/O.
 *
 * The desktop-Linux reader (`gamepad_backend_evdev.c`) opens
 * `/dev/input/event*` and feeds every event it reads through
 * @ref gamepad_evdev_apply. Everything that decides what a code MEANS lives
 * here, pure, so it is unit-tested on every platform: the kernel's event
 * codes are stable ABI, so this file carries them itself (checked against
 * `<linux/input-event-codes.h>` where that header exists).
 *
 * ## Which pad (SteamOS)
 *
 * On a Steam Deck, or any machine running Steam, there are two candidates for
 * one physical pad: the raw device the kernel driver exposes (hid-steam for
 * the Deck's built-in controls, xpad / hid-playstation / ... for others) and
 * Steam Input's virtual pad (uinput, 28DE:11FF), which carries the user's
 * per-game remapping. @ref gamepad_evdev_rank prefers the virtual pad, so the
 * app sees what Steam's controller settings say. When Steam is not running,
 * the raw device is used.
 *
 * ## Face-button layout
 *
 * The kernel's gamepad spec names face buttons by POSITION (BTN_NORTH = top),
 * but xpad — and every driver imitating it, Steam's virtual pad and hid-steam
 * included — predates that and emits code 0x133 (BTN_X == BTN_NORTH) for the
 * LEFT button and 0x134 (BTN_Y == BTN_WEST) for the TOP one. The two
 * conventions disagree exactly on X/Y, so the reader picks a layout per device
 * (@ref gamepad_evdev_layout_for). Both map to the Xbox POSITIONS the rest of
 * the provider uses (A bottom, B right, X left, Y top), as XInput and
 * GameController do.
 *
 * ## Axes
 *
 * Sticks ABS_X/ABS_Y and ABS_RX/ABS_RY, normalised by each axis's own
 * min/max, Y flipped to up-positive like XInput. Triggers ABS_Z/ABS_RZ (xpad,
 * hid-playstation, Steam's virtual pad), else ABS_HAT2Y/ABS_HAT2X (hid-steam),
 * else ABS_BRAKE/ABS_GAS, else the digital BTN_TL2/BTN_TR2. D-pad from
 * BTN_DPAD_* keys or the ABS_HAT0X/ABS_HAT0Y hat. Old hid-sony (DualShock 4
 * before hid-playstation took it over) put the right stick on ABS_Z/ABS_RZ and
 * is not handled.
 *
 * @ingroup drv_gamepad
 */

#pragma once

#include "gamepad_map.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Kernel event codes used here (include/uapi/linux/input-event-codes.h).
 */
#define GP_EV_SYN 0x00
#define GP_EV_KEY 0x01
#define GP_EV_ABS 0x03
#define GP_SYN_DROPPED 3

#define GP_BTN_SOUTH 0x130 //!< BTN_A
#define GP_BTN_EAST 0x131  //!< BTN_B
#define GP_BTN_0x133 0x133 //!< BTN_NORTH == BTN_X: top (spec) or left (xpad)
#define GP_BTN_0x134 0x134 //!< BTN_WEST == BTN_Y: left (spec) or top (xpad)
#define GP_BTN_TL 0x136
#define GP_BTN_TR 0x137
#define GP_BTN_TL2 0x138
#define GP_BTN_TR2 0x139
#define GP_BTN_SELECT 0x13a
#define GP_BTN_START 0x13b
#define GP_BTN_MODE 0x13c
#define GP_BTN_THUMBL 0x13d
#define GP_BTN_THUMBR 0x13e
#define GP_BTN_DPAD_UP 0x220
#define GP_BTN_DPAD_DOWN 0x221
#define GP_BTN_DPAD_LEFT 0x222
#define GP_BTN_DPAD_RIGHT 0x223

#define GP_ABS_X 0x00
#define GP_ABS_Y 0x01
#define GP_ABS_Z 0x02
#define GP_ABS_RX 0x03
#define GP_ABS_RY 0x04
#define GP_ABS_RZ 0x05
#define GP_ABS_GAS 0x09
#define GP_ABS_BRAKE 0x0a
#define GP_ABS_HAT0X 0x10
#define GP_ABS_HAT0Y 0x11
#define GP_ABS_HAT2X 0x14
#define GP_ABS_HAT2Y 0x15

//! ABS codes below this are tracked (every one used above is).
#define GAMEPAD_EVDEV_ABS_COUNT 0x16

//! USB vendor ids that decide the layout and the rank.
#define GAMEPAD_EVDEV_VID_VALVE 0x28DE
#define GAMEPAD_EVDEV_VID_MICROSOFT 0x045E
//! Steam Input's virtual pad (uinput).
#define GAMEPAD_EVDEV_PID_STEAM_VIRTUAL 0x11FF

//! What code 0x133 / 0x134 mean on this device (see the file comment).
enum gamepad_evdev_layout
{
	GAMEPAD_EVDEV_LAYOUT_SPEC = 0, //!< 0x133 top (Y), 0x134 left (X): hid-playstation, hid-nintendo, ...
	GAMEPAD_EVDEV_LAYOUT_XPAD,     //!< 0x133 left (X), 0x134 top (Y): xpad, Steam virtual pad, hid-steam
};

/*!
 * The layout for a device: xpad when its kernel driver is "xpad" (any vendor:
 * third-party Xbox-protocol pads) or its vendor is Microsoft or Valve; the
 * spec otherwise. @p driver may be NULL or empty (unknown).
 */
enum gamepad_evdev_layout
gamepad_evdev_layout_for(uint16_t vendor, const char *driver);

/*!
 * How much to prefer a device; the reader opens the highest, lowest event
 * number on a tie. Only called for devices that ARE pads (they report
 * BTN_SOUTH, ABS_X and ABS_Y).
 *
 * - 3: Steam Input's virtual pad — carries the user's Steam remapping.
 * - 2: any other pad.
 * - 1: a raw Valve device (the Deck's hid-steam controls, a Steam
 *      Controller): the same hands as the virtual pad whenever Steam runs.
 */
int
gamepad_evdev_rank(uint16_t vendor, uint16_t product);

//! One device's accumulated state.
struct gamepad_evdev_pad
{
	enum gamepad_evdev_layout layout;
	bool abs_present[GAMEPAD_EVDEV_ABS_COUNT];
	int32_t abs_min[GAMEPAD_EVDEV_ABS_COUNT];
	int32_t abs_max[GAMEPAD_EVDEV_ABS_COUNT];
	int32_t abs_value[GAMEPAD_EVDEV_ABS_COUNT];
	//! Buttons held, from EV_KEY (the hat's d-pad is folded in by @ref gamepad_evdev_fill).
	uint32_t key_buttons;
	bool key_tl2, key_tr2; //!< digital triggers
};

//! Reset @p pad for a device with @p layout. Every axis absent, nothing held.
void
gamepad_evdev_pad_init(struct gamepad_evdev_pad *pad, enum gamepad_evdev_layout layout);

//! The device has ABS @p code with this range (EVIOCGABS) and current @p value.
void
gamepad_evdev_set_abs(struct gamepad_evdev_pad *pad, uint16_t code, int32_t min, int32_t max, int32_t value);

//! The provider button EV_KEY @p code is under @p layout; 0 for none.
uint32_t
gamepad_evdev_button_for_key(uint16_t code, enum gamepad_evdev_layout layout);

/*!
 * Fold one event into @p pad. EV_KEY for a mapped code sets/clears its button
 * (value 0 = up, 1 = down, 2 = autorepeat = down); EV_ABS for a tracked code
 * stores the value. Everything else is ignored, including SYN_DROPPED — the
 * reader resyncs the whole device on that.
 */
void
gamepad_evdev_apply(struct gamepad_evdev_pad *pad, uint16_t type, uint16_t code, int32_t value);

//! @p v on [min, max] -> −1..1 about the centre, clamped. A degenerate range is 0.
float
gamepad_evdev_norm_stick(int32_t v, int32_t min, int32_t max);

//! @p v on [min, max] -> 0..1, clamped. A degenerate range is 0.
float
gamepad_evdev_norm_trigger(int32_t v, int32_t min, int32_t max);

//! The pad as a @ref gamepad_state: `connected` = true, `timestamp_ns` untouched.
void
gamepad_evdev_fill(const struct gamepad_evdev_pad *pad, struct gamepad_state *out);

#ifdef __cplusplus
}
#endif
