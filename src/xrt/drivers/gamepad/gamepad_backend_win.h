// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The two Windows pad readers behind `gamepad_backend_win.c`.
 *
 * Windows has two kinds of pad: XInput ones (Xbox and Xbox-compatible, or
 * anything a remapper wraps — their device node carries `IG_`) and plain HID
 * game controllers (Sony's). `gamepad_backend_win.c` implements the
 * @ref gamepad_backend_poll API by dispatching per device between the two
 * readers declared here; each has the same contract as the matching
 * `gamepad_backend_*` function and is called from the hub's poll thread only.
 *
 * @ingroup drv_gamepad
 */

#pragma once

#include "gamepad_map.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// XInput (gamepad_backend_xinput.c).
bool
gamepad_xinput_init(void);
void
gamepad_xinput_fini(void);
void
gamepad_xinput_poll(struct gamepad_state *out);
void
gamepad_xinput_rumble(float low_frequency, float high_frequency);

// HID — Sony DualSense / DualSense Edge / DualShock 4 (gamepad_backend_hid_win.c).
bool
gamepad_hid_init(void);
void
gamepad_hid_fini(void);
/*!
 * Fill @p out from the open pad, or (re)scan for one — no faster than once a
 * second while none is open — and report `connected` = false.
 */
void
gamepad_hid_poll(struct gamepad_state *out);
void
gamepad_hid_rumble(float low_frequency, float high_frequency);
//! "hid:dualsense" / "hid:dualshock4" for the open pad, "none" when none is.
const char *
gamepad_hid_name(void);

#ifdef __cplusplus
}
#endif
