// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_input's hand/controller SCRIPT: where the simulated hands are and
 *         what their fingers do, as pure functions of the requested timestamp.
 *
 * Split out of `sim_input_device.c` (#2191) so the placement and the pinch
 * policy can be unit-tested with no device, no plug-in and no display
 * (`tests/tests_sim_input_hands.cpp`).
 *
 * **Placement (#2191).** The hands are scripted relative to the DISPLAY
 * PLANE: origin at the display centre, +X right, +Y up, +Z toward the viewer
 * (the `XRT_TRACKING_TYPE_RIG_LOCAL` frame of input-provider-discovery.md
 * §4b.4). Each hand's root rides a small circle centred
 * `HAND_DISTANCE_M` in front of the glass and @ref SIM_INPUT_HAND_LATERAL_M
 * to its side, so a default display rig shows both hands without any
 * app-side offset. The device publishes these poses on a RIG_LOCAL origin,
 * which the builder anchors at the display plane (`u_builders.c`).
 *
 * @ingroup drv_sim_input
 */

#pragma once

#include "xrt/xrt_defines.h"

#include "util/u_hand_tracking.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Radius of each hand's circle (metres).
#define SIM_INPUT_CIRCLE_RADIUS_M 0.04f
//! One revolution of the circle (seconds).
#define SIM_INPUT_CIRCLE_PERIOD_S 4.0
//! Finger-curl wave period (#825 Tier 2): a full open→fist→open cycle.
#define SIM_INPUT_CURL_PERIOD_S 3.0
//! Circle centre's distance left/right of the display centre (metres).
#define SIM_INPUT_HAND_LATERAL_M 0.12f
//! Circle centre's height relative to the display centre (metres): slightly below.
#define SIM_INPUT_HAND_HEIGHT_M (-0.03f)
//! Default `HAND_DISTANCE_M`: circle centre's distance in front of the glass.
#define SIM_INPUT_HAND_DISTANCE_DEFAULT_M 0.25f

//! `HANDS` setting: which hands are tracked.
enum sim_input_hands_mode
{
	SIM_INPUT_HANDS_BOTH = 0, //!< default
	SIM_INPUT_HANDS_LEFT,
	SIM_INPUT_HANDS_RIGHT,
	SIM_INPUT_HANDS_NONE,
};

//! `PINCH` setting: what the fingers do.
enum sim_input_pinch_mode
{
	SIM_INPUT_PINCH_WAVE = 0, //!< default: the #825 curl wave (open→fist→open every 3 s)
	SIM_INPUT_PINCH_HOLD,     //!< thumb + index tips held pinched, other fingers relaxed
	SIM_INPUT_PINCH_OPEN,     //!< every finger relaxed, no pinch ever
};

/*!
 * Thumb / index curl that @ref SIM_INPUT_PINCH_HOLD holds: the pair that
 * brings `u_hand_sim_simulate_for_valve_index_knuckles`'s thumb tip and index
 * tip closest together (searched over the curl square; pinned by
 * tests_sim_input_hands — a 6.5 mm tip gap; under 1.5 cm is required).
 */
#define SIM_INPUT_PINCH_HOLD_THUMB_CURL 0.94f
#define SIM_INPUT_PINCH_HOLD_INDEX_CURL 1.0f
//! Curl of the relaxed (non-pinching) fingers in HOLD / OPEN.
#define SIM_INPUT_RELAXED_CURL 0.1f

/*!
 * Parse a `HANDS` value (`both` | `left` | `right` | `none`, case-insensitive).
 * Returns false (leaving @p out untouched) for anything else.
 */
bool
sim_input_parse_hands(const char *value, enum sim_input_hands_mode *out);

/*!
 * Parse a `PINCH` value (`wave` | `hold` | `open`, case-insensitive).
 * Returns false (leaving @p out untouched) for anything else.
 */
bool
sim_input_parse_pinch(const char *value, enum sim_input_pinch_mode *out);

//! Is the @p left (else right) hand tracked under @p mode?
bool
sim_input_hand_enabled(enum sim_input_hands_mode mode, bool left);

/*!
 * The circle centre of one hand in the display-plane (RIG_LOCAL) frame.
 * @p distance_m <= 0 or non-finite falls back to the default.
 */
struct xrt_pose
sim_input_hand_center(bool left, float distance_m);

/*!
 * The scripted root/grip relation at @p at_timestamp_ns: @p center plus a
 * circle of @ref SIM_INPUT_CIRCLE_RADIUS_M in the display plane's XY,
 * phase-shifted by @p phase_rad. Pure function of the timestamp, velocity
 * included, so prediction for any requested time is exact.
 */
void
sim_input_hand_relation(const struct xrt_pose *center,
                        double phase_rad,
                        int64_t at_timestamp_ns,
                        struct xrt_space_relation *out_relation);

//! The finger curls at @p at_timestamp_ns under @p mode (pure).
void
sim_input_hand_curls(enum sim_input_pinch_mode mode,
                     double phase_rad,
                     int64_t at_timestamp_ns,
                     struct u_hand_tracking_curl_values *out_curls);

#ifdef __cplusplus
}
#endif
