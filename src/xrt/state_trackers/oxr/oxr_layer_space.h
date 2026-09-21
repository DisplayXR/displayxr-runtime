// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1594 — composition-layer pose into the compositor's frame.
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Re-express a composition-layer pose supplied in some OpenXR space into the
 * frame every native compositor consumes.
 *
 * That frame is the head device's TRACKING-ORIGIN ("root") space — the same
 * one `oxr_session_locate_views()` does its view math in (#1370, see the long
 * comment in `oxr_session.c`). `oxr_space_locate_device()` on the head returns
 * that origin located in the layer's space (the overseer links a device to its
 * origin space, not to its pose), so inverting it and composing the app's pose
 * on top lands the layer beside the projection views.
 *
 * Pure function of its inputs: no session, no device, no clock — which is what
 * makes `tests_oxr_layer_space` able to pin it without hardware.
 *
 * @param T_space_xdev  Head tracking origin located in the layer's space, as
 *                      returned by @ref oxr_space_locate_device.
 * @param T_space_layer The app's layer pose, in that same space. Must already
 *                      have a normalised orientation.
 * @param[out] out_pose The layer pose in the head tracking-origin frame.
 * @return false if @p T_space_xdev carries no usable pose, in which case
 *         @p out_pose is untouched.
 *
 * @ingroup oxr_main
 */
bool
oxr_layer_pose_in_xdev_frame(const struct xrt_space_relation *T_space_xdev,
                             const struct xrt_pose *T_space_layer,
                             struct xrt_pose *out_pose);

#ifdef __cplusplus
}
#endif
