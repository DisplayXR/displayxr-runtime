// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display webcam eye tracking (#1855): the worker — one thread
 *         that opens the camera, runs the face estimator on every frame at
 *         the camera rate, maps + smooths the result and leaves the newest
 *         state for the display processors (latest wins).
 *
 * The capture backend and the estimator are injected, so tests run the
 * whole loop on the synthetic UVC backend with a fake estimator and never
 * touch a camera. Device enumeration, mode listing and open all happen ON
 * the worker: creating a tracker never blocks the caller (a compositor
 * thread). Sampling takes a mutex the worker holds only to fold one result
 * into a small struct.
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include "sim_display_webcam_tracking.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_stereo_uvc_backend;
struct sim_face_estimator;
struct sim_webcam_tracker;

/*!
 * Start a tracker. @p backend is copied; @p est is BORROWED and must outlive
 * the tracker. @p device_sel as @ref sim_webcam_select_device. Returns NULL
 * only on bad arguments / allocation / thread failure; a missing camera shows
 * up as a tracker that never tracks.
 */
struct sim_webcam_tracker *
sim_webcam_tracker_create(const struct sim_webcam_config *cfg,
                          const struct u_stereo_uvc_backend *backend,
                          const char *device_sel,
                          struct sim_face_estimator *est,
                          float screen_h_m);

//! Stop the worker (bounded by one camera read timeout), close the camera, free.
void
sim_webcam_tracker_destroy(struct sim_webcam_tracker **t_ptr);

//! The eyes to report now; see @ref sim_webcam_track_sample.
void
sim_webcam_tracker_sample(struct sim_webcam_tracker *t,
                          const struct xrt_vec3 nominal[2],
                          uint64_t now_ns,
                          struct sim_webcam_eyes *out);

//! Frames analysed so far (diagnostics, tests).
uint64_t
sim_webcam_tracker_frame_count(struct sim_webcam_tracker *t);

#ifdef __cplusplus
}
#endif
