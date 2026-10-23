// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  webcam eye tracking (#1855, sim_tracking SOURCE=webcam): the pluggable face
 *         estimator — one camera frame in, two pupil positions out.
 *
 * Everything around the estimator (camera, worker thread, geometry,
 * smoothing, timeout, publishing) is estimator-agnostic; tests drive it with
 * a fake. Which estimator ships is an open question (#1855): the default
 * build carries NONE (@ref sim_face_estimator_default_available returns
 * false), and webcam tracking then stays off — the camera is never opened
 * for an estimator that cannot see a face. See
 * docs/specs/runtime/tracking-provider-discovery.md § sim_tracking.
 *
 * Constraints on an estimator that lands here: vendor-neutral, permissively
 * licensed, builds in this repo's toolchain on Windows + Linux, model weights
 * FETCHED at build time with a pinned hash (never committed), CPU-only, fast
 * enough for the camera rate at ~640x480.
 *
 * @ingroup drv_sim_tracking
 */

#pragma once

#include "sim_webcam_settings.h"
#include "sim_webcam_tracking.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_stereo_uvc_raw_frame;

struct sim_face_estimator
{
	const char *name;
	/*!
	 * Find the viewer's face in @p frame (NV12 or YUY2, see
	 * u_stereo_uvc_raw_frame; the luma plane is all a grey-scale model
	 * needs) and report its pupils in pixels of @p frame. Called on the
	 * tracker's worker thread only. @return false on an internal error
	 * (treated as "no face").
	 */
	bool (*estimate)(struct sim_face_estimator *est,
	                 const struct u_stereo_uvc_raw_frame *frame,
	                 struct sim_face_landmarks *out);
	void (*destroy)(struct sim_face_estimator *est);
};

/*!
 * Is a real estimator built into this binary (and its files present)? Cheap,
 * loads nothing — decides whether a webcam tracker may advertise tracking and
 * open the camera. @p settings supplies `WEBCAM_MODEL` (a model path
 * override); NULL = none. The answer is cached per process.
 */
bool
sim_face_estimator_default_available(const struct sim_webcam_settings *settings);

//! The built-in estimator's name ("yunet-onnx"), or "none" for the stub build.
const char *
sim_face_estimator_default_name(void);

/*!
 * The built-in estimator, or NULL when none is built in. Called on a display
 * processor's thread, so it MUST be cheap: load model weights lazily on the
 * first estimate() call (the worker thread), never here.
 */
struct sim_face_estimator *
sim_face_estimator_create_default(const struct sim_webcam_settings *settings);

static inline void
sim_face_estimator_destroy(struct sim_face_estimator **est_ptr)
{
	if (est_ptr != NULL && *est_ptr != NULL) {
		(*est_ptr)->destroy(*est_ptr);
		*est_ptr = NULL;
	}
}

#ifdef __cplusplus
}
#endif
