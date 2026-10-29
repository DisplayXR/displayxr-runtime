// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The per-platform pad reader behind the gamepad provider.
 *
 * One implementation is compiled per OS — `gamepad_backend_win.c` (Windows:
 * per-device dispatch between XInput, `gamepad_backend_xinput.c`, and HID for
 * Sony DualSense / DualShock 4 pads, `gamepad_backend_hid_win.c`; XInput wins
 * whenever it has a pad), `gamepad_backend_gc.m` (macOS, GameController.framework),
 * `gamepad_backend_stub.c` (everything else: no pads, a TODO for evdev) — and
 * every function here is called from the hub's poll thread only, so a
 * backend needs no locking of its own.
 *
 * @ingroup drv_gamepad
 */

#pragma once

#include "gamepad_map.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Can this build read a pad at all? False on a platform with no backend
 * (the stub): the provider then DECLINES at probe, so it is never resident
 * and never a role candidate. True does not mean a pad is attached — that is
 * @ref gamepad_backend_poll's `connected`.
 */
bool
gamepad_backend_available(void);

//! One-time setup on the poll thread. False = the platform API refused; the provider reports ABSENT for good.
bool
gamepad_backend_init(void);

//! Teardown on the poll thread.
void
gamepad_backend_fini(void);

/*!
 * Read the first attached pad into @p out (every field, `connected` included;
 * `timestamp_ns` is stamped by the caller). Cheap when nothing is attached —
 * the hub polls a disconnected backend slowly, so hot-plug costs nothing.
 */
void
gamepad_backend_poll(struct gamepad_state *out);

//! Rumble, 0..1 per motor; a backend without haptics ignores it.
void
gamepad_backend_rumble(float low_frequency, float high_frequency);

/*!
 * A short name for the log: "GameController" / "stub", or on Windows the
 * reader that is live — "xinput", "hid:dualsense", "hid:dualshock4", "none".
 */
const char *
gamepad_backend_name(void);

#ifdef __cplusplus
}
#endif
