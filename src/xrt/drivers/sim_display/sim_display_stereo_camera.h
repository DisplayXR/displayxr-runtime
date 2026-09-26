// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display FAKE stereo camera provider (ADR-043) — the plug-in
 *         iface camera slots, filled under SIM_DISPLAY_FAKE_STEREO_CAMERA=1.
 *
 * Knobs (read by the process that loads the plug-in — the SERVICE):
 *   SIM_DISPLAY_FAKE_STEREO_CAMERA=1                 advertise one camera
 *   SIM_DISPLAY_FAKE_STEREO_CAMERA_SIZE=WxH          per-eye size (default 640x480)
 *   SIM_DISPLAY_FAKE_STEREO_CAMERA_FPS=N             source rate (default 30)
 *   SIM_DISPLAY_FAKE_STEREO_CAMERA_FORMAT=gray8|nv12|bgra   native format (default gray8)
 *   SIM_DISPLAY_FAKE_STEREO_CAMERA_SUSPEND_PERIOD_MS=N      square-wave SUSPENDED/AVAILABLE
 *   SIM_DISPLAY_FAKE_STEREO_CAMERA_DISTORT=1         R2: a RAW distorted pair (see below)
 *
 * The camera: SHARED_WITH_EYE_TRACKING | USER_FACING | CALIBRATED |
 * NATIVELY_RECTIFIED (+ MONOCHROME for gray8), 68 deg horizontal FOV per eye,
 * 50 mm baseline, background at 2.0 m and a bar at 0.6 m — at 640 px per eye
 * that is fx = 474.4 px and disparities of 12 px and 40 px.
 *
 * With _DISTORT=1 the same scene is seen through two raw cameras with known
 * ground truth (per-eye intrinsics, RADTAN5 barrel lenses, 0.7 deg relative
 * pitch = ~6 px of vertical misalignment, 0.9 deg relative roll, 0.5 deg yaw;
 * sim_stereo_camera_truth_init), the calibration slot returns that truth, and
 * NATIVELY_RECTIFIED is dropped — so RECTIFIED output exercises the service's
 * rectifier (u_stereo_rectify). After rectification rows align and the
 * disparities are f_rect * B / Z (f_rect: `displayxr-cli camera calib <id>
 * --rectified`).
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include "xrt/xrt_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t
sim_display_stereo_camera_enumerate(struct xrt_plugin_instance *inst,
                                    uint32_t capacity,
                                    struct xrt_plugin_stereo_camera_info *out);

xrt_result_t
sim_display_stereo_camera_get_calibration(struct xrt_plugin_instance *inst,
                                          uint32_t index,
                                          struct xrt_plugin_stereo_camera_calibration *out);

xrt_result_t
sim_display_stereo_camera_open(struct xrt_plugin_instance *inst,
                               uint32_t index,
                               struct xrt_plugin_stereo_camera **out_cam);

uint32_t
sim_display_stereo_camera_wait_frame(struct xrt_plugin_stereo_camera *cam,
                                     int64_t timeout_ns,
                                     struct xrt_plugin_stereo_camera_frame *out);

void
sim_display_stereo_camera_release_frame(struct xrt_plugin_stereo_camera *cam);

void
sim_display_stereo_camera_close(struct xrt_plugin_stereo_camera *cam);

#ifdef __cplusplus
}
#endif
