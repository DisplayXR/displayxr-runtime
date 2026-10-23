// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Viewer arbitration + composed tracking advertisement (ADR-054 §4.1, N6 Part 2).
 * @ingroup target_common
 */

#include "target_viewer_tracking.h"
#include "target_tracking_plugin_loader.h"
#include "target_plugin_loader.h"

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_tracking_plugin.h"

#include "util/u_eye_history.h"
#include "util/u_logging.h"

#include "os/os_threading.h"

#include <stdio.h>
#include <string.h>


/*
 *
 * Pure policy.
 *
 */

const char *
target_viewer_source_str(enum xrt_viewer_source s)
{
	switch (s) {
	case XRT_VIEWER_SOURCE_DP: return "dp";
	case XRT_VIEWER_SOURCE_PROVIDER: return "provider";
	case XRT_VIEWER_SOURCE_NOMINAL:
	default: return "nominal";
	}
}

const char *
target_viewer_policy_str(uint32_t policy)
{
	switch (policy) {
	case XRT_PLUGIN_VIEWER_POLICY_OWN: return "OWN";
	case XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK: return "EXTERNAL_OK";
	case XRT_PLUGIN_VIEWER_POLICY_FIXED:
	default: return "FIXED";
	}
}

void
target_viewer_source_label(const struct target_viewer_decision *d, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return;
	}
	if (d != NULL && d->source == XRT_VIEWER_SOURCE_PROVIDER) {
		snprintf(out, cap, "provider:%s", d->provider_id);
	} else {
		snprintf(out, cap, "%s", target_viewer_source_str(d != NULL ? d->source : XRT_VIEWER_SOURCE_NOMINAL));
	}
}

void
target_viewer_arbitrate(const struct target_viewer_inputs *in, struct target_viewer_decision *out)
{
	memset(out, 0, sizeof(*out));
	const uint32_t policy = in->dp_policy;
	out->dp_policy = policy;

	// (a) A display that tracks always wins on its screen — even while its
	// tracker has lost the face: weave and rendered views must come from the
	// same eyes, so no second source is ever spliced in.
	if (in->dp_tracks) {
		out->source = XRT_VIEWER_SOURCE_DP;
		snprintf(out->reason, sizeof(out->reason), "display tracks the viewer on this screen");
		return;
	}
	// The display decides the viewer itself without advertising tracking.
	if (policy == XRT_PLUGIN_VIEWER_POLICY_OWN) {
		out->source = XRT_VIEWER_SOURCE_DP;
		snprintf(out->reason, sizeof(out->reason), "display owns the viewer on this screen (policy OWN)");
		return;
	}
	if (policy != XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK) {
		out->source = XRT_VIEWER_SOURCE_NOMINAL;
		snprintf(out->reason, sizeof(out->reason), "display is FIXED");
		return;
	}
	if (!in->hosted) {
		out->source = XRT_VIEWER_SOURCE_NOMINAL;
		snprintf(out->reason, sizeof(out->reason),
		         "viewer-tracking providers are not hosted in the service yet");
		return;
	}
	// (b) The best enabled provider with a usable source.
	if (in->provider_usable && in->provider_id != NULL && in->provider_id[0] != '\0') {
		out->source = XRT_VIEWER_SOURCE_PROVIDER;
		snprintf(out->provider_id, sizeof(out->provider_id), "%s", in->provider_id);
		return;
	}
	// (c) Nominal.
	out->source = XRT_VIEWER_SOURCE_NOMINAL;
	snprintf(out->reason, sizeof(out->reason), "no source");
}

void
target_viewer_compose_advertisement(uint32_t dp_supported,
                                    uint32_t dp_default,
                                    const struct target_viewer_decision *d,
                                    uint32_t *out_supported,
                                    uint32_t *out_default,
                                    bool *out_mark_modes)
{
	*out_supported = dp_supported;
	*out_default = dp_default;
	*out_mark_modes = false;
	if (d != NULL && d->source == XRT_VIEWER_SOURCE_PROVIDER && dp_supported == 0) {
		*out_supported = TARGET_VIEWER_MODE_MANAGED_BIT | TARGET_VIEWER_MODE_MANUAL_BIT;
		*out_default = 0u; // MANAGED
		*out_mark_modes = true;
	}
}


/*
 *
 * Glue state.
 *
 */

static bool g_service_process = false;

static struct os_mutex g_mutex;
static bool g_mutex_ready = false;

static bool g_have_decision = false;
static struct target_viewer_decision g_decision;
static int g_provider_index = -1; //!< resident slot the decision names

static bool g_started = false;
static bool g_ring_ready = false;
static struct u_eye_history g_ring;
static struct xrt_tracking_sink g_sink;
static bool g_warned_start_failed = false;

static void
lock_init_once(void)
{
	if (!g_mutex_ready) {
		g_mutex_ready = os_mutex_init(&g_mutex) == 0;
	}
}

static void
lock(void)
{
	lock_init_once();
	if (g_mutex_ready) {
		os_mutex_lock(&g_mutex);
	}
}

static void
unlock(void)
{
	if (g_mutex_ready) {
		os_mutex_unlock(&g_mutex);
	}
}

void
target_viewer_tracking_set_service_process(bool service)
{
	g_service_process = service;
}

void
target_viewer_tracking_evaluate(const struct xrt_plugin_iface *plugin,
                                struct xrt_plugin_instance *inst,
                                uint32_t dp_supported,
                                struct target_viewer_decision *out)
{
	struct target_viewer_inputs in = {0};
	in.dp_tracks = dp_supported != 0;
	in.dp_policy = xrt_plugin_iface_viewer_policy(plugin, inst, NULL);
	in.hosted = !g_service_process;
	// The scan runs only when it can matter: a display that tracks, is FIXED /
	// OWN, or a service process never loads a tracking provider.
	if (!in.dp_tracks && in.dp_policy == XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK && in.hosted &&
	    target_tracking_plugin_get_count() > 0) {
		in.provider_usable = true;
		in.provider_id = target_tracking_plugin_get_row_id(0); // rank order: preferred, then ProbeOrder
	}
	target_viewer_arbitrate(&in, out);
}

void
target_viewer_tracking_apply(struct xrt_system_compositor_info *info,
                             struct xrt_device *head,
                             const struct xrt_plugin_iface *plugin,
                             const struct xrt_plugin_display_info *pdi)
{
	if (info == NULL || pdi == NULL) {
		return;
	}
	// The geometry first: a provider started later reads it from the host.
	struct xrt_tracking_host_display_geometry geo = {0};
	geo.struct_size = (uint32_t)sizeof(geo);
	geo.display_width_m = pdi->display_width_m;
	geo.display_height_m = pdi->display_height_m;
	geo.nominal_viewer_x_m = pdi->nominal_viewer_x_m;
	geo.nominal_viewer_y_m = pdi->nominal_viewer_y_m;
	geo.nominal_viewer_z_m = pdi->nominal_viewer_z_m;
	target_tracking_plugin_set_display_geometry(&geo);

	struct target_viewer_decision d;
	target_viewer_tracking_evaluate(plugin, target_plugin_get_active_instance(), pdi->supported_eye_tracking_modes,
	                                &d);

	uint32_t supported = 0, def = 0;
	bool mark = false;
	target_viewer_compose_advertisement(pdi->supported_eye_tracking_modes, pdi->default_eye_tracking_mode, &d,
	                                    &supported, &def, &mark);
	info->supported_eye_tracking_modes = supported;
	info->default_eye_tracking_mode = def;
	if (mark && head != NULL) {
		for (uint32_t mi = 0; mi < head->rendering_mode_count; mi++) {
			struct xrt_rendering_mode *m = &head->rendering_modes[mi];
			if (m->hardware_display_3d || m->view_count > 1) {
				m->mode_flags |= XRT_RENDERING_MODE_FLAG_HAS_TRACKING;
			}
		}
	}

	lock();
	const bool changed =
	    !g_have_decision || g_decision.source != d.source || strcmp(g_decision.provider_id, d.provider_id) != 0;
	g_decision = d;
	g_have_decision = true;
	g_provider_index = d.source == XRT_VIEWER_SOURCE_PROVIDER ? 0 : -1;
	unlock();

	if (changed) {
		char label[96];
		target_viewer_source_label(&d, label, sizeof(label));
		U_LOG_W("viewer tracking (ADR-054 N6): primary screen viewer source = %s (display policy %s%s%s)%s",
		        label, target_viewer_policy_str(d.dp_policy), d.reason[0] ? "; " : "", d.reason,
		        mark ? " — XR_DXR_display_info advertises MANAGED|MANUAL, 3D modes HAS_TRACKING (composed)"
		             : "");
	}
}

bool
target_viewer_tracking_get_decision(struct target_viewer_decision *out)
{
	lock();
	const bool have = g_have_decision;
	if (have && out != NULL) {
		*out = g_decision;
	}
	unlock();
	return have;
}


/*
 *
 * The provider sink + the xrt_viewer_tracking implementation.
 *
 */

static void
sink_push(const struct xrt_tracking_sink *sink, const struct xrt_tracking_viewer_sample *s)
{
	(void)sink;
	if (s == NULL || s->struct_size < offsetof(struct xrt_tracking_viewer_sample, head_position) ||
	    (s->flags & XRT_TRACKING_SAMPLE_EYES_VALID) == 0 || !g_ring_ready) {
		return;
	}
	struct xrt_eye_positions e;
	memset(&e, 0, sizeof(e));
	e.count = 2;
	e.eyes[0] = (struct xrt_eye_position){s->left_eye[0], s->left_eye[1], s->left_eye[2]};
	e.eyes[1] = (struct xrt_eye_position){s->right_eye[0], s->right_eye[1], s->right_eye[2]};
	e.valid = true;
	e.is_tracking = (s->flags & XRT_TRACKING_SAMPLE_TRACKING) != 0;
	e.timestamp_ns = s->capture_ns;
	(void)u_eye_history_push(&g_ring, &e, s->capture_ns);
}

//! Caller holds the lock.
static void
start_locked(void)
{
	if (g_started || g_provider_index < 0) {
		return;
	}
	const struct xrt_tracking_plugin_iface *iface = target_tracking_plugin_get_iface(g_provider_index);
	if (iface == NULL) {
		return;
	}
	if (!g_ring_ready) {
		u_eye_history_init(&g_ring);
		g_ring_ready = true;
	}
	memset(&g_sink, 0, sizeof(g_sink));
	g_sink.struct_size = (uint32_t)sizeof(g_sink);
	g_sink.push = sink_push;
	const xrt_result_t xret = iface->start(target_tracking_plugin_get_instance(g_provider_index), &g_sink);
	if (xret != XRT_SUCCESS) {
		if (!g_warned_start_failed) {
			g_warned_start_failed = true;
			U_LOG_W("viewer tracking: provider '%s' failed to start (%d) — views stay nominal.",
			        target_tracking_plugin_get_row_id(g_provider_index), (int)xret);
		}
		return;
	}
	g_started = true;
	U_LOG_W("viewer tracking: provider '%s' started on demand (first viewer request).",
	        target_tracking_plugin_get_row_id(g_provider_index));
}

static enum xrt_viewer_source
vt_get_source(struct xrt_viewer_tracking *vt)
{
	(void)vt;
	lock();
	const enum xrt_viewer_source s = g_have_decision ? g_decision.source : XRT_VIEWER_SOURCE_DP;
	unlock();
	return s;
}

static bool
vt_get_eyes_at(struct xrt_viewer_tracking *vt, int64_t target_ns, struct xrt_eye_positions *out)
{
	(void)vt;
	if (out == NULL) {
		return false;
	}
	lock();
	if (!g_have_decision || g_decision.source != XRT_VIEWER_SOURCE_PROVIDER) {
		unlock();
		return false;
	}
	start_locked();
	const bool started = g_started;
	unlock();
	if (!started) {
		return false;
	}
	struct xrt_eye_positions e;
	memset(&e, 0, sizeof(e));
	if (u_eye_history_get(&g_ring, target_ns, &e) == U_EYE_HISTORY_RESULT_EMPTY) {
		return false;
	}
	*out = e;
	return true;
}

static struct xrt_viewer_tracking g_vt = {
    .get_source = vt_get_source,
    .get_eyes_at = vt_get_eyes_at,
};

struct xrt_viewer_tracking *
target_viewer_tracking_get_iface(void)
{
	return &g_vt;
}

bool
target_viewer_tracking_provider_running(uint32_t *out_status)
{
	lock();
	const bool started = g_started;
	const int idx = g_provider_index;
	unlock();
	uint32_t st = XRT_TRACKING_PROVIDER_STATUS_STOPPED;
	if (started && idx >= 0) {
		const struct xrt_tracking_plugin_iface *iface = target_tracking_plugin_get_iface(idx);
		st = XRT_TRACKING_PLUGIN_IFACE_HAS(iface, get_status)
		         ? (uint32_t)iface->get_status(target_tracking_plugin_get_instance(idx))
		         : (uint32_t)XRT_TRACKING_PROVIDER_STATUS_RUNNING;
	}
	if (out_status != NULL) {
		*out_status = st;
	}
	return started;
}

void
target_viewer_tracking_shutdown(void)
{
	lock();
	if (g_started && g_provider_index >= 0) {
		const struct xrt_tracking_plugin_iface *iface = target_tracking_plugin_get_iface(g_provider_index);
		if (iface != NULL) {
			iface->stop(target_tracking_plugin_get_instance(g_provider_index));
		}
		g_started = false;
	}
	if (g_ring_ready) {
		u_eye_history_clear(&g_ring);
	}
	g_have_decision = false;
	g_provider_index = -1;
	unlock();
}
