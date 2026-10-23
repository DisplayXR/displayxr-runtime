// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_tracking's `SOURCE=webcam` mode. See the header.
 * @ingroup drv_sim_tracking
 */

#include "sim_tracking_webcam.h"
#include "sim_tracking_script.h"
#include "sim_webcam_source.h"
#include "sim_face_estimator.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_stereo_uvc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Set only by the unit test, before any probe / start (no locking needed).
static bool g_have_backend_override = false;
static struct u_stereo_uvc_backend g_backend_override;
static struct sim_face_estimator *g_est_override = NULL;

struct sim_tracking_webcam
{
	struct os_thread_helper oth;
	struct sim_webcam_source *src;
	const struct xrt_tracking_sink *sink;
	float nominal[3];
	float ipd_m;
	int64_t interval_ns;
	uint64_t last_frames;                  //!< pump thread only
	struct sim_tracking_camera calibrated; //!< the believed pose, when have_calibrated
	bool have_calibrated;
};

//! The worker's mapper under a calibration: display = R * cam + T (sim_tracking_camera_to_display).
static void
map_calibrated(void *ctx, const struct sim_webcam_camera_eyes *cam, struct xrt_vec3 out_eyes[2])
{
	const struct sim_tracking_camera *c = (const struct sim_tracking_camera *)ctx;
	for (int e = 0; e < 2; e++) {
		float o[3];
		sim_tracking_camera_to_display(c, cam->dir[e], cam->distance_m[e], o);
		out_eyes[e] = (struct xrt_vec3){o[0], o[1], o[2]};
	}
}

void
sim_tracking_webcam_set_test_overrides(const struct u_stereo_uvc_backend *backend, struct sim_face_estimator *est)
{
	g_have_backend_override = backend != NULL;
	if (backend != NULL) {
		g_backend_override = *backend;
	}
	g_est_override = est;
}

static const struct u_stereo_uvc_backend *
backend_or_null(void)
{
	return g_have_backend_override ? &g_backend_override : NULL;
}

bool
sim_tracking_webcam_camera_key(const struct sim_webcam_settings *s, char *out, size_t cap)
{
	struct sim_webcam_camera_info cam;
	if (sim_webcam_selected_camera(s, backend_or_null(), &cam) == 0) {
		return false;
	}
	snprintf(out, cap, "%s", cam.key);
	return true;
}

bool
sim_tracking_webcam_probe(const struct sim_webcam_settings *s)
{
	if (g_est_override == NULL && !sim_webcam_estimator_usable(s)) {
		U_LOG_W(
		    "sim-tracking: SOURCE=webcam declines - no usable face estimator in this build (built in: '%s'; "
		    "-DXRT_SIM_WEBCAM_ESTIMATOR=onnx builds one)",
		    sim_face_estimator_default_name());
		return false;
	}
	struct sim_webcam_camera_info cam;
	if (sim_webcam_selected_camera(s, backend_or_null(), &cam) == 0) {
		U_LOG_W("sim-tracking: SOURCE=webcam declines - no camera matches WEBCAM_DEVICE (or none present)");
		return false;
	}
	return true;
}

void
sim_tracking_webcam_describe(const struct sim_webcam_settings *s, struct xrt_tracking_provider_info *out)
{
	out->tracks = XRT_TRACKING_TRACKS_EYE_PAIR | XRT_TRACKING_TRACKS_HEAD_POSE | XRT_TRACKING_TRACKS_CAMERA_SPACE;
	out->typical_latency_ns = 0;
	struct sim_webcam_camera_info cam;
	memset(&cam, 0, sizeof(cam));
	(void)sim_webcam_selected_camera(s, backend_or_null(), &cam);
	const char *est = g_est_override != NULL && g_est_override->name != NULL ? g_est_override->name
	                                                                         : sim_face_estimator_default_name();
	snprintf(out->source, sizeof(out->source), "webcam '%s' (%s)", cam.name[0] != '\0' ? cam.name : "?", est);
	if (out->struct_size >= offsetof(struct xrt_tracking_provider_info, camera_key) + sizeof(out->camera_key)) {
		snprintf(out->camera_key, sizeof(out->camera_key), "%s", cam.key);
	}
}

void
sim_tracking_webcam_make_sample(struct sim_webcam_tracker *t,
                                const float nominal[3],
                                float ipd_m,
                                int64_t now_ns,
                                struct xrt_tracking_viewer_sample *out)
{
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	const float half = ipd_m * 0.5f;
	const struct xrt_vec3 nom[2] = {{nominal[0] - half, nominal[1], nominal[2]},
	                                {nominal[0] + half, nominal[1], nominal[2]}};
	struct sim_webcam_eyes eyes;
	sim_webcam_tracker_sample(t, nom, (uint64_t)now_ns, &eyes);

	out->left_eye[0] = eyes.eyes[0].x;
	out->left_eye[1] = eyes.eyes[0].y;
	out->left_eye[2] = eyes.eyes[0].z;
	out->right_eye[0] = eyes.eyes[1].x;
	out->right_eye[1] = eyes.eyes[1].y;
	out->right_eye[2] = eyes.eyes[1].z;
	for (int c = 0; c < 3; c++) {
		out->head_position[c] = (out->left_eye[c] + out->right_eye[c]) * 0.5f;
	}
	out->head_orientation[3] = 1.0f; // a webcam pupil pair carries no head rotation
	out->flags = XRT_TRACKING_SAMPLE_EYES_VALID | XRT_TRACKING_SAMPLE_HEAD_POSE_VALID |
	             (eyes.is_tracking ? XRT_TRACKING_SAMPLE_TRACKING : 0u);
	out->confidence = eyes.is_tracking ? eyes.weight : 0.0f;
	out->capture_ns = now_ns;

	struct sim_webcam_camera_eyes cam;
	uint64_t cam_ns = 0;
	char key[XRT_TRACKING_CAMERA_KEY_MAX];
	if (sim_webcam_tracker_camera(t, (uint64_t)now_ns, &cam, &cam_ns, key, sizeof(key))) {
		snprintf(out->camera.camera_key, sizeof(out->camera.camera_key), "%s", key);
		memcpy(out->camera.left_dir, cam.dir[0], sizeof(out->camera.left_dir));
		memcpy(out->camera.right_dir, cam.dir[1], sizeof(out->camera.right_dir));
		out->camera.left_distance_m = cam.distance_m[0];
		out->camera.right_distance_m = cam.distance_m[1];
		out->flags |= XRT_TRACKING_SAMPLE_CAMERA_VALID;
		if (eyes.is_tracking && cam_ns > 0 && (int64_t)cam_ns <= now_ns) {
			out->capture_ns = (int64_t)cam_ns; // the measurement's own time, not the push time
		}
	}
}

static void *
pump(void *ptr)
{
	struct sim_tracking_webcam *wc = (struct sim_tracking_webcam *)ptr;
	os_thread_helper_name(&wc->oth, "sim-tracking-webcam");
	struct sim_webcam_tracker *t = sim_webcam_source_tracker(wc->src);
	os_thread_helper_lock(&wc->oth);
	while (os_thread_helper_is_running_locked(&wc->oth)) {
		os_thread_helper_unlock(&wc->oth);

		const int64_t now = (int64_t)os_monotonic_get_ns();
		struct xrt_tracking_viewer_sample s;
		sim_tracking_webcam_make_sample(t, wc->nominal, wc->ipd_m, now, &s);
		// While tracking, push once per analysed frame; while lost, every tick
		// (the MANAGED collapse animates against time).
		const uint64_t frames = sim_webcam_tracker_frame_count(t);
		if ((s.flags & XRT_TRACKING_SAMPLE_TRACKING) == 0 || frames != wc->last_frames) {
			wc->last_frames = frames;
			wc->sink->push(wc->sink, &s);
		}

		os_nanosleep(wc->interval_ns);
		os_thread_helper_lock(&wc->oth);
	}
	os_thread_helper_unlock(&wc->oth);
	return NULL;
}

struct sim_tracking_webcam *
sim_tracking_webcam_start(const struct sim_webcam_settings *s,
                          float screen_h_m,
                          const float nominal[3],
                          float ipd_m,
                          int64_t interval_ns,
                          const struct sim_tracking_camera *calibrated,
                          const struct xrt_tracking_sink *sink)
{
	if (sink == NULL || sink->push == NULL) {
		return NULL;
	}
	// Allocated first: the mapper's context (the calibrated pose) must outlive the worker.
	struct sim_tracking_webcam *wc = (struct sim_tracking_webcam *)calloc(1, sizeof(*wc));
	if (wc == NULL) {
		return NULL;
	}
	if (calibrated != NULL) {
		wc->calibrated = *calibrated;
		wc->have_calibrated = true;
	}
	const struct sim_webcam_source_params params = {
	    .settings = s,
	    .screen_h_m = screen_h_m,
	    .backend = backend_or_null(),
	    .estimator = g_est_override,
	    .log_name = "sim-tracking",
	    .map_fn = wc->have_calibrated ? map_calibrated : NULL,
	    .map_ctx = wc->have_calibrated ? &wc->calibrated : NULL,
	};
	struct sim_webcam_source *src = sim_webcam_source_create(&params);
	if (src == NULL) {
		free(wc);
		return NULL;
	}
	wc->src = src;
	wc->sink = sink;
	memcpy(wc->nominal, nominal, sizeof(wc->nominal));
	wc->ipd_m = ipd_m;
	wc->interval_ns = interval_ns > 0 ? interval_ns : 1000000000LL / 60;

	// One sample synchronously (the nominal viewer, not tracking), so the first locate after start() has data.
	struct xrt_tracking_viewer_sample first;
	sim_tracking_webcam_make_sample(sim_webcam_source_tracker(src), wc->nominal, wc->ipd_m,
	                                (int64_t)os_monotonic_get_ns(), &first);
	sink->push(sink, &first);

	if (os_thread_helper_init(&wc->oth) != 0) {
		sim_webcam_source_destroy(&wc->src);
		free(wc);
		return NULL;
	}
	if (os_thread_helper_start(&wc->oth, pump, wc) != 0) {
		os_thread_helper_destroy(&wc->oth);
		sim_webcam_source_destroy(&wc->src);
		free(wc);
		return NULL;
	}
	return wc;
}

void
sim_tracking_webcam_stop(struct sim_tracking_webcam **wc_ptr)
{
	if (wc_ptr == NULL || *wc_ptr == NULL) {
		return;
	}
	struct sim_tracking_webcam *wc = *wc_ptr;
	os_thread_helper_destroy(&wc->oth);  // stops + joins the pump: no push after this
	sim_webcam_source_destroy(&wc->src); // joins the worker: closes the camera
	free(wc);
	*wc_ptr = NULL;
}

enum xrt_tracking_provider_status
sim_tracking_webcam_status(struct sim_tracking_webcam *wc)
{
	if (wc == NULL) {
		return XRT_TRACKING_PROVIDER_STATUS_STOPPED;
	}
	switch (sim_webcam_tracker_state(sim_webcam_source_tracker(wc->src))) {
	case SIM_WEBCAM_TRACKER_RUNNING: return XRT_TRACKING_PROVIDER_STATUS_RUNNING;
	case SIM_WEBCAM_TRACKER_NO_SOURCE: return XRT_TRACKING_PROVIDER_STATUS_NO_SOURCE;
	default: return XRT_TRACKING_PROVIDER_STATUS_UNKNOWN;
	}
}
