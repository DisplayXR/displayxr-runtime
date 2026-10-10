// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display webcam eye tracking (#1855): the process glue — env
 *         config, the refcounted tracker singleton the display processors
 *         share, the eye-position override and the advertised capability.
 *
 * Opt-in only. With SIM_DISPLAY_WEBCAM_TRACKING unset every entry point is
 * one cached env check and nothing else: no thread, no camera, no Media
 * Foundation load (the plug-in delay-loads it).
 *
 * The tracker starts when the first session display processor asks for eye
 * positions (never at plug-in load or DP creation, so a process that never
 * renders never opens the camera) and stops when the last such DP is
 * destroyed. Screen-bound (multi-screen segment) DPs keep their nominal
 * viewer: the webcam's pose is known relative to ONE panel only.
 *
 * @ingroup drv_sim_display
 */

#include "sim_display_interface.h"
#include "sim_display_face_estimator.h"
#include "sim_display_webcam_tracker.h"
#include "sim_display_webcam_tracking.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"
#include "util/u_stereo_uvc.h"
#include "os/os_threading.h"
#include "os/os_time.h"
#include "os/os_uvc_capture.h"

#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_webcam_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_webcam_refs = 0;                          // guarded
static struct sim_webcam_tracker *g_webcam_tracker = NULL; // guarded
static struct sim_face_estimator *g_webcam_est = NULL;     // guarded
static bool g_webcam_warned_no_estimator = false;          // guarded

static bool
env_truthy(const char *name)
{
	const char *e = getenv(name);
	return e != NULL && e[0] != '\0' && e[0] != '0';
}

bool
sim_display_webcam_tracking_requested(void)
{
	static int cached = -1;
	if (cached < 0) {
		cached = env_truthy("SIM_DISPLAY_WEBCAM_TRACKING") ? 1 : 0;
	}
	return cached == 1;
}

bool
sim_display_webcam_tracking_enabled(void)
{
	return sim_display_webcam_tracking_requested() && sim_face_estimator_default_available();
}

static void
config_from_env(struct sim_webcam_config *cfg)
{
	sim_webcam_config_defaults(cfg);
	const char *e = getenv("SIM_DISPLAY_WEBCAM_HFOV_DEG");
	if (e != NULL) {
		const float v = (float)atof(e);
		if (v > 10.0f && v < 170.0f) {
			cfg->hfov_deg = v;
		}
	}
	e = getenv("SIM_DISPLAY_WEBCAM_IPD_MM");
	if (e != NULL) {
		const float v = (float)atof(e);
		if (v > 40.0f && v < 90.0f) {
			cfg->ipd_m = v / 1000.0f;
		}
	}
	e = getenv("SIM_DISPLAY_WEBCAM_OFFSET_MM");
	if (e != NULL && !sim_webcam_parse_offset_mm(e, cfg->cam_offset_m)) {
		U_LOG_W("sim_display webcam tracking: SIM_DISPLAY_WEBCAM_OFFSET_MM='%s' is not \"x,y,z\" - ignored", e);
	}
	cfg->mirrored = env_truthy("SIM_DISPLAY_WEBCAM_MIRROR");
}

//! Start the tracker (caller holds the lock). Leaves it NULL when it cannot run.
static void
start_locked(void)
{
	if (!sim_face_estimator_default_available()) {
		if (!g_webcam_warned_no_estimator) {
			g_webcam_warned_no_estimator = true;
			U_LOG_W(
			    "sim_display webcam tracking (#1855): SIM_DISPLAY_WEBCAM_TRACKING is set but this build "
			    "has no face estimator - camera NOT opened, eye positions stay nominal");
		}
		return;
	}
	struct u_stereo_uvc_backend backend;
	if (!os_uvc_capture_backend(&backend)) {
		U_LOG_W(
		    "sim_display webcam tracking (#1855): no camera capture backend on this platform - not tracking");
		return;
	}
	g_webcam_est = sim_face_estimator_create_default();
	if (g_webcam_est == NULL) {
		U_LOG_W("sim_display webcam tracking (#1855): the face estimator failed to load - not tracking");
		return;
	}
	struct sim_webcam_config cfg;
	config_from_env(&cfg);
	float w_m = 0.0f, h_m = 0.0f;
	uint32_t px_w = 0, px_h = 0;
	sim_display_get_panel_metrics(&w_m, &h_m, &px_w, &px_h);
	if (!(h_m > 0.0f)) {
		sim_display_get_default_viewer(NULL, &h_m, NULL, NULL);
	}
	g_webcam_tracker =
	    sim_webcam_tracker_create(&cfg, &backend, getenv("SIM_DISPLAY_WEBCAM_DEVICE"), g_webcam_est, h_m);
	if (g_webcam_tracker == NULL) {
		sim_face_estimator_destroy(&g_webcam_est);
	}
}

static void
stop_locked(void)
{
	sim_webcam_tracker_destroy(&g_webcam_tracker); // joins: closes the camera
	sim_face_estimator_destroy(&g_webcam_est);
}

bool
sim_display_webcam_tracking_apply(
    struct xrt_eye_positions *out, float nominal_x_m, float nominal_y_m, float nominal_z_m, float ipd_m, bool *dp_ref)
{
	if (!sim_display_webcam_tracking_requested() || dp_ref == NULL || out == NULL) {
		return false;
	}

	pthread_mutex_lock(&g_webcam_lock);
	if (!*dp_ref) {
		*dp_ref = true;
		if (g_webcam_refs++ == 0) {
			start_locked();
		}
	}
	struct sim_webcam_tracker *t = g_webcam_tracker;
	if (t == NULL) {
		pthread_mutex_unlock(&g_webcam_lock);
		return false;
	}
	const float half = ipd_m * 0.5f;
	const struct xrt_vec3 nominal[2] = {{nominal_x_m - half, nominal_y_m, nominal_z_m},
	                                    {nominal_x_m + half, nominal_y_m, nominal_z_m}};
	struct sim_webcam_eyes eyes;
	sim_webcam_tracker_sample(t, nominal, os_monotonic_get_ns(), &eyes);
	pthread_mutex_unlock(&g_webcam_lock);

	const struct xrt_vec3 l = eyes.eyes[0], r = eyes.eyes[1];
	if (out->count == 1) {
		out->eyes[0] = (struct xrt_eye_position){(l.x + r.x) * 0.5f, (l.y + r.y) * 0.5f, (l.z + r.z) * 0.5f};
	} else if (out->count >= 4) {
		// Same 2x2 layout the nominal quad uses: the pair, 32 mm below and above.
		out->eyes[0] = (struct xrt_eye_position){l.x, l.y - 0.032f, l.z};
		out->eyes[1] = (struct xrt_eye_position){r.x, r.y - 0.032f, r.z};
		out->eyes[2] = (struct xrt_eye_position){l.x, l.y + 0.032f, l.z};
		out->eyes[3] = (struct xrt_eye_position){r.x, r.y + 0.032f, r.z};
	} else {
		out->eyes[0] = (struct xrt_eye_position){l.x, l.y, l.z};
		out->eyes[1] = (struct xrt_eye_position){r.x, r.y, r.z};
	}
	out->valid = true;
	out->is_tracking = eyes.is_tracking;
	return true;
}

void
sim_display_webcam_tracking_release(bool *dp_ref)
{
	if (dp_ref == NULL || !*dp_ref) {
		return;
	}
	pthread_mutex_lock(&g_webcam_lock);
	*dp_ref = false;
	if (--g_webcam_refs == 0) {
		stop_locked();
	}
	pthread_mutex_unlock(&g_webcam_lock);
}

bool
sim_display_webcam_tracking_status(bool *out_tracking, uint64_t *out_edges)
{
	if (!sim_display_webcam_tracking_enabled()) {
		return false;
	}
	bool tracking = false;
	uint64_t edges = 0;
	pthread_mutex_lock(&g_webcam_lock);
	if (g_webcam_tracker != NULL) {
		const struct xrt_vec3 zero[2] = {{0, 0, 0}, {0, 0, 0}};
		struct sim_webcam_eyes eyes;
		sim_webcam_tracker_sample(g_webcam_tracker, zero, os_monotonic_get_ns(), &eyes);
		tracking = eyes.is_tracking;
		edges = eyes.edges;
	}
	pthread_mutex_unlock(&g_webcam_lock);
	if (out_tracking != NULL) {
		*out_tracking = tracking;
	}
	if (out_edges != NULL) {
		*out_edges = edges;
	}
	return true;
}

void
sim_display_eye_tracking_caps(uint32_t *out_supported, uint32_t *out_default)
{
	uint32_t supported = 0u;
	uint32_t def = 0u;
	if (sim_display_fake_tracking_enabled()) {
		supported |= 2u; // MANUAL_BIT
		def = 1u;        // MANUAL
	}
	if (sim_display_webcam_tracking_enabled()) {
		supported |= 1u; // MANAGED_BIT: the webcam path animates loss itself
		def = 0u;        // MANAGED
	}
	*out_supported = supported;
	*out_default = def;
}
