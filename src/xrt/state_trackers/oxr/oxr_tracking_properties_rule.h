// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1631: what `XrSystemTrackingProperties` advertises on a 3D display.
 *
 * `xrGetSystemProperties` used to copy `orientationTracking` /
 * `positionTracking` straight off the HEAD device and nothing else:
 *
 * ```c
 * properties->trackingProperties.orientationTracking = xdev->supported.orientation_tracking;
 * properties->trackingProperties.positionTracking = xdev->supported.position_tracking;
 * ```
 *
 * In a display session the head device is the display driver's, which leaves
 * both false — even when the system is delivering fully tracked controller
 * poses from qwerty / `sim_input` / `net_input` / a hand-tracking provider, all
 * of which set both flags true and were simply never consulted.
 *
 * ### The rule (#1631, option (b))
 *
 * `XrSystemTrackingProperties` describes the SYSTEM, not the head, and the
 * system tracks whenever ANY of these is true:
 *
 *   1. the head device itself tracks — an eye-tracked rig reports this through
 *      the head, so keeping it in the OR is what makes real hardware true "the
 *      head way" as well;
 *   2. the display processor reports an eye-tracking capability
 *      (`xrt_plugin_display_info::supported_eye_tracking_modes != 0`, surfaced
 *      on `xrt_system_compositor_info`) — with a tracked viewer the runtime
 *      delivers tracked VIEW poses even if the head xdev never sets a flag;
 *   3. any tracked ROLE device exists — left/right controller-or-hand, gamepad,
 *      or one of the hand-tracking roles. These are the devices that supply
 *      located poses to `xrLocateSpace` / action poses.
 *
 * A bare sim-display with no eye tracking (`SIM_DISPLAY_FAKE_TRACKING` unset,
 * so `supported_eye_tracking_modes == 0`) and no role devices still reports
 * FALSE — the head really is untracked there and nothing else in the system
 * tracks either. That case is the honest counter-argument on the issue, and it
 * is preserved deliberately rather than papered over.
 *
 * ### The two consumers this flag actually has
 *
 *   - **The OpenXR CTS.** `test_SpaceOffsets.cpp`, `test_InteractiveThrow.cpp`
 *     and `test_ActionPoses.cpp` skip the whole file with "System does not
 *     support orientation or position tracking". A skip is not a failure, but
 *     those cases were never exercised — they run now and pass or fail on their
 *     own merits.
 *   - **WebXR pages.** Some sites read the same flag to decide whether to draw
 *     controllers at all; `qwerty_device.c` sets both flags on its HMD with a
 *     comment saying exactly that. With the rule below the qwerty HMD no longer
 *     has to be the head for the answer to come out right.
 *
 * A pure decision with no runtime dependency, so it is pinned on the host
 * (tests/tests_oxr_tracking_properties_rule.cpp). Same shape as
 * @ref oxr_legacy_mode_rule.h and @ref oxr_view_config_rule.h.
 *
 * @ingroup oxr_main
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Does the display processor track the viewer's eyes?
 *
 * @param supported_eye_tracking_modes `xrt_system_compositor_info::supported_eye_tracking_modes`,
 *        which the plug-in filled from `xrt_plugin_display_info` at instance
 *        create. Zero means "no eye-tracking capability at all" — sim-display's
 *        honest default (#441); `SIM_DISPLAY_FAKE_TRACKING=1` re-enables
 *        MANUAL_BIT and is therefore the hardware-free lever for this term.
 */
static inline bool
oxr_dp_tracks_eyes(uint32_t supported_eye_tracking_modes)
{
	return supported_eye_tracking_modes != 0u;
}

/*!
 * `XrSystemTrackingProperties::orientationTracking`.
 *
 * @param head_orientation        The head xdev's `supported.orientation_tracking`.
 * @param dp_eye_tracking         @ref oxr_dp_tracks_eyes.
 * @param any_role_orientation    Any role device (left, right, gamepad, hand
 *                                trackers) sets `supported.orientation_tracking`.
 */
static inline bool
oxr_system_orientation_tracking(bool head_orientation, bool dp_eye_tracking, bool any_role_orientation)
{
	return head_orientation || dp_eye_tracking || any_role_orientation;
}

/*!
 * `XrSystemTrackingProperties::positionTracking`. The same OR over the POSITION
 * flags — the two are computed independently so a device that tracks
 * orientation only cannot silently claim position.
 */
static inline bool
oxr_system_position_tracking(bool head_position, bool dp_eye_tracking, bool any_role_position)
{
	return head_position || dp_eye_tracking || any_role_position;
}

#ifdef __cplusplus
}
#endif
