// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  webcam eye tracking (#1855, sim_tracking SOURCE=webcam): the worker. See the header.
 * @ingroup drv_sim_tracking
 */

#include "sim_webcam_tracker.h"
#include "sim_face_estimator.h"

#include "util/u_logging.h"
#include "util/u_stereo_uvc.h"
#include "os/os_threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! Bounds how long a stop request waits for the worker.
#define SIM_WEBCAM_READ_TIMEOUT_NS (100ll * 1000000ll)

struct sim_webcam_tracker
{
	struct os_thread_helper oth;

	// Immutable after create.
	struct sim_webcam_config cfg;
	struct u_stereo_uvc_backend backend;
	char device_sel[256];
	struct sim_face_estimator *est;
	float screen_h_m;
	sim_webcam_map_fn map_fn; //!< NULL = the built-in bezel-offset model
	void *map_ctx;

	// Guarded by lock: the latest-wins handoff.
	struct os_mutex lock;
	struct sim_webcam_track_state state;
	uint64_t frames;
	enum sim_webcam_tracker_state wstate;
	char camera_key[128];
	bool have_camera_meas;
	uint64_t camera_meas_ns;
	struct sim_webcam_camera_eyes camera_meas;
};

static void
set_state(struct sim_webcam_tracker *t, enum sim_webcam_tracker_state s)
{
	os_mutex_lock(&t->lock);
	t->wstate = s;
	os_mutex_unlock(&t->lock);
}

static void
publish(struct sim_webcam_tracker *t,
        bool found,
        const struct xrt_vec3 eyes[2],
        const struct sim_webcam_camera_eyes *cam,
        uint64_t t_ns)
{
	os_mutex_lock(&t->lock);
	sim_webcam_track_update(&t->state, &t->cfg, found, eyes, t_ns);
	if (found && cam != NULL) {
		t->camera_meas = *cam;
		t->camera_meas_ns = t_ns;
		t->have_camera_meas = true;
	}
	t->frames++;
	os_mutex_unlock(&t->lock);
}

static bool
keep_running(struct sim_webcam_tracker *t)
{
	os_thread_helper_lock(&t->oth);
	const bool r = os_thread_helper_is_running_locked(&t->oth);
	os_thread_helper_unlock(&t->oth);
	return r;
}

static void *
worker(void *ptr)
{
	struct sim_webcam_tracker *t = (struct sim_webcam_tracker *)ptr;
	const struct u_stereo_uvc_backend *b = &t->backend;

	struct u_stereo_uvc_device devs[U_STEREO_UVC_MAX_DEVICES]; // ~6 KB
	struct u_stereo_uvc_mode modes[U_STEREO_UVC_MAX_MODES];

	const uint32_t n = b->enumerate != NULL ? b->enumerate(b->ctx, devs, U_STEREO_UVC_MAX_DEVICES) : 0;
	const int32_t idx = sim_webcam_select_device(devs, n, t->device_sel);
	if (idx < 0) {
		U_LOG_W(
		    "webcam tracking (#1855): no camera matches WEBCAM_DEVICE='%s' (%u present)"
		    " - not tracking",
		    t->device_sel, n);
		set_state(t, SIM_WEBCAM_TRACKER_NO_SOURCE);
		return NULL;
	}
	struct u_stereo_uvc_device dev = devs[idx];
	os_mutex_lock(&t->lock);
	sim_webcam_camera_key(&dev, t->camera_key, sizeof(t->camera_key));
	os_mutex_unlock(&t->lock);

	const uint32_t mc = b->list_modes != NULL ? b->list_modes(b->ctx, dev.id, modes, U_STEREO_UVC_MAX_MODES) : 0;
	struct u_stereo_uvc_mode mode;
	if (!sim_webcam_select_mode(modes, mc, &mode)) {
		U_LOG_W("webcam tracking (#1855): camera '%s' reports no capture mode - not tracking", dev.name);
		set_state(t, SIM_WEBCAM_TRACKER_NO_SOURCE);
		return NULL;
	}

	if (!keep_running(t)) {
		return NULL; // stopped before the camera was ever opened
	}
	void *h = NULL;
	if (b->open == NULL || !b->open(b->ctx, dev.id, &mode, 0, 0, &h)) {
		U_LOG_W("webcam tracking (#1855): could not open camera '%s' - not tracking", dev.name);
		set_state(t, SIM_WEBCAM_TRACKER_NO_SOURCE);
		return NULL;
	}
	set_state(t, SIM_WEBCAM_TRACKER_RUNNING);
	U_LOG_W("webcam tracking (#1855): camera OPENED - '%s' %ux%u@%.0f, estimator '%s'", dev.name, mode.width,
	        mode.height, (double)mode.fps, t->est->name != NULL ? t->est->name : "?");

	while (keep_running(t)) {
		struct u_stereo_uvc_raw_frame frame;
		const uint32_t r = b->read(h, SIM_WEBCAM_READ_TIMEOUT_NS, &frame);
		if (r == U_STEREO_UVC_READ_TIMEOUT) {
			continue;
		}
		if (r != U_STEREO_UVC_READ_OK) {
			U_LOG_W("webcam tracking (#1855): camera '%s' failed - tracking stopped", dev.name);
			set_state(t, SIM_WEBCAM_TRACKER_NO_SOURCE);
			break;
		}
		struct sim_face_landmarks lm;
		memset(&lm, 0, sizeof(lm));
		struct xrt_vec3 eyes[2] = {{0, 0, 0}, {0, 0, 0}};
		struct sim_webcam_camera_eyes cam;
		memset(&cam, 0, sizeof(cam));
		bool found = t->est->estimate(t->est, &frame, &lm) &&
		             sim_webcam_landmarks_to_camera(&t->cfg, frame.width, frame.height, &lm, &cam);
		if (found && t->map_fn != NULL) {
			t->map_fn(t->map_ctx, &cam, eyes); // the host's calibration replaces the bezel model
		} else if (found) {
			found =
			    sim_webcam_landmarks_to_eyes(&t->cfg, t->screen_h_m, frame.width, frame.height, &lm, eyes);
		}
		publish(t, found, eyes, &cam, frame.time_ns > 0 ? (uint64_t)frame.time_ns : 0);
	}

	b->close(h);
	U_LOG_W("webcam tracking (#1855): camera CLOSED - '%s'", dev.name);
	return NULL;
}

struct sim_webcam_tracker *
sim_webcam_tracker_create(const struct sim_webcam_config *cfg,
                          const struct u_stereo_uvc_backend *backend,
                          const char *device_sel,
                          struct sim_face_estimator *est,
                          float screen_h_m)
{
	return sim_webcam_tracker_create_mapped(cfg, backend, device_sel, est, screen_h_m, NULL, NULL);
}

struct sim_webcam_tracker *
sim_webcam_tracker_create_mapped(const struct sim_webcam_config *cfg,
                                 const struct u_stereo_uvc_backend *backend,
                                 const char *device_sel,
                                 struct sim_face_estimator *est,
                                 float screen_h_m,
                                 sim_webcam_map_fn map_fn,
                                 void *map_ctx)
{
	if (cfg == NULL || backend == NULL || backend->read == NULL || backend->close == NULL || est == NULL ||
	    est->estimate == NULL) {
		return NULL;
	}
	struct sim_webcam_tracker *t = calloc(1, sizeof(*t));
	if (t == NULL) {
		return NULL;
	}
	t->cfg = *cfg;
	t->backend = *backend;
	if (device_sel != NULL) {
		snprintf(t->device_sel, sizeof(t->device_sel), "%s", device_sel);
	}
	t->est = est;
	t->screen_h_m = screen_h_m;
	t->map_fn = map_fn;
	t->map_ctx = map_ctx;
	sim_webcam_track_reset(&t->state);

	if (os_mutex_init(&t->lock) != 0) {
		free(t);
		return NULL;
	}
	if (os_thread_helper_init(&t->oth) != 0) {
		os_mutex_destroy(&t->lock);
		free(t);
		return NULL;
	}
	if (os_thread_helper_start(&t->oth, worker, t) != 0) {
		os_thread_helper_destroy(&t->oth);
		os_mutex_destroy(&t->lock);
		free(t);
		return NULL;
	}
	return t;
}

void
sim_webcam_tracker_destroy(struct sim_webcam_tracker **t_ptr)
{
	if (t_ptr == NULL || *t_ptr == NULL) {
		return;
	}
	struct sim_webcam_tracker *t = *t_ptr;
	os_thread_helper_destroy(&t->oth); // signals stop + joins
	os_mutex_destroy(&t->lock);
	free(t);
	*t_ptr = NULL;
}

void
sim_webcam_tracker_sample(struct sim_webcam_tracker *t,
                          const struct xrt_vec3 nominal[2],
                          uint64_t now_ns,
                          struct sim_webcam_eyes *out)
{
	os_mutex_lock(&t->lock);
	sim_webcam_track_sample(&t->state, &t->cfg, nominal, now_ns, out);
	os_mutex_unlock(&t->lock);
}

uint64_t
sim_webcam_tracker_frame_count(struct sim_webcam_tracker *t)
{
	os_mutex_lock(&t->lock);
	const uint64_t n = t->frames;
	os_mutex_unlock(&t->lock);
	return n;
}

enum sim_webcam_tracker_state
sim_webcam_tracker_state(struct sim_webcam_tracker *t)
{
	os_mutex_lock(&t->lock);
	const enum sim_webcam_tracker_state s = t->wstate;
	os_mutex_unlock(&t->lock);
	return s;
}

bool
sim_webcam_tracker_camera(struct sim_webcam_tracker *t,
                          uint64_t now_ns,
                          struct sim_webcam_camera_eyes *out,
                          uint64_t *out_capture_ns,
                          char *key,
                          size_t key_cap)
{
	os_mutex_lock(&t->lock);
	const bool fresh = t->have_camera_meas && t->camera_key[0] != '\0' &&
	                   (now_ns <= t->camera_meas_ns || now_ns - t->camera_meas_ns <= t->cfg.timeout_ns);
	if (fresh) {
		if (out != NULL) {
			*out = t->camera_meas;
		}
		if (out_capture_ns != NULL) {
			*out_capture_ns = t->camera_meas_ns;
		}
	}
	if (key != NULL && key_cap > 0) {
		snprintf(key, key_cap, "%s", t->camera_key);
	}
	os_mutex_unlock(&t->lock);
	return fresh;
}
