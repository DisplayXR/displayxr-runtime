// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_tracking's `SOURCE=webcam` mode: the webcam eye tracker (#1855)
 *         as a viewer-tracking provider (ADR-054 §4.1, #2144 stack part 3).
 *
 * Wraps the shared core (sim_webcam_source.h) in the provider shape: probe
 * says yes only when a face estimator is built in AND a camera is enumerated
 * (never opens it); start() starts the camera worker plus a pump thread that
 * samples it at RATE_HZ and pushes display-space eye samples — with the
 * camera-space block (camera key, per-eye direction + distance) whenever a
 * face was measured — into the runtime's sink.
 *
 * @ingroup drv_sim_tracking
 */

#pragma once

#include "xrt/xrt_tracking_plugin.h"

#include "sim_webcam_settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_stereo_uvc_backend;
struct sim_face_estimator;
struct sim_webcam_tracker;
struct sim_tracking_webcam;
struct sim_tracking_camera;

/*!
 * Test seam: use @p backend instead of this OS's capture backend and/or
 * @p est (BORROWED) instead of the built-in estimator, for every later probe /
 * describe / start in this process. NULL restores the default. Not exported
 * from the plug-in binary (hidden visibility); the unit test compiles the
 * provider in.
 */
void
sim_tracking_webcam_set_test_overrides(const struct u_stereo_uvc_backend *backend, struct sim_face_estimator *est);

//! Is there a usable webcam source (estimator + an enumerated camera)? Opens nothing.
bool
sim_tracking_webcam_probe(const struct sim_webcam_settings *s);

//! Fill the provider description for the webcam source.
void
sim_tracking_webcam_describe(const struct sim_webcam_settings *s, struct xrt_tracking_provider_info *out);

//! The key of the camera `WEBCAM_DEVICE` selects (what `CAMERA_POSE_<n>_CAMERA` matches). Opens nothing.
bool
sim_tracking_webcam_camera_key(const struct sim_webcam_settings *s, char *out, size_t cap);

/*!
 * Start the camera worker and the pump. @p nominal is the display's nominal
 * viewer (the MANAGED collapse target), @p screen_h_m the panel height (the
 * camera sits on its top edge). @p calibrated (copied; NULL = none) is the
 * camera's `CAMERA_POSE_<n>`: when given, every measurement is mapped to
 * display space as R * cam + T (sim_tracking_camera_to_display, with its
 * depth / focal scale) BEFORE smoothing, replacing the bezel-offset model.
 * Pushes one sample synchronously. NULL when the source cannot run (the core
 * WARNs why).
 */
struct sim_tracking_webcam *
sim_tracking_webcam_start(const struct sim_webcam_settings *s,
                          float screen_h_m,
                          const float nominal[3],
                          float ipd_m,
                          int64_t interval_ns,
                          const struct sim_tracking_camera *calibrated,
                          const struct xrt_tracking_sink *sink);

//! Stop the pump, then the worker (closes the camera); returns after the last push.
void
sim_tracking_webcam_stop(struct sim_tracking_webcam **wc_ptr);

//! RUNNING / NO_SOURCE / UNKNOWN (still opening). Non-blocking.
enum xrt_tracking_provider_status
sim_tracking_webcam_status(struct sim_tracking_webcam *wc);

/*!
 * One provider sample from the tracker's state at @p now_ns (pure apart from
 * the tracker lock): the MANAGED-blended eye pair, head = their midpoint,
 * TRACKING while a face is within the timeout, and the camera block when a
 * fresh camera-space measurement exists. capture_ns is the newest face
 * frame's capture time while tracking, @p now_ns otherwise.
 */
void
sim_tracking_webcam_make_sample(struct sim_webcam_tracker *t,
                                const float nominal[3],
                                float ipd_m,
                                int64_t now_ns,
                                struct xrt_tracking_viewer_sample *out);

#ifdef __cplusplus
}
#endif
