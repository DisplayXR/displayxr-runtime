// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_tracking — the in-tree reference viewer-tracking provider (ADR-054 §4.1, N6 Part 2).
 *
 * Implements `xrt/xrt_tracking_plugin.h`, with two sources (`SOURCE`):
 *  - `script` (default): a scripted viewer (`SCRIPT=sweep|offset`) — the
 *    hardware-free source CI uses to prove that `xrLocateViews` follows a
 *    provider on a display that does not track (sim_display, EXTERNAL_OK).
 *    `CAMERA=1` adds a simulated webcam (the calibration test bed);
 *  - `webcam`: the webcam eye tracker of #1855 (moved here from sim_display;
 *    sim_tracking_webcam.h). Display-space eye samples plus the camera-space
 *    block (camera key, per-eye direction + distance). Needs a face estimator
 *    built in (`-DXRT_SIM_WEBCAM_ESTIMATOR=onnx`); the default build's probe
 *    declines.
 * Both camera sources report display eyes through the camera's pose as the
 * RUNTIME answers it (`host->get_camera_pose`: display-declared >
 * user-calibration > placement-hint > default-prior, spec §6) — R * cam + T,
 * with the viewer's per-user iris scale. The provider keeps no prior of its
 * own any more; the legacy `CAMERA_POSE_<n>` settings (and, for the webcam,
 * an explicit `WEBCAM_OFFSET_MM`) are read only as one-release fallbacks.
 *
 * **Opt-in.** `probe()` declines unless the `ENABLE` setting is 1 — being
 * registered is not enough (a scripted head sweeping the views with nothing
 * driving it is not something a user should ever get by accident). Settings
 * come ONLY through the host iface (ADR-054 decision 7): `displayxr-cli
 * tracking set sim-tracking ENABLE 1`, or `DXR_TRACKING_SIM_TRACKING_ENABLE=1`
 * in the hosting process's environment, which always wins.
 *
 * Shape the class contract asks of every provider: session-free, its own
 * thread (started by `start()`, joined by `stop()`), samples pushed into the
 * runtime's sink stamped with their capture time. sim_display's
 * `WEBCAM_TRACKING` is the one-release deprecation alias of `SOURCE=webcam`
 * and runs the same core (sim_webcam_source.h) through its own settings.
 *
 * @ingroup drv_sim_tracking
 */

#include "xrt/xrt_tracking_plugin.h"
#include "xrt/xrt_results.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include "sim_tracking_script.h"
#include "sim_tracking_webcam.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM_TRACKING_ID "sim-tracking"


/*
 *
 * Settings (host iface only — no getenv anywhere in this driver).
 *
 */

static const struct xrt_component_setting_desc g_descs[] = {
    {"ENABLE", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
     "probe() declines unless 1: being registered is not enough (a scripted viewer would drive the views of "
     "every display that accepts an external viewer)"},
    {"SOURCE", XRT_COMPONENT_SETTING_TYPE_ENUM, "script",
     "script (a scripted viewer; see SCRIPT, CAMERA) | webcam (the webcam eye tracker; needs a face estimator "
     "built in, -DXRT_SIM_WEBCAM_ESTIMATOR=onnx; see WEBCAM_*)"},
    {"SCRIPT", XRT_COMPONENT_SETTING_TYPE_ENUM, "sweep",
     "sweep (the head sweeps left-right, AMPLITUDE_M over PERIOD_MS) | offset (holds still at nominal + "
     "OFFSET_*_M)"},
    {"PERIOD_MS", XRT_COMPONENT_SETTING_TYPE_INT, "4000", "sweep period"},
    {"AMPLITUDE_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.10", "sweep half-width, metres"},
    {"OFFSET_X_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0", "viewer offset from the nominal viewer, metres (+X right)"},
    {"OFFSET_Y_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0", "viewer offset, metres (+Y up)"},
    {"OFFSET_Z_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0", "viewer offset, metres (+Z toward the viewer)"},
    {"IPD_MM", XRT_COMPONENT_SETTING_TYPE_FLOAT, "63", "eye separation"},
    {"LOSS_PERIOD_MS", XRT_COMPONENT_SETTING_TYPE_INT, "0",
     "> 0: tracking square-waves off for half of every period (exercises is_tracking edges + "
     "XrEventDataEyeTrackingStateChangedDXR); 0 = always tracking"},
    {"RATE_HZ", XRT_COMPONENT_SETTING_TYPE_INT, "60", "samples per second the provider thread pushes"},
    {"CAMERA", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
     "1: simulate a webcam (calibration test bed). The scripted eyes are MEASURED from CAMERA_TRUE_POSE "
     "(camera-space samples, XRT_TRACKING_SAMPLE_CAMERA_VALID) and REPORTED through the runtime's pose for "
     "CAMERA_KEY (host get_camera_pose: calibration > placement hint > top-centre default prior)"},
    {"CAMERA_KEY", XRT_COMPONENT_SETTING_TYPE_STRING, "sim-tracking:webcam0", "the simulated camera's key"},
    {"CAMERA_TRUE_POSE", XRT_COMPONENT_SETTING_TYPE_STRING, "",
     "hidden ground truth \"tx,ty,tz,qx,qy,qz,qw\" (metres, display space); empty = the bezel prior"},
    {"CAMERA_DEPTH_BIAS", XRT_COMPONENT_SETTING_TYPE_FLOAT, "1",
     "reported distance = true distance x this (a simulated per-user iris-size error)"},
    {"WEBCAM_ESTIMATOR", XRT_COMPONENT_SETTING_TYPE_ENUM, "auto",
     "SOURCE=webcam: auto (the built-in face estimator), none, or the built-in name (yunet-onnx)"},
    {"WEBCAM_MODEL", XRT_COMPONENT_SETTING_TYPE_STRING, "",
     "SOURCE=webcam: face model path (default: beside the plug-in)"},
    {"WEBCAM_DEVICE", XRT_COMPONENT_SETTING_TYPE_STRING, "",
     "SOURCE=webcam: camera index or a substring of its name (default: the first)"},
    {"WEBCAM_HFOV_DEG", XRT_COMPONENT_SETTING_TYPE_FLOAT, "70", "SOURCE=webcam: horizontal field of view (10..170)"},
    {"WEBCAM_IPD_MM", XRT_COMPONENT_SETTING_TYPE_FLOAT, "63", "SOURCE=webcam: IPD prior for depth (40..90)"},
    {"WEBCAM_OFFSET_MM", XRT_COMPONENT_SETTING_TYPE_STRING, "0,0,0",
     "DEPRECATED (SOURCE=webcam): when set, the legacy bezel-offset model (camera at the panel's top-centre + "
     "\"x,y,z\" mm) replaces the runtime's default prior; a calibration or placement hint still wins"},
    {"WEBCAM_MIRROR", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "SOURCE=webcam: the source delivers a mirrored image"},
};

static const struct xrt_tracking_plugin_host_iface *g_host = NULL;

static bool
setting(const char *key, char *buf, uint32_t cap)
{
	buf[0] = '\0';
	if (g_host == NULL ||
	    g_host->struct_size <
	        offsetof(struct xrt_tracking_plugin_host_iface, get_setting) + sizeof(g_host->get_setting) ||
	    g_host->get_setting == NULL) {
		return false;
	}
	return g_host->get_setting(SIM_TRACKING_ID, key, buf, cap) && buf[0] != '\0';
}

static bool
setting_on(const char *key)
{
	char v[16];
	if (!setting(key, v, sizeof(v))) {
		return false;
	}
	return !(strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0 || strcmp(v, "off") == 0);
}

static float
setting_float(const char *key, float fallback)
{
	char v[32];
	if (!setting(key, v, sizeof(v))) {
		return fallback;
	}
	char *end = NULL;
	const double d = strtod(v, &end);
	return (end != v) ? (float)d : fallback;
}

static int64_t
setting_ms(const char *key, int64_t fallback)
{
	char v[32];
	if (!setting(key, v, sizeof(v))) {
		return fallback;
	}
	char *end = NULL;
	const long long n = strtoll(v, &end, 10);
	return (end != v && n >= 0) ? (int64_t)n : fallback;
}

//! The webcam core's settings seam, satisfied by the host iface.
static bool
webcam_get(void *ctx, const char *key, char *buf, size_t cap)
{
	(void)ctx;
	return setting(key, buf, (uint32_t)cap);
}

static const struct sim_webcam_settings g_webcam_settings = {webcam_get, NULL};

//! SOURCE=webcam? (anything else, unset included, is the script)
static bool
source_is_webcam(void)
{
	char v[16];
	if (!setting("SOURCE", v, sizeof(v))) {
		return false;
	}
	if (strcmp(v, "webcam") == 0) {
		return true;
	}
	if (strcmp(v, "script") != 0) {
		U_LOG_W("sim-tracking: SOURCE='%s' is not script|webcam - using script.", v);
	}
	return false;
}

static uint32_t
describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (out_descs != NULL) {
		*out_descs = g_descs;
	}
	return (uint32_t)(sizeof(g_descs) / sizeof(g_descs[0]));
}


/*
 *
 * The provider.
 *
 */

struct sim_tracking
{
	struct os_thread_helper oth;
	bool camera_on;
	char camera_key[XRT_TRACKING_CAMERA_KEY_MAX];
	struct sim_tracking_camera cam;
	bool oth_ready;
	bool running;
	struct sim_tracking_webcam *webcam; //!< SOURCE=webcam while started
	const struct xrt_tracking_sink *sink;
	struct sim_tracking_script script;
	int64_t interval_ns;
};

static struct sim_tracking g_st; // one per process: the runtime loads a provider once

//! One sample at @p t_ns: the script, and (CAMERA=1) the camera's measurement + the eyes mapped back.
static void
make_sample(const struct sim_tracking *st, int64_t t_ns, struct xrt_tracking_viewer_sample *s)
{
	memset(s, 0, sizeof(*s));
	s->struct_size = (uint32_t)sizeof(*s);
	s->capture_ns = t_ns;
	float left[3], right[3], head[3];
	const bool tracking = sim_tracking_script_eval(&st->script, t_ns, left, right, head);
	s->head_orientation[3] = 1.0f;
	s->flags = XRT_TRACKING_SAMPLE_EYES_VALID | XRT_TRACKING_SAMPLE_HEAD_POSE_VALID |
	           (tracking ? XRT_TRACKING_SAMPLE_TRACKING : 0u);
	s->confidence = tracking ? 1.0f : 0.0f;
	if (!st->camera_on) {
		memcpy(s->left_eye, left, sizeof(left));
		memcpy(s->right_eye, right, sizeof(right));
		memcpy(s->head_position, head, sizeof(head));
		return;
	}
	struct xrt_tracking_camera_sample *c = &s->camera;
	snprintf(c->camera_key, sizeof(c->camera_key), "%s", st->camera_key);
	sim_tracking_camera_measure(&st->cam, left, c->left_dir, &c->left_distance_m);
	sim_tracking_camera_measure(&st->cam, right, c->right_dir, &c->right_distance_m);
	s->flags |= XRT_TRACKING_SAMPLE_CAMERA_VALID;
	// What a camera provider reports: its measurements through the pose it BELIEVES.
	sim_tracking_camera_to_display(&st->cam, c->left_dir, c->left_distance_m, s->left_eye);
	sim_tracking_camera_to_display(&st->cam, c->right_dir, c->right_distance_m, s->right_eye);
	for (int i = 0; i < 3; i++) {
		s->head_position[i] = 0.5f * (s->left_eye[i] + s->right_eye[i]);
	}
}

/*!
 * LEGACY (one release, #2303's storage): the first `CAMERA_POSE_<n>` provider
 * setting whose `_CAMERA` is @p camera_key, into @p c's believed pose (+ depth /
 * focal scale). Superseded by the runtime's camera calibration store; read
 * only when the host has no user calibration / placement hint for the camera.
 */
static bool
load_legacy_pose(const char *camera_key, struct sim_tracking_camera *c)
{
	char v[256];
	for (int n = 0; n < SIM_TRACKING_POSE_SLOTS; n++) {
		char key[48], cam[XRT_TRACKING_CAMERA_KEY_MAX];
		snprintf(key, sizeof(key), "CAMERA_POSE_%d_CAMERA", n);
		if (!setting(key, cam, sizeof(cam)) || strcmp(cam, camera_key) != 0) {
			continue;
		}
		snprintf(key, sizeof(key), "CAMERA_POSE_%d", n);
		double t[3], q[4];
		if (!setting(key, v, sizeof(v)) || !sim_tracking_parse_pose(v, t, q)) {
			U_LOG_W("sim-tracking: legacy %s for camera '%s' is missing or malformed - ignored.", key, cam);
			return false;
		}
		memcpy(c->pose_t, t, sizeof(t));
		memcpy(c->pose_q, q, sizeof(q));
		snprintf(key, sizeof(key), "CAMERA_POSE_%d_DEPTH_SCALE", n);
		c->depth_scale = setting_float(key, 1.0f);
		snprintf(key, sizeof(key), "CAMERA_POSE_%d_FOCAL_SCALE", n);
		c->focal_scale = setting_float(key, 1.0f);
		U_LOG_W(
		    "sim-tracking: camera '%s' uses the LEGACY provider setting CAMERA_POSE_%d (deprecated, removed "
		    "next "
		    "release): re-run 'displayxr-cli tracking calibrate apply' to move it into the runtime's camera "
		    "calibration store.",
		    cam, n);
		return true;
	}
	return false;
}

//! Where the believed pose came from.
enum sim_pose_origin
{
	SIM_POSE_HOST,         //!< the runtime's answer (any source)
	SIM_POSE_LEGACY,       //!< the legacy CAMERA_POSE_<n> settings
	SIM_POSE_LEGACY_MODEL, //!< SOURCE=webcam: the legacy bezel-offset model (WEBCAM_OFFSET_MM / no host slot)
	SIM_POSE_LOCAL_PRIOR,  //!< no host slot (an older runtime): the top-centre prior, computed here
};

static void
pose_from_host(const struct xrt_camera_pose *a, struct sim_tracking_camera *c)
{
	for (int i = 0; i < 3; i++) {
		c->pose_t[i] = a->position_m[i];
	}
	for (int i = 0; i < 4; i++) {
		c->pose_q[i] = a->orientation[i];
	}
	c->focal_scale = a->focal_scale > 0.0f ? a->focal_scale : 1.0;
	c->depth_scale = a->viewer_iris_scale > 0.0f ? a->viewer_iris_scale : 1.0;
}

/*!
 * The believed pose of @p camera_key into @p c, by the runtime's precedence:
 * the host answer when it is a real one (display-declared, user calibration
 * or placement hint); else a legacy `CAMERA_POSE_<n>`; else (webcam only) an
 * explicitly set legacy `WEBCAM_OFFSET_MM`; else the host's default prior;
 * else — an older runtime without the slot — the prior computed here.
 */
static enum sim_pose_origin
resolve_pose(const char *camera_key, float display_height_m, bool webcam, struct sim_tracking_camera *c)
{
	c->depth_scale = 1.0;
	c->focal_scale = 1.0;
	struct xrt_camera_pose a;
	memset(&a, 0, sizeof(a));
	a.struct_size = (uint32_t)sizeof(a);
	const bool have_host = XRT_TRACKING_PLUGIN_HOST_HAS_CAMERA_POSE(g_host) &&
	                       g_host->get_camera_pose(g_host, camera_key, NULL, &a) == XRT_SUCCESS &&
	                       a.source != XRT_CAMERA_POSE_SOURCE_NONE;
	enum sim_pose_origin origin;
	char offset_v[64];
	if (have_host && a.source > XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR) {
		origin = SIM_POSE_HOST;
	} else if (load_legacy_pose(camera_key, c)) {
		return SIM_POSE_LEGACY;
	} else if (webcam && !have_host) {
		return SIM_POSE_LEGACY_MODEL; // an older runtime: the model this source always had
	} else if (webcam && setting("WEBCAM_OFFSET_MM", offset_v, sizeof(offset_v))) {
		U_LOG_W(
		    "sim-tracking: camera '%s' uses the LEGACY WEBCAM_OFFSET_MM bezel-offset model (deprecated): "
		    "'displayxr-cli camera calibration set-placement' replaces it.",
		    camera_key);
		return SIM_POSE_LEGACY_MODEL;
	} else if (have_host) {
		origin = SIM_POSE_HOST;
	} else {
		sim_tracking_camera_prior(display_height_m, c->pose_t, c->pose_q);
		U_LOG_W(
		    "sim-tracking: the runtime has no get_camera_pose slot - camera '%s' assumed at the top centre.",
		    camera_key);
		return SIM_POSE_LOCAL_PRIOR;
	}
	pose_from_host(&a, c);
	U_LOG_W(
	    "sim-tracking: camera '%s' pose from the runtime, source %s (rms %.2f mm): (%.4f, %.4f, %.4f) m, q (%.4f, "
	    "%.4f, %.4f, %.4f), focal scale %.4f, viewer iris scale %.4f.",
	    camera_key,
	    a.source == XRT_CAMERA_POSE_SOURCE_DISPLAY_DECLARED   ? "display-declared"
	    : a.source == XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION ? "user-calibration"
	    : a.source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT   ? "placement-hint"
	                                                          : "default-prior",
	    (double)a.rms_mm, c->pose_t[0], c->pose_t[1], c->pose_t[2], c->pose_q[0], c->pose_q[1], c->pose_q[2],
	    c->pose_q[3], c->focal_scale, c->depth_scale);
	return origin;
}

/*!
 * The camera model from settings: the hidden true pose, and the believed pose
 * (@ref resolve_pose). The provider serves the primary screen (spec §4), so
 * it asks for "the screen it serves" (NULL) and the runtime matches any stored
 * screen for this camera.
 */
static void
load_camera(struct sim_tracking *st, float display_height_m)
{
	st->camera_on = setting_on("CAMERA");
	if (!setting("CAMERA_KEY", st->camera_key, sizeof(st->camera_key))) {
		snprintf(st->camera_key, sizeof(st->camera_key), "%s", "sim-tracking:webcam0");
	}
	struct sim_tracking_camera *c = &st->cam;
	memset(c, 0, sizeof(*c));
	// The hidden truth defaults to where an uncalibrated camera is assumed to be.
	sim_tracking_camera_prior(display_height_m, c->true_t, c->true_q);
	sim_tracking_camera_prior(display_height_m, c->pose_t, c->pose_q);
	c->depth_bias = setting_float("CAMERA_DEPTH_BIAS", 1.0f);
	c->depth_scale = 1.0;
	c->focal_scale = 1.0;
	char v[256];
	if (setting("CAMERA_TRUE_POSE", v, sizeof(v)) && !sim_tracking_parse_pose(v, c->true_t, c->true_q)) {
		U_LOG_W("sim-tracking: CAMERA_TRUE_POSE='%s' is not \"tx,ty,tz,qx,qy,qz,qw\" - using the prior.", v);
	}
	if (!st->camera_on) {
		return;
	}
	(void)resolve_pose(st->camera_key, display_height_m, false, c);
}

static void *
run(void *ptr)
{
	struct sim_tracking *st = (struct sim_tracking *)ptr;
	os_thread_helper_name(&st->oth, "sim-tracking");
	os_thread_helper_lock(&st->oth);
	while (os_thread_helper_is_running_locked(&st->oth)) {
		os_thread_helper_unlock(&st->oth);

		struct xrt_tracking_viewer_sample s;
		make_sample(st, (int64_t)os_monotonic_get_ns(), &s);
		st->sink->push(st->sink, &s);

		os_nanosleep(st->interval_ns);
		os_thread_helper_lock(&st->oth);
	}
	os_thread_helper_unlock(&st->oth);
	return NULL;
}

static xrt_result_t
sim_tracking_probe(struct xrt_tracking_plugin_instance **out_inst)
{
	*out_inst = (struct xrt_tracking_plugin_instance *)&g_st;
	if (!setting_on("ENABLE")) {
		U_LOG_I(
		    "sim-tracking: declining — 'displayxr-cli tracking set sim-tracking ENABLE 1' (or "
		    "DXR_TRACKING_SIM_TRACKING_ENABLE=1 in the hosting process) enables the scripted viewer.");
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	if (source_is_webcam() && !sim_tracking_webcam_probe(&g_webcam_settings)) {
		return XRT_ERROR_PROBER_NOT_SUPPORTED; // probe must not say yes without a usable source
	}
	return XRT_SUCCESS;
}

static xrt_result_t
sim_tracking_describe(struct xrt_tracking_plugin_instance *inst, struct xrt_tracking_provider_info *out)
{
	(void)inst;
	if (out == NULL || out->struct_size < offsetof(struct xrt_tracking_provider_info, camera_key)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (source_is_webcam()) {
		sim_tracking_webcam_describe(&g_webcam_settings, out);
		return XRT_SUCCESS;
	}
	out->tracks = XRT_TRACKING_TRACKS_EYE_PAIR | XRT_TRACKING_TRACKS_HEAD_POSE;
	out->typical_latency_ns = 0;
	if (setting_on("CAMERA")) {
		out->tracks |= XRT_TRACKING_TRACKS_CAMERA_SPACE;
		if (out->struct_size >=
		        offsetof(struct xrt_tracking_provider_info, camera_key) + sizeof(out->camera_key) &&
		    !setting("CAMERA_KEY", out->camera_key, sizeof(out->camera_key))) {
			snprintf(out->camera_key, sizeof(out->camera_key), "%s", "sim-tracking:webcam0");
		}
	}
	char v[16];
	enum sim_tracking_script_kind k = SIM_TRACKING_SCRIPT_SWEEP;
	if (setting("SCRIPT", v, sizeof(v))) {
		(void)sim_tracking_parse_script(v, &k);
	}
	snprintf(out->source, sizeof(out->source), "scripted %s (%s)",
	         k == SIM_TRACKING_SCRIPT_OFFSET ? "offset" : "sweep",
	         setting_on("CAMERA") ? "simulated webcam" : "no camera");
	return XRT_SUCCESS;
}

static xrt_result_t
sim_tracking_start(struct xrt_tracking_plugin_instance *inst, const struct xrt_tracking_sink *sink)
{
	struct sim_tracking *st = (struct sim_tracking *)inst;
	if (st == NULL || sink == NULL || sink->push == NULL) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (st->running) {
		return XRT_SUCCESS;
	}

	struct sim_tracking_script *sc = &st->script;
	memset(sc, 0, sizeof(*sc));
	sc->kind = SIM_TRACKING_SCRIPT_SWEEP;
	char v[16];
	if (setting("SCRIPT", v, sizeof(v)) && !sim_tracking_parse_script(v, &sc->kind)) {
		U_LOG_W("sim-tracking: SCRIPT='%s' is not sweep|offset - using sweep.", v);
	}
	// The nominal viewer from the host's display geometry; a sane desk default otherwise.
	sc->nominal[0] = 0.0f;
	sc->nominal[1] = 0.0f;
	sc->nominal[2] = 0.6f;
	float display_height_m = 0.0f;
	if (g_host != NULL &&
	    g_host->struct_size >= offsetof(struct xrt_tracking_plugin_host_iface, get_display_geometry) +
	                               sizeof(g_host->get_display_geometry) &&
	    g_host->get_display_geometry != NULL) {
		struct xrt_tracking_host_display_geometry geo = {0};
		geo.struct_size = (uint32_t)sizeof(geo);
		if (g_host->get_display_geometry(&geo) == XRT_SUCCESS && geo.nominal_viewer_z_m > 0.0f) {
			display_height_m = geo.display_height_m;
			sc->nominal[0] = geo.nominal_viewer_x_m;
			sc->nominal[1] = geo.nominal_viewer_y_m;
			sc->nominal[2] = geo.nominal_viewer_z_m;
		}
	}
	sc->offset[0] = setting_float("OFFSET_X_M", 0.0f);
	sc->offset[1] = setting_float("OFFSET_Y_M", 0.0f);
	sc->offset[2] = setting_float("OFFSET_Z_M", 0.0f);
	sc->amplitude_m = setting_float("AMPLITUDE_M", 0.10f);
	sc->period_ns = setting_ms("PERIOD_MS", 4000) * 1000000LL;
	if (sc->period_ns <= 0) {
		sc->period_ns = 4000LL * 1000000LL;
	}
	sc->ipd_m = setting_float("IPD_MM", 63.0f) / 1000.0f;
	sc->loss_period_ns = setting_ms("LOSS_PERIOD_MS", 0) * 1000000LL;
	sc->epoch_ns = (int64_t)os_monotonic_get_ns();
	int64_t rate = setting_ms("RATE_HZ", 60);
	if (rate < 1 || rate > 1000) {
		rate = 60;
	}
	st->interval_ns = 1000000000LL / rate;
	st->sink = sink;

	if (source_is_webcam()) {
		const float screen_h_m = display_height_m > 0.0f ? display_height_m : 0.194f; // sim default panel
		// The runtime's camera pose (calibration, placement hint or its default prior) maps every
		// measurement; the legacy bezel-offset model runs only on an explicit WEBCAM_OFFSET_MM or
		// an older runtime without the host slot.
		struct sim_tracking_camera pose;
		memset(&pose, 0, sizeof(pose));
		char key[XRT_TRACKING_CAMERA_KEY_MAX];
		enum sim_pose_origin origin = SIM_POSE_LEGACY_MODEL;
		if (sim_tracking_webcam_camera_key(&g_webcam_settings, key, sizeof(key))) {
			origin = resolve_pose(key, screen_h_m, true, &pose);
		}
		const bool calibrated = origin != SIM_POSE_LEGACY_MODEL;
		st->webcam = sim_tracking_webcam_start(&g_webcam_settings, screen_h_m, sc->nominal, sc->ipd_m,
		                                       st->interval_ns, calibrated ? &pose : NULL, sink);
		if (st->webcam == NULL) {
			st->sink = NULL;
			return XRT_ERROR_PROBER_NOT_SUPPORTED; // the core WARNed why
		}
		st->running = true;
		U_LOG_W(
		    "sim-tracking: started — WEBCAM viewer (nominal (%.3f, %.3f, %.3f) m, panel height %.3f m, %s, "
		    "%lld "
		    "Hz poll).",
		    (double)sc->nominal[0], (double)sc->nominal[1], (double)sc->nominal[2], (double)screen_h_m,
		    calibrated ? "camera pose from the runtime" : "legacy bezel-offset model", (long long)rate);
		return XRT_SUCCESS;
	}

	load_camera(st, display_height_m);

	// One sample synchronously, so the first locate after start() has data.
	{
		struct xrt_tracking_viewer_sample s;
		make_sample(st, sc->epoch_ns, &s);
		sink->push(sink, &s);
	}

	if (!st->oth_ready) {
		if (os_thread_helper_init(&st->oth) != 0) {
			return XRT_ERROR_ALLOCATION;
		}
		st->oth_ready = true;
	}
	if (os_thread_helper_start(&st->oth, run, st) != 0) {
		return XRT_ERROR_ALLOCATION;
	}
	st->running = true;
	U_LOG_W(
	    "sim-tracking: started — SCRIPTED viewer (%s) around (%.3f, %.3f, %.3f) m, offset (%.3f, %.3f, %.3f), "
	    "%lld Hz.",
	    sc->kind == SIM_TRACKING_SCRIPT_OFFSET ? "offset" : "sweep", (double)sc->nominal[0], (double)sc->nominal[1],
	    (double)sc->nominal[2], (double)sc->offset[0], (double)sc->offset[1], (double)sc->offset[2],
	    (long long)rate);
	return XRT_SUCCESS;
}

static void
sim_tracking_stop(struct xrt_tracking_plugin_instance *inst)
{
	struct sim_tracking *st = (struct sim_tracking *)inst;
	if (st == NULL || !st->running) {
		return;
	}
	if (st->webcam != NULL) {
		sim_tracking_webcam_stop(&st->webcam); // returns after the last push; closes the camera
		st->running = false;
		st->sink = NULL;
		return;
	}
	os_thread_helper_stop_and_wait(&st->oth);
	// A stopped helper cannot be restarted: re-init it for the next start().
	os_thread_helper_destroy(&st->oth);
	st->oth_ready = false;
	st->running = false;
	st->sink = NULL;
}

static enum xrt_tracking_provider_status
sim_tracking_get_status(struct xrt_tracking_plugin_instance *inst)
{
	struct sim_tracking *st = (struct sim_tracking *)inst;
	if (st != NULL && st->running && st->webcam != NULL) {
		return sim_tracking_webcam_status(st->webcam);
	}
	return (st != NULL && st->running) ? XRT_TRACKING_PROVIDER_STATUS_RUNNING
	                                   : XRT_TRACKING_PROVIDER_STATUS_STOPPED;
}

static void
sim_tracking_destroy(struct xrt_tracking_plugin_instance *inst)
{
	sim_tracking_stop(inst);
}

static struct xrt_tracking_plugin_iface g_iface = {
    .struct_size = sizeof(struct xrt_tracking_plugin_iface),
    .reserved_0 = 0,
    .id = SIM_TRACKING_ID,
    .display_name = "DisplayXR Sim Tracking",
    .vendor = "DisplayXR",
    .version = NULL,
    .probe = sim_tracking_probe,
    .describe = sim_tracking_describe,
    .start = sim_tracking_start,
    .stop = sim_tracking_stop,
    .get_status = sim_tracking_get_status,
    .destroy = sim_tracking_destroy,
    .describe_settings = describe_settings,
};

XRT_TRACKING_PLUGIN_EXPORT xrt_result_t
xrtTrackingPluginNegotiate(uint32_t runtime_api_version,
                           const struct xrt_tracking_plugin_host_iface *host,
                           struct xrt_tracking_plugin_iface **out_iface,
                           uint32_t *out_plugin_api_version)
{
	*out_plugin_api_version = XRT_TRACKING_PLUGIN_API_VERSION_CURRENT;
	if (runtime_api_version != XRT_TRACKING_PLUGIN_API_VERSION_CURRENT) {
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	g_host = host;
	*out_iface = &g_iface;
	return XRT_SUCCESS;
}
