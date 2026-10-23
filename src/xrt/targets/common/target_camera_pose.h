// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime's answer to `get_camera_pose` — ONE implementation
 *         behind both the viewer-tracking and the input host ifaces, so the
 *         two classes cannot disagree about where a camera is.
 *
 * Resolution (`xrt/xrt_camera_pose.h`, spec `tracking-provider-discovery.md`
 * §6): display-declared > user-calibration > placement-hint > default-prior,
 * over the camera calibration store (`util/u_camera_calib.h`). The panel size
 * the hint and the prior need is the HOME display's (the screen providers
 * serve), published by whichever loader learns it first.
 *
 * @ingroup target_common
 */

#pragma once

#include "xrt/xrt_camera_pose.h"
#include "util/u_camera_calib.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Publish the home display's physical size (metres) and, when known, its
 * screen key. Called from both loaders' `*_set_display_geometry`. Copied.
 */
void
target_camera_pose_set_home_display(float width_m, float height_m, const char *screen_key);

/*!
 * The display-declared pose source: fills @p out (camera pose of
 * @p camera_key on @p screen_key) and returns true when the display declares
 * one. TODO(#2296): the ADR-043 display-declared camera field feeds this; until
 * it exists only tests install a hook.
 */
typedef bool (*target_camera_pose_declared_fn)(const char *camera_key,
                                               const char *screen_key,
                                               struct u_camera_calib_record *out);

//! Install (or with NULL remove) the display-declared source. Test hook until #2296.
void
target_camera_pose_set_declared_hook(target_camera_pose_declared_fn fn);

/*!
 * Resolve @p camera_key on @p screen_key (NULL / "" = the home screen, any
 * stored screen matches). Panel size: @p panel_w_m / @p panel_h_m when > 0,
 * else the published home display. False only when nothing is stored and no
 * panel size is known. Also returns the viewer iris scale (1 = none).
 */
bool
target_camera_pose_resolve(const char *camera_key,
                           const char *screen_key,
                           float panel_w_m,
                           float panel_h_m,
                           struct u_camera_calib_record *out,
                           double *out_iris_scale);

/*!
 * THE host slot (`xrt_camera_get_pose_fn_t`) both host ifaces carry. @p host
 * is ignored: the answer is per process and per user, never per class.
 */
xrt_result_t
target_camera_pose_host_get(const void *host,
                            const char *camera_key,
                            const char *screen_key,
                            struct xrt_camera_pose *inout);

#ifdef __cplusplus
}
#endif
