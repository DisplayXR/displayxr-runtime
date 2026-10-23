// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_tracking — the in-tree reference viewer-tracking provider (ADR-054 §4.1, N6 Part 2).
 *
 * Implements `xrt/xrt_tracking_plugin.h`. A scripted viewer with no camera:
 * the hardware-free source CI uses to prove that `xrLocateViews` follows a
 * provider on a display that does not track (sim_display, EXTERNAL_OK).
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
 * runtime's sink stamped with their capture time. The webcam tracker core of
 * #1855 is still in sim_display (`SIM_DISPLAY_WEBCAM_TRACKING`); moving it in
 * here as `SCRIPT=webcam` is the follow-up (#2144 stack part 3).
 *
 * @ingroup drv_sim_tracking
 */

#include "xrt/xrt_tracking_plugin.h"
#include "xrt/xrt_results.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include "sim_tracking_script.h"

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
	bool oth_ready;
	bool running;
	const struct xrt_tracking_sink *sink;
	struct sim_tracking_script script;
	int64_t interval_ns;
};

static struct sim_tracking g_st; // one per process: the runtime loads a provider once

static void *
run(void *ptr)
{
	struct sim_tracking *st = (struct sim_tracking *)ptr;
	os_thread_helper_name(&st->oth, "sim-tracking");
	os_thread_helper_lock(&st->oth);
	while (os_thread_helper_is_running_locked(&st->oth)) {
		os_thread_helper_unlock(&st->oth);

		struct xrt_tracking_viewer_sample s;
		memset(&s, 0, sizeof(s));
		s.struct_size = (uint32_t)sizeof(s);
		s.capture_ns = (int64_t)os_monotonic_get_ns();
		const bool tracking =
		    sim_tracking_script_eval(&st->script, s.capture_ns, s.left_eye, s.right_eye, s.head_position);
		s.head_orientation[3] = 1.0f;
		s.flags = XRT_TRACKING_SAMPLE_EYES_VALID | XRT_TRACKING_SAMPLE_HEAD_POSE_VALID |
		          (tracking ? XRT_TRACKING_SAMPLE_TRACKING : 0u);
		s.confidence = tracking ? 1.0f : 0.0f;
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
	return XRT_SUCCESS;
}

static xrt_result_t
sim_tracking_describe(struct xrt_tracking_plugin_instance *inst, struct xrt_tracking_provider_info *out)
{
	(void)inst;
	if (out == NULL || out->struct_size < offsetof(struct xrt_tracking_provider_info, camera_key)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	out->tracks = XRT_TRACKING_TRACKS_EYE_PAIR | XRT_TRACKING_TRACKS_HEAD_POSE;
	out->typical_latency_ns = 0;
	char v[16];
	enum sim_tracking_script_kind k = SIM_TRACKING_SCRIPT_SWEEP;
	if (setting("SCRIPT", v, sizeof(v))) {
		(void)sim_tracking_parse_script(v, &k);
	}
	snprintf(out->source, sizeof(out->source), "scripted %s (no camera)",
	         k == SIM_TRACKING_SCRIPT_OFFSET ? "offset" : "sweep");
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
	if (g_host != NULL &&
	    g_host->struct_size >= offsetof(struct xrt_tracking_plugin_host_iface, get_display_geometry) +
	                               sizeof(g_host->get_display_geometry) &&
	    g_host->get_display_geometry != NULL) {
		struct xrt_tracking_host_display_geometry geo = {0};
		geo.struct_size = (uint32_t)sizeof(geo);
		if (g_host->get_display_geometry(&geo) == XRT_SUCCESS && geo.nominal_viewer_z_m > 0.0f) {
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

	// One sample synchronously, so the first locate after start() has data.
	{
		struct xrt_tracking_viewer_sample s;
		memset(&s, 0, sizeof(s));
		s.struct_size = (uint32_t)sizeof(s);
		s.capture_ns = sc->epoch_ns;
		const bool tracking =
		    sim_tracking_script_eval(sc, s.capture_ns, s.left_eye, s.right_eye, s.head_position);
		s.head_orientation[3] = 1.0f;
		s.flags = XRT_TRACKING_SAMPLE_EYES_VALID | XRT_TRACKING_SAMPLE_HEAD_POSE_VALID |
		          (tracking ? XRT_TRACKING_SAMPLE_TRACKING : 0u);
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
