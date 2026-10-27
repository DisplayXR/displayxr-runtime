// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to the gamepad input provider.
 *
 * The gamepad provider is an input-provider plug-in (ADR-034 / ADR-054, the
 * existing class — not a new one) that turns the one controller every desk
 * already has into three things the runtime can use on a flat 3D display,
 * where there is no tracked head and no tracked hands:
 *
 *   1. a `XRT_DEVICE_TYPE_GAMEPAD` device on the `microsoft/xbox_controller`
 *      profile, filling the `/user/gamepad` role the action system already
 *      knows (`xrt_system_roles::gamepad`) — the first provider to do so;
 *   2. a `XRT_DEVICE_TYPE_NAVIGATION` device so a present pad drives the RIG
 *      role (ADR-034 Amendment 4): left stick = translate, right stick =
 *      look, triggers = up / down, right-stick click = recenter;
 *   3. optionally, an emulated left / right `khr/simple_controller` pair
 *      driven by the pad's buttons, so HANDLE apps — whose own window owns
 *      the keyboard, which is why the qwerty pair is withheld from them
 *      (`oxr_input.c`, `has_external_window`) — get `xrSyncActions`
 *      controllers at all. A pad is focus-independent, so there is no such
 *      conflict here.
 *
 * All three read ONE latched sample from the hub (`gamepad_hub.c`), which
 * owns the poll thread and the platform backend (`gamepad_backend.h`), and
 * every "what does this input mean" decision goes through the mapping table
 * (`gamepad_map.h`). Presence (`get_presence`) follows the backend's
 * `connected` with a hot-plug poll, so an unplugged pad hands every role
 * back — the rig to the fly camera, the hands to qwerty, the gamepad role to
 * -1 — and a re-plugged one takes them again, with no app restart.
 *
 * Ships as `DisplayXR-Gamepad` (ProbeOrder 150 by convention: below a real
 * tracking vendor at 50, above sim-input at 200).
 *
 * @author David Fattal
 * @ingroup drv_gamepad
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_input_plugin.h"

#include "gamepad_map.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct gamepad_hub;

//! What the hub is built with: the mapping table and its numeric knobs.
struct gamepad_hub_config
{
	const struct gamepad_map_table *table;
	struct gamepad_map_config map;

	/*!
	 * "Last-used input wins" (the `IDLE_TIMEOUT_S` setting): the pad is
	 * PRESENT only while connected AND touched within this many seconds;
	 * untouched longer, it reports ABSENT and every role it held returns
	 * to the floor (fly camera / qwerty) until the next touch. 0 = the
	 * connected-only rule (a plugged-in pad holds its roles indefinitely).
	 */
	float idle_timeout_s;
};

/*!
 * Create the hub and start its poll thread. Returns NULL when the platform
 * backend cannot be brought up at all. The hub starts with ONE reference
 * owned by the caller; every device created on it takes another
 * (@ref gamepad_hub_retain) and drops it in its own destroy
 * (@ref gamepad_hub_release); the last release stops the thread and frees
 * the hub.
 */
struct gamepad_hub *
gamepad_hub_create(const struct gamepad_hub_config *cfg);

void
gamepad_hub_retain(struct gamepad_hub *hub);

void
gamepad_hub_release(struct gamepad_hub *hub);

//! The live hub (NULL before create_devices and after the last device is destroyed) — for `get_presence`.
struct gamepad_hub *
gamepad_hub_current(void);

/*!
 * The arbiter's question — is the pad there RIGHT NOW? PRESENT only while a
 * pad is connected AND, with a non-zero `idle_timeout_s`, was touched within
 * that window (@ref gamepad_map_recently_active); ABSENT otherwise; UNKNOWN
 * before the first poll.
 */
enum xrt_input_provider_presence
gamepad_hub_presence(struct gamepad_hub *hub);

//! The latest latched sample (zeros + `connected = false` with no pad).
void
gamepad_hub_snapshot(struct gamepad_hub *hub, struct gamepad_state *out);

/*!
 * N(t): the integrated navigation pose in the provider's frame F,
 * extrapolated with the last velocity for a bounded horizon beyond the
 * latch time. @p out_valid is false until the backend produced a sample.
 * @p out_recenter_ns is the durable recenter timestamp (0 = never).
 */
void
gamepad_hub_nav_sample(struct gamepad_hub *hub,
                       int64_t at_timestamp_ns,
                       struct xrt_pose *out_pose,
                       bool *out_valid,
                       int64_t *out_recenter_ns);

/*!
 * Rumble for @p duration_ns (the poll thread stops it when the deadline
 * passes; 0 = stop now). @p low / @p high are 0..1 per motor.
 */
void
gamepad_hub_rumble(struct gamepad_hub *hub, float low, float high, int64_t duration_ns);


/*
 *
 * Devices.
 *
 */

//! The `microsoft/xbox_controller` gamepad device (`XRT_DEVICE_TYPE_GAMEPAD`).
struct xrt_device *
gamepad_create_pad_device(struct gamepad_hub *hub);

//! The navigation device (`XRT_DEVICE_TYPE_NAVIGATION`): NAVIGATION_POSE + the durable RECENTER level.
struct xrt_device *
gamepad_create_navigation(struct gamepad_hub *hub);

/*!
 * One emulated `khr/simple_controller` (@p type = LEFT or RIGHT hand) at a
 * fixed display-plane-relative pose (`XRT_TRACKING_TYPE_RIG_LOCAL`, as a
 * navigating provider's controllers must be — spec §4b.4), with select /
 * menu from the mapping table and vibration routed to the pad's motors.
 */
struct xrt_device *
gamepad_create_emulated_controller(struct gamepad_hub *hub, enum xrt_device_type type);

//! How the Touch pair (`EMULATE_PROFILE` = touch) moves its hands.
struct gamepad_touch_config
{
	float deadzone;             //!< radial stick deadzone, as @ref gamepad_map_config::deadzone
	float hand_speed_mps;       //!< hand speed at full stick deflection (`HAND_SPEED`)
	struct xrt_vec3 right_home; //!< right hand's rest point, rig-local metres; the left hand mirrors X
};

/*!
 * One emulated `oculus/touch_controller` (@p type = LEFT or RIGHT hand)
 * whose pose the pad moves (@ref gamepad_map_touch_hand), rig-local like
 * the simple pair, with the full Touch button set and haptics.
 */
struct xrt_device *
gamepad_create_touch_controller(struct gamepad_hub *hub,
                                enum xrt_device_type type,
                                const struct gamepad_touch_config *cfg);

#ifdef __cplusplus
}
#endif
