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
#include "xrt/xrt_screen.h"
#include "xrt/xrt_tracking_plugin.h"

#include "util/u_eye_history.h"
#include "util/u_logging.h"

#include "os/os_threading.h"
#include "os/os_time.h"

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
	out->dp_tracks = in->dp_tracks;
	const bool usable = in->hosted && in->provider_usable && in->provider_id != NULL && in->provider_id[0] != '\0';
	out->provider_available = usable || (in->hosted && in->provider_enabled);

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
		snprintf(out->reason, sizeof(out->reason), "viewer-tracking providers are not hosted in this process");
		return;
	}
	// (b) The best enabled provider with a usable source.
	out->provider_available = usable;
	if (usable) {
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

//! Metres per desktop pixel of @p s, borrowing @p other's when @p s has no size; false when neither has one.
static bool
screen_pitch(const struct target_viewer_screen_in *s,
             const struct target_viewer_screen_in *other,
             double *out_px,
             double *out_py)
{
	if (s->desktop_width == 0 || s->desktop_height == 0) {
		return false;
	}
	if (s->width_m > 0.0f && s->height_m > 0.0f) {
		*out_px = (double)s->width_m / (double)s->desktop_width;
		*out_py = (double)s->height_m / (double)s->desktop_height;
		return true;
	}
	if (other != NULL && other->desktop_width > 0 && other->desktop_height > 0 && other->width_m > 0.0f &&
	    other->height_m > 0.0f) {
		*out_px = (double)other->width_m / (double)other->desktop_width;
		*out_py = (double)other->height_m / (double)other->desktop_height;
		return true;
	}
	return false;
}

bool
target_viewer_map_offset(const struct target_viewer_screen_in *home,
                         const struct target_viewer_screen_in *to,
                         float out_offset_m[3])
{
	if (home == NULL || to == NULL || out_offset_m == NULL) {
		return false;
	}
	if (home == to || home->screen_id == to->screen_id) {
		out_offset_m[0] = out_offset_m[1] = out_offset_m[2] = 0.0f;
		return true;
	}
	double hpx, hpy, tpx, tpy;
	if (!screen_pitch(home, to, &hpx, &hpy) || !screen_pitch(to, home, &tpx, &tpy)) {
		return false;
	}
	// Desktop pixels: the home centre, measured from the target centre (y down).
	const double hcx = (double)home->desktop_left + (double)home->desktop_width * 0.5;
	const double hcy = (double)home->desktop_top + (double)home->desktop_height * 0.5;
	const double tcx = (double)to->desktop_left + (double)to->desktop_width * 0.5;
	const double tcy = (double)to->desktop_top + (double)to->desktop_height * 0.5;
	// The vector between the centres splits at the seam: the part inside each
	// screen's half-extent is read at that screen's own pitch (comp_ws_map_pose
	// reads the whole vector at the TARGET's pitch for a placed window; for a
	// physical point that is right only at equal pitches, so each half is
	// weighed by the screen it lies on). Along an axis where the rects overlap
	// (side by side: y), the target's pitch alone.
	double dx_px = hcx - tcx;
	double dy_px = hcy - tcy;
	double ox, oy;
	{
		const double half_t = (double)to->desktop_width * 0.5;
		const double half_h = (double)home->desktop_width * 0.5;
		const double adx = dx_px < 0 ? -dx_px : dx_px;
		if (adx >= half_t + half_h - 0.5) {
			// Disjoint along x: each half-extent at its own pitch, any gap at the target's.
			const double gap = adx - half_t - half_h;
			const double m = half_t * tpx + half_h * hpx + (gap > 0 ? gap * tpx : 0.0);
			ox = dx_px < 0 ? -m : m;
		} else {
			ox = dx_px * tpx;
		}
	}
	{
		const double half_t = (double)to->desktop_height * 0.5;
		const double half_h = (double)home->desktop_height * 0.5;
		const double ady = dy_px < 0 ? -dy_px : dy_px;
		if (ady >= half_t + half_h - 0.5) {
			const double gap = ady - half_t - half_h;
			const double m = half_t * tpy + half_h * hpy + (gap > 0 ? gap * tpy : 0.0);
			oy = dy_px < 0 ? -m : m;
		} else {
			oy = dy_px * tpy;
		}
	}
	out_offset_m[0] = (float)ox;
	out_offset_m[1] = (float)-oy; // +y up in display space, desktop y down
	out_offset_m[2] = 0.0f;
	return true;
}

uint32_t
target_viewer_arbitrate_screens(const struct target_viewer_screen_in *screens,
                                uint32_t count,
                                const struct target_viewer_provider_in *provider,
                                struct target_viewer_screen_decision *out)
{
	if (screens == NULL || out == NULL) {
		return 0;
	}
	if (count > TARGET_VIEWER_MAX_SCREENS) {
		count = TARGET_VIEWER_MAX_SCREENS;
	}
	const struct target_viewer_screen_in *home = NULL;
	for (uint32_t i = 0; i < count && home == NULL; i++) {
		if (screens[i].is_primary) {
			home = &screens[i];
		}
	}
	if (home == NULL && count > 0) {
		home = &screens[0];
	}

	for (uint32_t i = 0; i < count; i++) {
		const struct target_viewer_screen_in *s = &screens[i];
		struct target_viewer_screen_decision *o = &out[i];
		memset(o, 0, sizeof(*o));
		o->screen_id = s->screen_id;
		o->is_primary = s == home;

		struct target_viewer_inputs in = {0};
		in.dp_tracks = s->has_dp && s->dp_tracks;
		in.dp_policy = s->has_dp ? s->dp_policy : (uint32_t)XRT_PLUGIN_VIEWER_POLICY_FIXED;
		in.hosted = provider != NULL && provider->hosted;
		in.provider_usable = provider != NULL && provider->usable;
		in.provider_id = provider != NULL ? provider->id : NULL;
		in.provider_enabled = provider != NULL && (provider->enabled || provider->usable);
		target_viewer_arbitrate(&in, &o->d);
		if (!s->has_dp) {
			// A plain monitor: nothing weaves there, so nothing to follow.
			snprintf(o->d.reason, sizeof(o->d.reason), "no display processor on this screen");
			continue;
		}
		if (o->d.source != XRT_VIEWER_SOURCE_PROVIDER || o->is_primary) {
			continue;
		}
		// The provider reports in the home screen's display space: translate.
		if (!target_viewer_map_offset(home, s, o->offset_m)) {
			o->d.source = XRT_VIEWER_SOURCE_NOMINAL;
			o->d.provider_id[0] = '\0';
			snprintf(o->d.reason, sizeof(o->d.reason),
			         "no desktop geometry to map the provider's samples onto this screen");
			continue;
		}
		snprintf(o->d.reason, sizeof(o->d.reason),
		         "provider samples mapped from the primary screen (desktop layout, coplanar)");
	}
	return count;
}

const char *
target_viewer_provider_state_str(enum target_viewer_provider_state s)
{
	switch (s) {
	case TARGET_VIEWER_PROVIDER_SERVING: return "serving";
	case TARGET_VIEWER_PROVIDER_IDLE: return "idle";
	case TARGET_VIEWER_PROVIDER_PAUSED: return "paused";
	case TARGET_VIEWER_PROVIDER_STANDBY: return "standby";
	case TARGET_VIEWER_PROVIDER_NO_SOURCE: return "no_source";
	case TARGET_VIEWER_PROVIDER_NONE:
	default: return "none";
	}
}

enum target_viewer_provider_state
target_viewer_provider_state_of(const struct target_viewer_decision *d, bool started, bool producing)
{
	if (d == NULL) {
		return TARGET_VIEWER_PROVIDER_NONE;
	}
	if (d->source == XRT_VIEWER_SOURCE_PROVIDER) {
		if (!started) {
			return TARGET_VIEWER_PROVIDER_IDLE;
		}
		return producing ? TARGET_VIEWER_PROVIDER_SERVING : TARGET_VIEWER_PROVIDER_PAUSED;
	}
	if (d->source == XRT_VIEWER_SOURCE_DP) {
		// Outranked by the display (it tracks, or owns the viewer): standby
		// only when a provider could otherwise have served.
		return d->provider_available ? TARGET_VIEWER_PROVIDER_STANDBY : TARGET_VIEWER_PROVIDER_NONE;
	}
	// Nominal: "no source" only where a provider would be taken.
	if (d->dp_policy == XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK && !d->dp_tracks && !d->provider_available) {
		return TARGET_VIEWER_PROVIDER_NO_SOURCE;
	}
	return TARGET_VIEWER_PROVIDER_NONE;
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
static struct target_viewer_decision g_decision; //!< the primary screen's (static per instance)
static int g_provider_index = -1;                //!< resident slot a decision names

//! Per-screen decisions of the last published topology (none until one was published).
static bool g_have_screens = false;
static uint32_t g_screen_count = 0;
static struct target_viewer_screen_decision g_screens[TARGET_VIEWER_MAX_SCREENS];
static uint64_t g_primary_screen_id = 0; //!< monitor id of the primary in g_screens (0 = unknown)

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

//! -1 = not counted yet; else whether an enabled provider row is registered (listed, never loaded).
static int g_enabled_rows = -1;

//! Is an enabled row registered? Enumerates without loading (cached; @p refresh re-counts).
static bool
enabled_rows_exist(bool refresh)
{
	if (target_tracking_plugin_disabled_by_env()) {
		return false;
	}
	if (g_enabled_rows < 0 || refresh) {
		struct target_tracking_plugin_desc descs[16];
		const int n = target_tracking_plugin_enumerate(descs, 16);
		int enabled = 0;
		for (int i = 0; i < n; i++) {
			enabled += descs[i].enabled ? 1 : 0;
		}
		g_enabled_rows = enabled > 0 ? 1 : 0;
	}
	return g_enabled_rows > 0;
}

/*!
 * The provider side. The tracking class scan (loads + probes) runs only when
 * a screen can take a provider (@p needed). @p count_rows (status only) also
 * counts the registered rows without loading them, so a display that tracks
 * — no scan ran — still reports a provider it outranks as "standby".
 */
static void
provider_inputs(bool needed, bool count_rows, struct target_viewer_provider_in *out)
{
	memset(out, 0, sizeof(*out));
	out->hosted = true; // in-process runtime and service both host (one provider per view-computing process)
	if (needed && target_tracking_plugin_get_count() > 0) {
		out->usable = true;
		out->id = target_tracking_plugin_get_row_id(0); // rank order: preferred, then ProbeOrder
	}
	out->enabled = out->usable || (count_rows && enabled_rows_exist(false));
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
	in.hosted = true;
	// The scan runs only when it can matter: a display that tracks, or is
	// FIXED / OWN, never loads a tracking provider for this screen.
	struct target_viewer_provider_in p;
	provider_inputs(!in.dp_tracks && in.dp_policy == XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK, false, &p);
	in.provider_usable = p.usable;
	in.provider_id = p.id;
	in.provider_enabled = p.enabled;
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
	if (d.source == XRT_VIEWER_SOURCE_PROVIDER) {
		g_provider_index = 0;
	}
	unlock();

	if (changed) {
		char label[96];
		target_viewer_source_label(&d, label, sizeof(label));
		U_LOG_W("viewer tracking (ADR-054 N6): primary screen viewer source = %s (display policy %s%s%s)%s%s",
		        label, target_viewer_policy_str(d.dp_policy), d.reason[0] ? "; " : "", d.reason,
		        mark ? " — XR_DXR_display_info advertises MANAGED|MANUAL, 3D modes HAS_TRACKING (composed)"
		             : "",
		        g_service_process && d.source == XRT_VIEWER_SOURCE_PROVIDER
		            ? " — hosted in the service, shared by every IPC client"
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

//! The registry entry of @p id, NULL when none.
static const struct xrt_dp_registry_entry *
registry_entry(const struct xrt_dp_factory_registry *reg, uint64_t id)
{
	if (reg == NULL) {
		return NULL;
	}
	for (uint32_t i = 0; i < reg->entry_count && i < XRT_DP_REGISTRY_MAX_ENTRIES; i++) {
		if (reg->entries[i].monitor_id == id) {
			return &reg->entries[i];
		}
	}
	return NULL;
}

uint32_t
target_viewer_screens_from_list(const struct xrt_screen_list *list,
                                const struct xrt_dp_factory_registry *reg,
                                struct target_viewer_screen_in *out,
                                uint32_t max)
{
	if (list == NULL || out == NULL) {
		return 0;
	}
	uint32_t n = 0;
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX && n < max; i++) {
		const struct xrt_screen *sc = &list->screens[i];
		struct target_viewer_screen_in *s = &out[n++];
		memset(s, 0, sizeof(*s));
		s->screen_id = sc->id;
		s->is_primary = (sc->flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0;
		s->has_dp = sc->plugin_id[0] != '\0';
		s->dp_tracks = sc->info.supported_eye_tracking_modes != 0;
		s->desktop_left = sc->desktop_left;
		s->desktop_top = sc->desktop_top;
		s->desktop_width = sc->desktop_width;
		s->desktop_height = sc->desktop_height;
		s->width_m = sc->info.width_m;
		s->height_m = sc->info.height_m;

		// THAT screen's DP: the plug-in that won the monitor, asked about the
		// monitor (never the primary's answer reused).
		s->dp_policy = XRT_PLUGIN_VIEWER_POLICY_FIXED;
		const struct xrt_dp_registry_entry *e = registry_entry(reg, sc->id);
		if (e != NULL && e->owning_iface != NULL) {
			struct xrt_display_descriptor desc;
			memset(&desc, 0, sizeof(desc));
			const bool have_desc = target_plugin_get_monitor_record(sc->id, &desc, NULL);
			s->dp_policy = xrt_plugin_iface_viewer_policy((const struct xrt_plugin_iface *)e->owning_iface,
			                                              (struct xrt_plugin_instance *)e->owning_instance,
			                                              have_desc ? &desc : NULL);
		} else if (s->is_primary) {
			s->dp_policy = xrt_plugin_iface_viewer_policy(target_plugin_get_active(),
			                                              target_plugin_get_active_instance(), NULL);
		}
	}
	return n;
}

//! Evaluate @p list now (no start). The primary takes @p primary when given.
static uint32_t
evaluate_screens(const struct xrt_screen_list *list,
                 const struct xrt_dp_factory_registry *reg,
                 const struct target_viewer_decision *primary,
                 bool count_rows,
                 struct target_viewer_screen_decision *out,
                 uint32_t max)
{
	struct target_viewer_screen_in in[TARGET_VIEWER_MAX_SCREENS];
	const uint32_t n = target_viewer_screens_from_list(
	    list, reg, in, max < TARGET_VIEWER_MAX_SCREENS ? max : TARGET_VIEWER_MAX_SCREENS);
	bool needed = primary != NULL && primary->source == XRT_VIEWER_SOURCE_PROVIDER;
	for (uint32_t i = 0; i < n && !needed; i++) {
		needed = in[i].has_dp && !in[i].dp_tracks && in[i].dp_policy == XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK;
	}
	struct target_viewer_provider_in p;
	provider_inputs(needed, count_rows, &p);
	const uint32_t m = target_viewer_arbitrate_screens(in, n, &p, out);
	for (uint32_t i = 0; i < m; i++) {
		if (out[i].is_primary && primary != NULL) {
			// The primary's answer is the instance's (the advertisement was
			// composed from it): keep it, whatever the screen list says.
			out[i].d = *primary;
			out[i].offset_m[0] = out[i].offset_m[1] = out[i].offset_m[2] = 0.0f;
			if (count_rows && out[i].d.source == XRT_VIEWER_SOURCE_DP && p.enabled) {
				out[i].d.provider_available = true; // outranked: standby
			}
		}
	}
	return m;
}

void
target_viewer_tracking_update_screens(const struct xrt_screen_list *list, const struct xrt_dp_factory_registry *reg)
{
	if (list == NULL) {
		return;
	}
	struct target_viewer_decision primary;
	const bool have_primary = target_viewer_tracking_get_decision(&primary);
	struct target_viewer_screen_decision now[TARGET_VIEWER_MAX_SCREENS];
	const uint32_t n =
	    evaluate_screens(list, reg, have_primary ? &primary : NULL, false, now, TARGET_VIEWER_MAX_SCREENS);

	struct target_viewer_screen_decision prev[TARGET_VIEWER_MAX_SCREENS];
	lock();
	const bool had = g_have_screens;
	const uint32_t prev_n = g_screen_count;
	memcpy(prev, g_screens, sizeof(prev));
	memcpy(g_screens, now, sizeof(now));
	g_screen_count = n;
	g_have_screens = true;
	g_primary_screen_id = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (now[i].is_primary) {
			g_primary_screen_id = now[i].screen_id;
		}
		if (now[i].d.source == XRT_VIEWER_SOURCE_PROVIDER) {
			g_provider_index = 0;
		}
	}
	unlock();

	// One WARN per screen whose answer changed (hotplug / re-probe only — never per frame).
	for (uint32_t i = 0; i < n; i++) {
		bool same = false;
		for (uint32_t k = 0; had && k < prev_n && !same; k++) {
			same = prev[k].screen_id == now[i].screen_id && prev[k].d.source == now[i].d.source &&
			       strcmp(prev[k].d.provider_id, now[i].d.provider_id) == 0 &&
			       prev[k].offset_m[0] == now[i].offset_m[0] && prev[k].offset_m[1] == now[i].offset_m[1];
		}
		if (same) {
			continue;
		}
		char label[96];
		target_viewer_source_label(&now[i].d, label, sizeof(label));
		U_LOG_W("viewer tracking (ADR-054 N6): screen 0x%016llx%s viewer source = %s (display policy %s%s%s)",
		        (unsigned long long)now[i].screen_id, now[i].is_primary ? " (primary)" : "", label,
		        target_viewer_policy_str(now[i].d.dp_policy), now[i].d.reason[0] ? "; " : "", now[i].d.reason);
		if (now[i].d.source == XRT_VIEWER_SOURCE_PROVIDER && !now[i].is_primary) {
			U_LOG_W("viewer tracking:   offset from the primary's display space = (%.4f, %.4f, %.4f) m",
			        (double)now[i].offset_m[0], (double)now[i].offset_m[1], (double)now[i].offset_m[2]);
		}
	}
}

uint32_t
target_viewer_tracking_screen_decisions(const struct xrt_screen_list *list,
                                        const struct xrt_dp_factory_registry *reg,
                                        struct target_viewer_screen_decision *out,
                                        uint32_t max)
{
	if (list == NULL || out == NULL || max == 0) {
		return 0;
	}
	struct target_viewer_decision primary;
	bool have_primary = target_viewer_tracking_get_decision(&primary);

	// Recorded: answer from what this process arbitrated, screen by screen.
	lock();
	const bool have_screens = g_have_screens;
	struct target_viewer_screen_decision rec[TARGET_VIEWER_MAX_SCREENS];
	const uint32_t rec_n = g_screen_count;
	memcpy(rec, g_screens, sizeof(rec));
	unlock();

	struct target_viewer_screen_decision fresh[TARGET_VIEWER_MAX_SCREENS];
	if (!have_primary) {
		// Headless (a CLI): the primary as the active plug-in answers now.
		const struct xrt_plugin_iface *iface = target_plugin_get_active();
		if (iface != NULL) {
			uint32_t sup = 0;
			for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX; i++) {
				if ((list->screens[i].flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0) {
					sup = list->screens[i].info.supported_eye_tracking_modes;
				}
			}
			target_viewer_tracking_evaluate(iface, target_plugin_get_active_instance(), sup, &primary);
			have_primary = true;
		}
	}
	const uint32_t n = evaluate_screens(list, reg, have_primary ? &primary : NULL, true, fresh,
	                                    max < TARGET_VIEWER_MAX_SCREENS ? max : TARGET_VIEWER_MAX_SCREENS);
	for (uint32_t i = 0; i < n; i++) {
		out[i] = fresh[i];
		for (uint32_t k = 0; have_screens && k < rec_n; k++) {
			if (rec[k].screen_id == fresh[i].screen_id) {
				out[i] = rec[k];
				// The recorded run counted no rows (it never enumerates on
				// the arbitration path); the fresh one did, for "standby".
				if (out[i].d.source == XRT_VIEWER_SOURCE_DP && fresh[i].d.provider_available) {
					out[i].d.provider_available = true;
				}
				break;
			}
		}
	}
	return n;
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
	U_LOG_W("viewer tracking: provider '%s' started on demand (first viewer request%s).",
	        target_tracking_plugin_get_row_id(g_provider_index),
	        g_service_process ? ", service: one instance for every IPC client" : "");
}

/*!
 * The decision for @p screen_id and its offset. Caller holds the lock.
 * PRIMARY (0) and the primary's monitor id read the instance's decision; any
 * other id the per-screen table; an unknown id reads as DP (the caller keeps
 * the display processor's answer).
 */
static bool
lookup_locked(uint64_t screen_id, struct target_viewer_decision *out_d, float out_offset[3])
{
	out_offset[0] = out_offset[1] = out_offset[2] = 0.0f;
	if (screen_id == XRT_VIEWER_SCREEN_PRIMARY || (g_primary_screen_id != 0 && screen_id == g_primary_screen_id)) {
		if (!g_have_decision) {
			return false;
		}
		*out_d = g_decision;
		return true;
	}
	for (uint32_t i = 0; g_have_screens && i < g_screen_count; i++) {
		if (g_screens[i].screen_id == screen_id) {
			*out_d = g_screens[i].d;
			out_offset[0] = g_screens[i].offset_m[0];
			out_offset[1] = g_screens[i].offset_m[1];
			out_offset[2] = g_screens[i].offset_m[2];
			return true;
		}
	}
	return false;
}

static enum xrt_viewer_source
vt_get_source(struct xrt_viewer_tracking *vt, uint64_t screen_id)
{
	(void)vt;
	struct target_viewer_decision d;
	float off[3];
	lock();
	const bool have = lookup_locked(screen_id, &d, off);
	unlock();
	return have ? d.source : XRT_VIEWER_SOURCE_DP;
}

static bool
vt_get_eyes_at(struct xrt_viewer_tracking *vt, uint64_t screen_id, int64_t target_ns, struct xrt_eye_positions *out)
{
	(void)vt;
	if (out == NULL) {
		return false;
	}
	struct target_viewer_decision d;
	float off[3];
	lock();
	if (!lookup_locked(screen_id, &d, off) || d.source != XRT_VIEWER_SOURCE_PROVIDER) {
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
	// The samples are in the primary's display space; this screen's is offset.
	for (uint32_t i = 0; i < e.count && i < XRT_MAX_VIEWS; i++) {
		e.eyes[i].x += off[0];
		e.eyes[i].y += off[1];
		e.eyes[i].z += off[2];
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

enum target_viewer_provider_state
target_viewer_tracking_provider_state(const struct target_viewer_decision *d)
{
	uint32_t st = 0;
	const bool started = target_viewer_tracking_provider_running(&st);
	bool producing = false;
	if (started && st != XRT_TRACKING_PROVIDER_STATUS_NO_SOURCE && st != XRT_TRACKING_PROVIDER_STATUS_STOPPED) {
		// Fresh samples: the ring answers "now" without going stale.
		struct xrt_eye_positions e;
		memset(&e, 0, sizeof(e));
		const enum u_eye_history_result r = g_ring_ready
		                                        ? u_eye_history_get(&g_ring, (int64_t)os_monotonic_get_ns(), &e)
		                                        : U_EYE_HISTORY_RESULT_EMPTY;
		producing = r != U_EYE_HISTORY_RESULT_EMPTY && r != U_EYE_HISTORY_RESULT_STALE;
	}
	return target_viewer_provider_state_of(d, started, producing);
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
	g_have_screens = false;
	g_screen_count = 0;
	g_primary_screen_id = 0;
	g_provider_index = -1;
	unlock();
}
