// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  webcam eye tracking (#1855, sim_tracking SOURCE=webcam): the worker — one thread
 *         that opens the camera, runs the face estimator on every frame at
 *         the camera rate, maps + smooths the result and leaves the newest
 *         state for its host to sample (latest wins): sim_tracking's
 *         provider thread, or sim_display's display processors (the
 *         deprecated alias).
 *
 * The capture backend and the estimator are injected, so tests run the
 * whole loop on the synthetic UVC backend with a fake estimator and never
 * touch a camera. Device enumeration, mode listing and open all happen ON
 * the worker: creating a tracker never blocks the caller (a compositor
 * thread). Sampling takes a mutex the worker holds only to fold one result
 * into a small struct.
 *
 * @ingroup drv_sim_tracking
 */

#pragma once

#include "sim_webcam_tracking.h"

#include <stdbool.h>
#include <stddef.h>
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

/*!
 * A host's camera -> display mapping that REPLACES the built-in bezel-offset
 * model (@ref sim_webcam_landmarks_to_eyes): e.g. sim_tracking applying a
 * calibrated `CAMERA_POSE_<n>` (R * cam + T). Called on the worker thread
 * for every frame with a face, before smoothing; @p out_eyes[0] must be the
 * viewer's left.
 */
typedef void (*sim_webcam_map_fn)(void *ctx, const struct sim_webcam_camera_eyes *cam, struct xrt_vec3 out_eyes[2]);

//! @ref sim_webcam_tracker_create with a mapper (@p map_fn NULL = the built-in model). @p map_ctx is BORROWED.
struct sim_webcam_tracker *
sim_webcam_tracker_create_mapped(const struct sim_webcam_config *cfg,
                                 const struct u_stereo_uvc_backend *backend,
                                 const char *device_sel,
                                 struct sim_face_estimator *est,
                                 float screen_h_m,
                                 sim_webcam_map_fn map_fn,
                                 void *map_ctx);

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

//! Where the worker is.
enum sim_webcam_tracker_state
{
	SIM_WEBCAM_TRACKER_STARTING = 0,  //!< enumerating / opening
	SIM_WEBCAM_TRACKER_RUNNING = 1,   //!< the camera is open and delivering
	SIM_WEBCAM_TRACKER_NO_SOURCE = 2, //!< no matching camera, open failed, or the camera failed
};

enum sim_webcam_tracker_state
sim_webcam_tracker_state(struct sim_webcam_tracker *t);

/*!
 * The camera-space measurement of the newest analysed frame that had a face
 * (raw, unsmoothed), its capture time and the camera's key (USB "vvvv:pppp"
 * when the device has one, else its OS id). @return false when the camera is
 * not open yet or no face was measured within the tracking timeout of
 * @p now_ns. @p key may be NULL.
 */
bool
sim_webcam_tracker_camera(struct sim_webcam_tracker *t,
                          uint64_t now_ns,
                          struct sim_webcam_camera_eyes *out,
                          uint64_t *out_capture_ns,
                          char *key,
                          size_t key_cap);

#ifdef __cplusplus
}
#endif
