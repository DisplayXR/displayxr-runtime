// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display webcam eye tracking (#1855) — the DEPRECATED alias.
 *         The process glue only: the refcounted tracker singleton the
 *         display processors share, the eye-position override and the
 *         advertised capability.
 *
 * The tracker itself moved to sim_tracking (`SOURCE=webcam`, the viewer-
 * tracking provider class). `WEBCAM_TRACKING=1` keeps today's behaviour for
 * one release — sim_display tracks, OWN-like, and advertises MANAGED — by
 * running the SAME core (sim_tracking/sim_webcam_source.h) with sim_display's
 * settings shim behind its @ref sim_webcam_settings callback. Enabling it logs
 * one WARN pointing to `sim-tracking SOURCE=webcam`.
 *
 * Opt-in only. With WEBCAM_TRACKING unset every entry point is one cached
 * setting check and nothing else: no thread, no camera, no Media Foundation
 * load (the plug-in delay-loads it).
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
#include "sim_display_settings.h"
#include "sim_webcam_source.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_eye_history.h"
#include "util/u_logging.h"
#include "os/os_threading.h"
#include "os/os_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_webcam_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_webcam_refs = 0;                          // guarded
static struct sim_webcam_source *g_webcam_src = NULL;      // guarded
static struct u_eye_history g_webcam_hist;                 // guarded; lives with the tracker (N6 Part 1)

//! The core's settings seam, satisfied by sim_display's shim (host iface + legacy SIM_DISPLAY_* names).
static bool
shim_get(void *ctx, const char *key, char *buf, size_t cap)
{
	(void)ctx;
	return sim_display_setting(key, buf, cap);
}

static const struct sim_webcam_settings g_shim_settings = {shim_get, NULL};

bool
sim_display_webcam_tracking_requested(void)
{
	// Through the host iface (N3c, sim_display_settings.h; cached per process).
	const bool on = sim_display_setting_on("WEBCAM_TRACKING");
	static bool warned = false; // one WARN per process; a racing repeat is harmless
	if (on && !warned) {
		warned = true;
		U_LOG_W(
		    "sim_display: WEBCAM_TRACKING is DEPRECATED (removed next release) - the webcam tracker moved to "
		    "the sim-tracking viewer-tracking provider: 'displayxr-cli tracking set sim-tracking ENABLE 1' + "
		    "'displayxr-cli tracking set sim-tracking SOURCE webcam' (WEBCAM_* settings carry over by name).");
	}
	return on;
}

uint32_t
sim_display_webcam_claimed_cameras(uint32_t capacity, char (*out_ids)[256])
{
	if (!sim_display_webcam_tracking_requested()) {
		return 0;
	}
	struct sim_webcam_camera_info cam;
	if (sim_webcam_selected_camera(&g_shim_settings, NULL, &cam) == 0) {
		return 0;
	}
	if (capacity > 0 && out_ids != NULL) {
		snprintf(out_ids[0], 256, "%s", cam.id);
	}
	return 1;
}

bool
sim_display_webcam_tracking_enabled(void)
{
	return sim_display_webcam_tracking_requested() && sim_webcam_estimator_usable(&g_shim_settings);
}

//! Start the tracker (caller holds the lock). Leaves it NULL when it cannot run (the core WARNs why).
static void
start_locked(void)
{
	u_eye_history_init(&g_webcam_hist); // paired with stop_locked, whatever happens below
	float w_m = 0.0f, h_m = 0.0f;
	uint32_t px_w = 0, px_h = 0;
	sim_display_get_panel_metrics(&w_m, &h_m, &px_w, &px_h);
	if (!(h_m > 0.0f)) {
		sim_display_get_default_viewer(NULL, &h_m, NULL, NULL);
	}
	const struct sim_webcam_source_params params = {
	    .settings = &g_shim_settings,
	    .screen_h_m = h_m,
	    .backend = NULL,
	    .estimator = NULL,
	    .log_name = "sim_display (deprecated WEBCAM_TRACKING)",
	};
	g_webcam_src = sim_webcam_source_create(&params);
}

static void
stop_locked(void)
{
	sim_webcam_source_destroy(&g_webcam_src); // joins: closes the camera
	u_eye_history_destroy(&g_webcam_hist);
}

bool
sim_display_webcam_tracking_apply(
    struct xrt_eye_positions *out, float nominal_x_m, float nominal_y_m, float nominal_z_m, float ipd_m, bool *dp_ref)
{
	return sim_display_webcam_tracking_apply_at(out, nominal_x_m, nominal_y_m, nominal_z_m, ipd_m,
	                                            (int64_t)os_monotonic_get_ns(), dp_ref);
}

bool
sim_display_webcam_tracking_apply_at(struct xrt_eye_positions *out,
                                     float nominal_x_m,
                                     float nominal_y_m,
                                     float nominal_z_m,
                                     float ipd_m,
                                     int64_t target_ns,
                                     bool *dp_ref)
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
	struct sim_webcam_tracker *t = sim_webcam_source_tracker(g_webcam_src);
	if (t == NULL) {
		pthread_mutex_unlock(&g_webcam_lock);
		return false;
	}
	const float half = ipd_m * 0.5f;
	const struct xrt_vec3 nominal[2] = {{nominal_x_m - half, nominal_y_m, nominal_z_m},
	                                    {nominal_x_m + half, nominal_y_m, nominal_z_m}};
	struct sim_webcam_eyes eyes;
	const int64_t now_ns = (int64_t)os_monotonic_get_ns();
	sim_webcam_tracker_sample(t, nominal, (uint64_t)now_ns, &eyes);

	// N6 Part 1 (#2144): the tracker's pairs go through a ring sampled at
	// the TARGET — interpolated between samples, carried forward on the last
	// velocity, or "last known, not tracking" when the camera stalled.
	struct xrt_eye_positions pair;
	memset(&pair, 0, sizeof(pair));
	pair.count = 2;
	pair.eyes[0] = (struct xrt_eye_position){eyes.eyes[0].x, eyes.eyes[0].y, eyes.eyes[0].z};
	pair.eyes[1] = (struct xrt_eye_position){eyes.eyes[1].x, eyes.eyes[1].y, eyes.eyes[1].z};
	pair.valid = true;
	pair.is_tracking = eyes.is_tracking;
	pair.timestamp_ns = now_ns;
	u_eye_history_push(&g_webcam_hist, &pair, now_ns);
	struct xrt_eye_positions at = pair;
	(void)u_eye_history_get(&g_webcam_hist, target_ns, &at); // never EMPTY right after a push
	pthread_mutex_unlock(&g_webcam_lock);

	const struct xrt_vec3 l = {at.eyes[0].x, at.eyes[0].y, at.eyes[0].z};
	const struct xrt_vec3 r = {at.eyes[1].x, at.eyes[1].y, at.eyes[1].z};
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
	out->is_tracking = at.is_tracking;
	out->timestamp_ns = at.timestamp_ns; // the sample time the answer is based on
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
	if (g_webcam_src != NULL) {
		const struct xrt_vec3 zero[2] = {{0, 0, 0}, {0, 0, 0}};
		struct sim_webcam_eyes eyes;
		sim_webcam_tracker_sample(sim_webcam_source_tracker(g_webcam_src), zero, os_monotonic_get_ns(), &eyes);
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
