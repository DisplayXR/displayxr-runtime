// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Warning derivation + JSON / text serialisation of the display status
 *         snapshot (ADR-051, design `docs/roadmap/display-dashboard.md` §3/§4/§7).
 * @ingroup aux_util
 */

#include "util/u_status_snapshot.h"

#include "xrt/xrt_instance.h" // enum xrt_client_class
#include "xrt/xrt_plugin.h"   // enum xrt_display_claim_confidence, xrt_plugin_platform_state

#include <cjson/cJSON.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>


/*
 *
 * Enum spellings.
 *
 */

const char *
u_status_source_str(enum xrt_status_source s)
{
	return s == XRT_STATUS_SOURCE_SERVICE ? "service" : "headless";
}

const char *
u_status_tracking_str(enum xrt_status_tracking t)
{
	switch (t) {
	case XRT_STATUS_TRACKING_TRACKING: return "TRACKING";
	case XRT_STATUS_TRACKING_NOT_TRACKING: return "NOT_TRACKING";
	case XRT_STATUS_TRACKING_NO_DP: return "NO_DP";
	default: return "UNKNOWN";
	}
}

const char *
u_status_presenter_str(enum xrt_status_presenter p)
{
	switch (p) {
	case XRT_STATUS_PRESENTER_APP_HWND: return "APP_HWND";
	case XRT_STATUS_PRESENTER_CLIENT_TEXTURE: return "CLIENT_TEXTURE";
	case XRT_STATUS_PRESENTER_SERVICE_WINDOW: return "SERVICE_WINDOW";
	case XRT_STATUS_PRESENTER_SELF: return "SELF";
	default: return "NONE";
	}
}

const char *
u_status_lease_str(enum xrt_status_lease l)
{
	switch (l) {
	case XRT_STATUS_LEASE_CONTROLLER: return "controller";
	case XRT_STATUS_LEASE_SLOT: return "slot";
	default: return "none";
	}
}

const char *
u_status_level_str(enum xrt_status_level l)
{
	switch (l) {
	case XRT_STATUS_LEVEL_WARN: return "warn";
	case XRT_STATUS_LEVEL_CRITICAL: return "critical";
	default: return "info";
	}
}

const char *
u_status_vendor_tracker_str(enum xrt_status_vendor_tracker t)
{
	switch (t) {
	case XRT_STATUS_VENDOR_TRACKER_NONE: return "NONE";
	case XRT_STATUS_VENDOR_TRACKER_OFF: return "OFF";
	case XRT_STATUS_VENDOR_TRACKER_STARTING: return "STARTING";
	case XRT_STATUS_VENDOR_TRACKER_RUNNING: return "RUNNING";
	case XRT_STATUS_VENDOR_TRACKER_DOWN: return "DOWN";
	case XRT_STATUS_VENDOR_TRACKER_UNSUPPORTED: return "UNSUPPORTED";
	default: return "UNKNOWN";
	}
}

const char *
u_status_vendor_lens_str(enum xrt_status_vendor_lens l)
{
	switch (l) {
	case XRT_STATUS_VENDOR_LENS_2D: return "2D";
	case XRT_STATUS_VENDOR_LENS_3D: return "3D";
	default: return "UNKNOWN";
	}
}

const char *
u_status_mm_source_str(enum xrt_status_mm_source s)
{
	switch (s) {
	case XRT_STATUS_MM_SOURCE_EDID: return "edid";
	case XRT_STATUS_MM_SOURCE_PLUGIN: return "plugin";
	default: return "none";
	}
}

const char *
u_status_layout_source_str(uint32_t src)
{
	// SYSTEM is the active plug-in's own get_display_info as applied to the
	// system, so to a reader it is "the plug-in said so", like PLUGIN_MONITOR.
	switch (src) {
	case XRT_SCREEN_INFO_SOURCE_SYSTEM:
	case XRT_SCREEN_INFO_SOURCE_PLUGIN_MONITOR: return "plugin";
	case XRT_SCREEN_INFO_SOURCE_DERIVED: return "derived";
	default: return "none";
	}
}

const char *
u_status_dp_api_str(enum xrt_status_dp_api a)
{
	switch (a) {
	case XRT_STATUS_DP_API_D3D11: return "d3d11";
	case XRT_STATUS_DP_API_D3D12: return "d3d12";
	case XRT_STATUS_DP_API_VK: return "vk";
	case XRT_STATUS_DP_API_GL: return "gl";
	case XRT_STATUS_DP_API_METAL: return "metal";
	default: return "?";
	}
}

const char *
u_status_dp_kind_str(enum xrt_status_dp_kind k)
{
	return k == XRT_STATUS_DP_KIND_SEGMENT ? "segment" : "primary";
}

const char *
u_status_dp_backend_str(enum xrt_status_dp_backend b)
{
	switch (b) {
	case XRT_STATUS_DP_BACKEND_DEGRADED: return "DEGRADED";
	case XRT_STATUS_DP_BACKEND_STALE: return "STALE";
	default: return "OK";
	}
}

const char *
u_status_eye_source_str(enum xrt_status_eye_source e)
{
	switch (e) {
	case XRT_STATUS_EYE_SOURCE_DP: return "DP";
	case XRT_STATUS_EYE_SOURCE_PRIMARY: return "PRIMARY";
	default: return "NONE";
	}
}

const char *
u_status_confidence_str(uint32_t c)
{
	switch (c) {
	case 0: return "NONE";
	case (uint32_t)XRT_DISPLAY_CLAIM_FALLBACK: return "FALLBACK";
	case (uint32_t)XRT_DISPLAY_CLAIM_EDID: return "EDID";
	case (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED: return "VERIFIED";
	default: return "UNKNOWN";
	}
}

const char *
u_status_pref_source_str(enum xrt_status_pref_source s)
{
	switch (s) {
	case XRT_STATUS_PREF_SOURCE_ENV: return "env";
	case XRT_STATUS_PREF_SOURCE_USER: return "user";
	case XRT_STATUS_PREF_SOURCE_MACHINE: return "machine";
	case XRT_STATUS_PREF_SOURCE_NONE:
	default: return NULL;
	}
}

const char *
u_status_apply_str(enum xrt_status_apply a)
{
	return a == XRT_STATUS_APPLY_NEXT_SESSION ? "next-session" : "live";
}

const char *
u_status_platform_state_str(uint32_t state)
{
	switch (state) {
	case XRT_PLUGIN_PLATFORM_STATE_READY: return "READY";
	case XRT_PLUGIN_PLATFORM_STATE_PLATFORM_ABSENT: return "PLATFORM_ABSENT";
	case XRT_PLUGIN_PLATFORM_STATE_PLATFORM_NOT_RUNNING: return "PLATFORM_NOT_RUNNING";
	case XRT_PLUGIN_PLATFORM_STATE_NO_DISPLAY: return "NO_DISPLAY";
	case XRT_PLUGIN_PLATFORM_STATE_INCOMPATIBLE: return "INCOMPATIBLE";
	default: return "UNKNOWN";
	}
}

const char *
u_status_client_class_str(uint32_t client_class, bool verified)
{
	if (!verified) {
		return "UNVERIFIED";
	}
	switch (client_class) {
	case XRT_CLIENT_CLASS_APP: return "APP";
	case XRT_CLIENT_CLASS_CONTROLLER: return "CONTROLLER";
	case XRT_CLIENT_CLASS_PRESENT_OWNER: return "PRESENT_OWNER";
	case XRT_CLIENT_CLASS_RELAY: return "RELAY";
	case XRT_CLIENT_CLASS_PROVIDER_HOST: return "PROVIDER_HOST";
	case XRT_CLIENT_CLASS_DIAG: return "DIAG";
	case XRT_CLIENT_CLASS_CAMERA_CONSUMER: return "CAMERA_CONSUMER";
	default: return "?";
	}
}


/*
 *
 * Warnings (§4).
 *
 */

bool
u_status_warning_push(struct xrt_status_warning *arr,
                      uint32_t *count,
                      uint32_t cap,
                      const char *code,
                      enum xrt_status_level level,
                      const char *text)
{
	if (*count >= cap) {
		return false;
	}
	struct xrt_status_warning *w = &arr[(*count)++];
	memset(w, 0, sizeof(*w));
	(void)snprintf(w->code, sizeof(w->code), "%s", code);
	w->level = level;
	(void)snprintf(w->text, sizeof(w->text), "%s", text != NULL ? text : "");
	return true;
}

static void
screen_warn(struct xrt_status_screen *s, const char *code, enum xrt_status_level level, const char *text)
{
	(void)u_status_warning_push(s->warnings, &s->warning_count, XRT_STATUS_MAX_WARNINGS, code, level, text);
}

static const struct xrt_status_plugin *
find_plugin(const struct xrt_status_snapshot *snap, const char *id)
{
	if (id == NULL || id[0] == '\0') {
		return NULL;
	}
	for (uint32_t i = 0; i < snap->plugin_count && i < XRT_STATUS_MAX_PLUGINS; i++) {
		if (strcmp(snap->plugins[i].id, id) == 0) {
			return &snap->plugins[i];
		}
	}
	return NULL;
}

//! A load outcome that is a failure (the loader's spellings), not a choice.
static bool
load_is_failure(const char *load)
{
	static const char *const ok[] = {"", "ACTIVE", "LOADED", "DECLINED", "NOT_ATTEMPTED"};
	for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
		if (strcmp(load, ok[i]) == 0) {
			return false;
		}
	}
	return true;
}

//! A platform state the plug-in reported and that is not READY (UNKNOWN = not reported, never a warning).
static bool
platform_not_ready(uint32_t state)
{
	return state != XRT_PLUGIN_PLATFORM_STATE_UNKNOWN && state != XRT_PLUGIN_PLATFORM_STATE_READY;
}

/*!
 * The runtime cannot know whether a monitor is 3D-capable (phase 3's vendor
 * cell will). The proxy for "a 3D-capable monitor got only the fallback": a
 * registered VENDOR plug-in is unhealthy (failed to load, or reports a
 * platform state other than READY), so the fallback claim is the vendor's
 * absence and not the normal claim of a plain 2D monitor.
 */
static bool
vendor_plugin_unhealthy(const struct xrt_status_snapshot *snap)
{
	for (uint32_t i = 0; i < snap->plugin_count && i < XRT_STATUS_MAX_PLUGINS; i++) {
		const struct xrt_status_plugin *p = &snap->plugins[i];
		if (p->fallback) {
			continue;
		}
		if (load_is_failure(p->load) || platform_not_ready(p->platform_state)) {
			return true;
		}
	}
	return false;
}

static void
plugin_not_ready_text(const struct xrt_status_plugin *p, char *out, size_t cap)
{
	if (p->hint[0] != '\0') {
		(void)snprintf(out, cap, "%s", p->hint); // the plug-in's hint, verbatim
	} else {
		(void)snprintf(out, cap, "Plug-in '%s' reports %s: start or repair its platform.", p->id,
		               u_status_platform_state_str(p->platform_state));
	}
}

void
u_status_warnings_derive(struct xrt_status_snapshot *snap)
{
	if (snap == NULL) {
		return;
	}
	snap->warning_count = 0;
	memset(snap->warnings, 0, sizeof(snap->warnings));

	const bool vendor_unhealthy = vendor_plugin_unhealthy(snap);
	const uint32_t nscreens =
	    snap->screen_count < XRT_STATUS_MAX_SCREENS ? snap->screen_count : XRT_STATUS_MAX_SCREENS;
	const uint32_t nclients =
	    snap->client_count < XRT_STATUS_MAX_CLIENTS ? snap->client_count : XRT_STATUS_MAX_CLIENTS;

	for (uint32_t i = 0; i < nscreens; i++) {
		struct xrt_status_screen *s = &snap->screens[i];
		s->warning_count = 0;
		memset(s->warnings, 0, sizeof(s->warnings));
		char text[XRT_STATUS_TEXT_MAX];
		const bool claimed = s->claim.plugin_id[0] != '\0';

		// NOT_NATIVE — only when the native mode is KNOWN; unknown never warns.
		if (claimed && s->native.width > 0 && s->native.height > 0 && s->desktop.width > 0 &&
		    s->desktop.height > 0 &&
		    (s->desktop.width != s->native.width || s->desktop.height != s->native.height)) {
			(void)snprintf(
			    text, sizeof(text),
			    "Set this screen to its native %ux%u (now %ux%u): a 3D panel weaves correctly only "
			    "at native resolution.",
			    s->native.width, s->native.height, s->desktop.width, s->desktop.height);
			screen_warn(s, U_STATUS_W_NOT_NATIVE, XRT_STATUS_LEVEL_CRITICAL, text);
		}

		// PLUGIN_NOT_READY — the claiming plug-in reported a non-READY platform state.
		const struct xrt_status_plugin *owner = find_plugin(snap, s->claim.plugin_id);
		if (owner != NULL && platform_not_ready(owner->platform_state)) {
			plugin_not_ready_text(owner, text, sizeof(text));
			screen_warn(s, U_STATUS_W_PLUGIN_NOT_READY, XRT_STATUS_LEVEL_CRITICAL, text);
		}

		// CLAIM_FALLBACK / CLAIM_EDID_ONLY.
		if (s->claim.confidence == (uint32_t)XRT_DISPLAY_CLAIM_FALLBACK && vendor_unhealthy) {
			(void)snprintf(text, sizeof(text),
			               "Only the fallback '%s' claims this screen: check that the vendor plug-in is "
			               "installed and its platform is running.",
			               s->claim.plugin_id);
			screen_warn(s, U_STATUS_W_CLAIM_FALLBACK, XRT_STATUS_LEVEL_WARN, text);
		} else if (s->claim.confidence == (uint32_t)XRT_DISPLAY_CLAIM_EDID) {
			screen_warn(s, U_STATUS_W_CLAIM_EDID_ONLY, XRT_STATUS_LEVEL_INFO,
			            "Claimed by EDID identity, not verified by the vendor: check that the vendor "
			            "platform sees this display.");
		}

		// CLAIM_FORCED — a per-screen preference picked this screen's plug-in
		// (display dashboard phase 7): not a fault, but the dashboard shows it.
		if (claimed && s->claim.forced) {
			const char *src = u_status_pref_source_str(s->claim.preferred_source);
			(void)snprintf(text, sizeof(text), "Plug-in forced by a per-screen preference (%s)",
			               src != NULL ? src : "unknown");
			screen_warn(s, U_STATUS_W_CLAIM_FORCED, XRT_STATUS_LEVEL_INFO, text);
		}

		// NO_PHYSICAL_SIZE — the 0 m trap, for a screen a DP can be made for.
		if (claimed && s->physical.source == XRT_STATUS_MM_SOURCE_NONE &&
		    !(s->layout.width_m > 0.0f && s->layout.height_m > 0.0f)) {
			screen_warn(
			    s, U_STATUS_W_NO_PHYSICAL_SIZE, XRT_STATUS_LEVEL_WARN,
			    "No physical size from the plug-in or EDID: a window on this screen gets one view set "
			    "for the whole window.");
		}

		// SEGMENT_FLAT_2D — a live window has a segment on this screen with no DP.
		bool flat = false;
		for (uint32_t c = 0; c < nclients && !flat; c++) {
			const struct xrt_status_segments *sg = &snap->clients[c].segments;
			for (uint32_t k = 0; k < sg->count && k < XRT_STATUS_MAX_SEGMENTS; k++) {
				if (sg->items[k].screen == s->id && !sg->items[k].has_dp) {
					flat = true;
					break;
				}
			}
		}
		if (flat) {
			screen_warn(s, U_STATUS_W_SEGMENT_FLAT_2D, XRT_STATUS_LEVEL_WARN,
			            "A window spans this screen but its segment has no display processor; that half is "
			            "flat 2D.");
		}

		// TRACKER_DOWN — needs timing the headless snapshot does not have: it
		// fires only when the input carries `not_tracking_ms` (phase 2, the
		// service's DPs). Headless screens are NO_DP and never match.
		bool other_tracking = false;
		for (uint32_t j = 0; j < nscreens; j++) {
			if (j != i && snap->screens[j].eye_tracking.state == XRT_STATUS_TRACKING_TRACKING) {
				other_tracking = true;
			}
		}
		if (s->eye_tracking.supported != 0 && s->dp_count > 0 &&
		    s->eye_tracking.state == XRT_STATUS_TRACKING_NOT_TRACKING &&
		    s->eye_tracking.not_tracking_ms > U_STATUS_TRACKER_DOWN_MS && other_tracking) {
			(void)snprintf(
			    text, sizeof(text),
			    "No viewer tracked on this screen for %u s while another screen tracks: check this "
			    "screen's camera.",
			    s->eye_tracking.not_tracking_ms / 1000u);
			screen_warn(s, U_STATUS_W_TRACKER_DOWN, XRT_STATUS_LEVEL_WARN, text);
		}

		// DP_DEGRADED / DP_STALE — one per affected DP.
		for (uint32_t d = 0; d < s->dp_count && d < XRT_STATUS_MAX_SCREEN_DPS; d++) {
			const struct xrt_status_dp *dp = &s->dps[d];
			if (dp->backend == XRT_STATUS_DP_BACKEND_DEGRADED) {
				(void)snprintf(
				    text, sizeof(text),
				    "The display processor of client %u is reconnecting to its vendor backend; "
				    "wait, or restart the vendor service if it persists.",
				    dp->client_id);
				screen_warn(s, U_STATUS_W_DP_DEGRADED, XRT_STATUS_LEVEL_WARN, text);
			} else if (dp->backend == XRT_STATUS_DP_BACKEND_STALE) {
				(void)snprintf(
				    text, sizeof(text),
				    "The display processor of client %u lost its vendor backend and is being "
				    "recreated: restart the vendor service if this persists.",
				    dp->client_id);
				screen_warn(s, U_STATUS_W_DP_STALE, XRT_STATUS_LEVEL_CRITICAL, text);
			}
		}
	}

	// System level.
	if (snap->source == XRT_STATUS_SOURCE_HEADLESS) {
		(void)u_status_warning_push(snap->warnings, &snap->warning_count, XRT_STATUS_MAX_WARNINGS,
		                            U_STATUS_W_SERVICE_HEADLESS, XRT_STATUS_LEVEL_INFO,
		                            "No service reached: this is what a process starting now would get; live "
		                            "rows are absent.");
	}
	// The active plug-in not READY and claiming no screen has nowhere else to show.
	for (uint32_t i = 0; i < snap->plugin_count && i < XRT_STATUS_MAX_PLUGINS; i++) {
		const struct xrt_status_plugin *p = &snap->plugins[i];
		if (!p->active || !platform_not_ready(p->platform_state)) {
			continue;
		}
		bool claims = false;
		for (uint32_t k = 0; k < nscreens; k++) {
			claims |= strcmp(snap->screens[k].claim.plugin_id, p->id) == 0;
		}
		if (!claims) {
			char text[XRT_STATUS_TEXT_MAX];
			plugin_not_ready_text(p, text, sizeof(text));
			(void)u_status_warning_push(snap->warnings, &snap->warning_count, XRT_STATUS_MAX_WARNINGS,
			                            U_STATUS_W_PLUGIN_NOT_READY, XRT_STATUS_LEVEL_CRITICAL, text);
		}
	}
}


/*
 *
 * Wire pieces (phase 2).
 *
 */

void
u_status_snapshot_get_head(const struct xrt_status_snapshot *snap, struct xrt_status_head *out)
{
	memset(out, 0, sizeof(*out));
	if (snap == NULL) {
		return;
	}
	out->schema = snap->schema;
	out->source = snap->source;
	out->generation = snap->generation;
	out->runtime = snap->runtime;
	out->plugin_count = snap->plugin_count < XRT_STATUS_MAX_PLUGINS ? snap->plugin_count : XRT_STATUS_MAX_PLUGINS;
	memcpy(out->plugins, snap->plugins, sizeof(out->plugins));
	out->screen_count = snap->screen_count < XRT_STATUS_MAX_SCREENS ? snap->screen_count : XRT_STATUS_MAX_SCREENS;
	out->client_count = snap->client_count < XRT_STATUS_MAX_CLIENTS ? snap->client_count : XRT_STATUS_MAX_CLIENTS;
	for (uint32_t i = 0; i < out->client_count; i++) {
		out->client_ids[i] = snap->clients[i].id;
	}
	out->workspace = snap->workspace;
	out->warning_count =
	    snap->warning_count < XRT_STATUS_MAX_WARNINGS ? snap->warning_count : XRT_STATUS_MAX_WARNINGS;
	memcpy(out->warnings, snap->warnings, sizeof(out->warnings));
}

void
u_status_snapshot_set_head(struct xrt_status_snapshot *snap, const struct xrt_status_head *head)
{
	memset(snap, 0, sizeof(*snap));
	if (head == NULL) {
		return;
	}
	snap->schema = head->schema;
	snap->source = head->source;
	snap->generation = head->generation;
	snap->runtime = head->runtime;
	snap->plugin_count = head->plugin_count < XRT_STATUS_MAX_PLUGINS ? head->plugin_count : XRT_STATUS_MAX_PLUGINS;
	memcpy(snap->plugins, head->plugins, sizeof(snap->plugins));
	snap->screen_count = head->screen_count < XRT_STATUS_MAX_SCREENS ? head->screen_count : XRT_STATUS_MAX_SCREENS;
	snap->client_count = head->client_count < XRT_STATUS_MAX_CLIENTS ? head->client_count : XRT_STATUS_MAX_CLIENTS;
	for (uint32_t i = 0; i < snap->client_count; i++) {
		snap->clients[i].id = head->client_ids[i];
	}
	snap->workspace = head->workspace;
	snap->warning_count =
	    head->warning_count < XRT_STATUS_MAX_WARNINGS ? head->warning_count : XRT_STATUS_MAX_WARNINGS;
	memcpy(snap->warnings, head->warnings, sizeof(snap->warnings));
}

bool
u_status_generation_equal(const struct xrt_status_generation *a, const struct xrt_status_generation *b)
{
	return a->topology == b->topology && a->status == b->status;
}


/*
 *
 * JSON (§3).
 *
 */

static void
add_hex_id(cJSON *o, const char *key, uint64_t id)
{
	if (id == 0) {
		cJSON_AddNullToObject(o, key);
		return;
	}
	char buf[24];
	(void)snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)id);
	cJSON_AddStringToObject(o, key, buf);
}

static void
add_str_or_null(cJSON *o, const char *key, const char *s)
{
	if (s == NULL || s[0] == '\0') {
		cJSON_AddNullToObject(o, key);
	} else {
		cJSON_AddStringToObject(o, key, s);
	}
}

static cJSON *
warning_to_cjson(const struct xrt_status_warning *w)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "code", w->code);
	cJSON_AddStringToObject(o, "level", u_status_level_str(w->level));
	cJSON_AddStringToObject(o, "text", w->text);
	return o;
}

static void
add_warnings(cJSON *o, const struct xrt_status_warning *arr, uint32_t count)
{
	cJSON *a = cJSON_AddArrayToObject(o, "warnings");
	for (uint32_t i = 0; i < count && i < XRT_STATUS_MAX_WARNINGS; i++) {
		cJSON_AddItemToArray(a, warning_to_cjson(&arr[i]));
	}
}

static void
add_eye_modes(cJSON *o, const char *key, uint32_t bits)
{
	cJSON *a = cJSON_AddArrayToObject(o, key);
	if ((bits & 0x1u) != 0) {
		cJSON_AddItemToArray(a, cJSON_CreateString("MANAGED"));
	}
	if ((bits & 0x2u) != 0) {
		cJSON_AddItemToArray(a, cJSON_CreateString("MANUAL"));
	}
}

static void
add_apis(cJSON *o, uint32_t bits)
{
	// Order of design §3: d3d11, d3d12, vk, gl (then metal).
	static const struct
	{
		uint32_t bit;
		const char *name;
	} apis[] = {
	    {XRT_STATUS_API_BIT_D3D11, "d3d11"}, {XRT_STATUS_API_BIT_D3D12, "d3d12"}, {XRT_STATUS_API_BIT_VK, "vk"},
	    {XRT_STATUS_API_BIT_GL, "gl"},       {XRT_STATUS_API_BIT_METAL, "metal"},
	};
	cJSON *a = cJSON_AddArrayToObject(o, "apis");
	for (size_t i = 0; i < sizeof(apis) / sizeof(apis[0]); i++) {
		if ((bits & apis[i].bit) != 0) {
			cJSON_AddItemToArray(a, cJSON_CreateString(apis[i].name));
		}
	}
}

static cJSON *
screen_to_cjson(const struct xrt_status_screen *s)
{
	cJSON *o = cJSON_CreateObject();
	add_hex_id(o, "id", s->id);
	cJSON_AddNumberToObject(o, "index", (double)s->index);
	cJSON_AddStringToObject(o, "device_name", s->device_name);
	cJSON_AddStringToObject(o, "friendly_name", s->friendly_name);
	add_str_or_null(o, "key", s->key);

	cJSON *edid = cJSON_AddObjectToObject(o, "edid");
	cJSON_AddStringToObject(edid, "manufacturer", s->edid.manufacturer);
	cJSON_AddStringToObject(edid, "product", s->edid.product);
	cJSON_AddNumberToObject(edid, "serial", (double)s->edid.serial);

	cJSON *desk = cJSON_AddObjectToObject(o, "desktop");
	cJSON_AddNumberToObject(desk, "left", (double)s->desktop.left);
	cJSON_AddNumberToObject(desk, "top", (double)s->desktop.top);
	cJSON_AddNumberToObject(desk, "width", (double)s->desktop.width);
	cJSON_AddNumberToObject(desk, "height", (double)s->desktop.height);
	cJSON_AddNumberToObject(desk, "scale", (double)s->desktop.scale);

	cJSON *nat = cJSON_AddObjectToObject(o, "native");
	cJSON_AddNumberToObject(nat, "width", (double)s->native.width);
	cJSON_AddNumberToObject(nat, "height", (double)s->native.height);
	cJSON_AddNumberToObject(nat, "refresh_mhz", (double)s->native.refresh_mhz);
	cJSON_AddBoolToObject(nat, "is_native", s->native.is_native);

	cJSON *mm = cJSON_AddObjectToObject(o, "physical_mm");
	cJSON_AddNumberToObject(mm, "width", (double)s->physical.width_mm);
	cJSON_AddNumberToObject(mm, "height", (double)s->physical.height_mm);
	cJSON_AddStringToObject(mm, "source", u_status_mm_source_str(s->physical.source));

	cJSON *roles = cJSON_AddObjectToObject(o, "roles");
	cJSON_AddBoolToObject(roles, "os_main", s->roles.os_main);
	cJSON_AddBoolToObject(roles, "runtime_default", s->roles.runtime_default);
	cJSON_AddBoolToObject(roles, "vendor_primary", s->roles.vendor_primary);

	cJSON *claim = cJSON_AddObjectToObject(o, "claim");
	add_str_or_null(claim, "plugin_id", s->claim.plugin_id);
	cJSON_AddStringToObject(claim, "confidence", u_status_confidence_str(s->claim.confidence));
	cJSON_AddNumberToObject(claim, "confidence_value", (double)s->claim.confidence);
	cJSON_AddStringToObject(claim, "serial", s->claim.serial);
	add_apis(claim, s->claim.apis);
	cJSON_AddBoolToObject(claim, "forced", s->claim.forced);
	add_str_or_null(claim, "preferred_plugin", s->claim.preferred_plugin);
	add_str_or_null(claim, "preferred_source", u_status_pref_source_str(s->claim.preferred_source));
	cJSON_AddStringToObject(claim, "apply", u_status_apply_str(s->claim.apply));

	cJSON *lay = cJSON_AddObjectToObject(o, "layout");
	cJSON_AddNumberToObject(lay, "width_m", (double)s->layout.width_m);
	cJSON_AddNumberToObject(lay, "height_m", (double)s->layout.height_m);
	cJSON *viewer = cJSON_AddObjectToObject(lay, "nominal_viewer_m");
	cJSON_AddNumberToObject(viewer, "x", (double)s->layout.nominal_viewer_x_m);
	cJSON_AddNumberToObject(viewer, "y", (double)s->layout.nominal_viewer_y_m);
	cJSON_AddNumberToObject(viewer, "z", (double)s->layout.nominal_viewer_z_m);
	cJSON_AddStringToObject(lay, "source", u_status_layout_source_str(s->layout.source));

	cJSON *et = cJSON_AddObjectToObject(o, "eye_tracking");
	add_eye_modes(et, "supported", s->eye_tracking.supported);
	if (s->eye_tracking.supported != 0) {
		cJSON_AddStringToObject(et, "default", s->eye_tracking.default_mode == 1 ? "MANUAL" : "MANAGED");
	} else {
		cJSON_AddNullToObject(et, "default");
	}
	cJSON_AddStringToObject(et, "state", u_status_tracking_str(s->eye_tracking.state));

	if (s->mode.valid) {
		cJSON *mode = cJSON_AddObjectToObject(o, "mode");
		cJSON_AddNumberToObject(mode, "index", (double)s->mode.index);
		cJSON_AddStringToObject(mode, "name", s->mode.name);
		cJSON_AddNumberToObject(mode, "views", (double)s->mode.views);
		cJSON_AddBoolToObject(mode, "is_3d", s->mode.is_3d);
	} else {
		cJSON_AddNullToObject(o, "mode");
	}

	cJSON *dps = cJSON_AddArrayToObject(o, "dps");
	for (uint32_t i = 0; i < s->dp_count && i < XRT_STATUS_MAX_SCREEN_DPS; i++) {
		const struct xrt_status_dp *dp = &s->dps[i];
		cJSON *d = cJSON_CreateObject();
		cJSON_AddNumberToObject(d, "client_id", (double)dp->client_id);
		cJSON_AddStringToObject(d, "api", u_status_dp_api_str(dp->api));
		cJSON_AddStringToObject(d, "kind", u_status_dp_kind_str(dp->kind));
		cJSON_AddStringToObject(d, "backend", u_status_dp_backend_str(dp->backend));
		cJSON_AddItemToArray(dps, d);
	}

	const struct xrt_status_vendor *v = &s->vendor;
	cJSON *vo = cJSON_AddObjectToObject(o, "vendor");
	cJSON_AddBoolToObject(vo, "present", v->present);
	cJSON_AddBoolToObject(vo, "ready", v->ready);
	cJSON_AddBoolToObject(vo, "verified", v->verified);
	cJSON_AddBoolToObject(vo, "calibrated", v->calibrated);
	cJSON_AddStringToObject(vo, "tracker", u_status_vendor_tracker_str(v->tracker));
	cJSON_AddStringToObject(vo, "lens", u_status_vendor_lens_str(v->lens));
	cJSON_AddStringToObject(vo, "model", v->model);
	cJSON_AddStringToObject(vo, "serial", v->serial);
	if (v->worst_warning.code[0] != '\0') {
		cJSON_AddItemToObject(vo, "worst_warning", warning_to_cjson(&v->worst_warning));
	} else {
		cJSON_AddNullToObject(vo, "worst_warning");
	}
	add_str_or_null(vo, "dashboard_command", v->dashboard_command);

	add_warnings(o, s->warnings, s->warning_count);
	return o;
}

static cJSON *
client_to_cjson(const struct xrt_status_client *c)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "id", (double)c->id);
	cJSON_AddNumberToObject(o, "pid", (double)c->pid);
	cJSON_AddStringToObject(o, "class", u_status_client_class_str(c->client_class, c->class_verified));
	cJSON_AddStringToObject(o, "name", c->name);

	cJSON *fl = cJSON_AddObjectToObject(o, "flags");
	cJSON_AddBoolToObject(fl, "active", c->flags.active);
	cJSON_AddBoolToObject(fl, "visible", c->flags.visible);
	cJSON_AddBoolToObject(fl, "focused", c->flags.focused);
	cJSON_AddBoolToObject(fl, "overlay", c->flags.overlay);

	cJSON_AddStringToObject(o, "presenter", u_status_presenter_str(c->presenter));
	cJSON_AddStringToObject(o, "lease", u_status_lease_str(c->lease));

	if (c->window.valid) {
		cJSON *w = cJSON_AddObjectToObject(o, "window");
		cJSON_AddNumberToObject(w, "left", (double)c->window.left);
		cJSON_AddNumberToObject(w, "top", (double)c->window.top);
		cJSON_AddNumberToObject(w, "width", (double)c->window.width);
		cJSON_AddNumberToObject(w, "height", (double)c->window.height);
	} else {
		cJSON_AddNullToObject(o, "window");
	}
	add_hex_id(o, "owner_screen", c->owner_screen);

	cJSON *sg = cJSON_AddObjectToObject(o, "segments");
	cJSON_AddNumberToObject(sg, "generation", (double)c->segments.generation);
	cJSON_AddBoolToObject(sg, "split", c->segments.split);
	cJSON *items = cJSON_AddArrayToObject(sg, "items");
	for (uint32_t i = 0; i < c->segments.count && i < XRT_STATUS_MAX_SEGMENTS; i++) {
		const struct xrt_status_segment *it = &c->segments.items[i];
		cJSON *io = cJSON_CreateObject();
		add_hex_id(io, "screen", it->screen);
		cJSON *cv = cJSON_AddObjectToObject(io, "canvas");
		cJSON_AddNumberToObject(cv, "x", (double)it->canvas.x);
		cJSON_AddNumberToObject(cv, "y", (double)it->canvas.y);
		cJSON_AddNumberToObject(cv, "w", (double)it->canvas.w);
		cJSON_AddNumberToObject(cv, "h", (double)it->canvas.h);
		cJSON_AddBoolToObject(io, "has_dp", it->has_dp);
		cJSON_AddBoolToObject(io, "woven", it->woven);
		cJSON_AddStringToObject(io, "eye_source", u_status_eye_source_str(it->eye_source));
		cJSON_AddItemToArray(items, io);
	}

	cJSON *vw = cJSON_AddObjectToObject(o, "views");
	cJSON_AddNumberToObject(vw, "capacity", (double)c->views.capacity);
	cJSON_AddNumberToObject(vw, "active", (double)c->views.active);
	cJSON_AddNumberToObject(vw, "reported", (double)c->views.reported);

	cJSON *in = cJSON_AddObjectToObject(o, "integrity");
	cJSON_AddNumberToObject(in, "paint", (double)c->integrity.paint);
	cJSON_AddNumberToObject(in, "present", (double)c->integrity.present);
	cJSON_AddNumberToObject(in, "skip", (double)c->integrity.skip);
	add_str_or_null(in, "weave_placement", c->integrity.weave_placement);
	return o;
}

cJSON *
u_status_snapshot_to_cjson(const struct xrt_status_snapshot *snap)
{
	cJSON *root = cJSON_CreateObject();
	if (snap == NULL) {
		return root;
	}
	cJSON_AddNumberToObject(root, "schema", (double)snap->schema); // always first
	cJSON_AddStringToObject(root, "source", u_status_source_str(snap->source));

	cJSON *gen = cJSON_AddObjectToObject(root, "generation");
	cJSON_AddNumberToObject(gen, "topology", (double)snap->generation.topology);
	cJSON_AddNumberToObject(gen, "status", (double)snap->generation.status);

	cJSON *rt = cJSON_AddObjectToObject(root, "runtime");
	cJSON_AddStringToObject(rt, "version", snap->runtime.version);
	cJSON_AddStringToObject(rt, "git_tag", snap->runtime.git_tag);
	cJSON_AddNumberToObject(rt, "plugin_abi", (double)snap->runtime.plugin_abi);
	add_str_or_null(rt, "active_openxr_runtime", snap->runtime.active_openxr_runtime);

	cJSON *pl = cJSON_AddArrayToObject(root, "plugins");
	for (uint32_t i = 0; i < snap->plugin_count && i < XRT_STATUS_MAX_PLUGINS; i++) {
		const struct xrt_status_plugin *p = &snap->plugins[i];
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "id", p->id);
		cJSON_AddStringToObject(o, "name", p->name);
		cJSON_AddStringToObject(o, "vendor", p->vendor);
		cJSON_AddStringToObject(o, "version", p->version);
		cJSON_AddStringToObject(o, "load", p->load);
		cJSON_AddStringToObject(o, "platform_state", u_status_platform_state_str(p->platform_state));
		cJSON_AddStringToObject(o, "hint", p->hint);
		cJSON_AddBoolToObject(o, "fallback", p->fallback);
		cJSON_AddNumberToObject(o, "probe_order", (double)p->probe_order);
		cJSON_AddItemToArray(pl, o);
	}

	cJSON *sc = cJSON_AddArrayToObject(root, "screens");
	for (uint32_t i = 0; i < snap->screen_count && i < XRT_STATUS_MAX_SCREENS; i++) {
		cJSON_AddItemToArray(sc, screen_to_cjson(&snap->screens[i]));
	}

	cJSON *cl = cJSON_AddArrayToObject(root, "clients");
	for (uint32_t i = 0; i < snap->client_count && i < XRT_STATUS_MAX_CLIENTS; i++) {
		cJSON_AddItemToArray(cl, client_to_cjson(&snap->clients[i]));
	}

	cJSON *ws = cJSON_AddObjectToObject(root, "workspace");
	cJSON_AddBoolToObject(ws, "enabled", snap->workspace.enabled);
	add_str_or_null(ws, "controller", snap->workspace.controller);

	add_warnings(root, snap->warnings, snap->warning_count);
	return root;
}


/*
 *
 * Text (§7).
 *
 */

//! Screen-table column widths (characters), design §7.
#define STATUS_COL_IDX 2
#define STATUS_COL_DEVICE 13
#define STATUS_COL_NAME 20
#define STATUS_COL_DESKTOP 22
#define STATUS_COL_MODE 13
#define STATUS_COL_CLAIM 21
#define STATUS_COL_TRACKING 12
#define STATUS_COL_LENS 5
#define STATUS_COL_VENDOR 15

struct text_buf
{
	char *buf;
	size_t cap;
	size_t len; //!< Length the full text needs so far.
};

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void
tb_printf(struct text_buf *t, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	const size_t room = t->len < t->cap ? t->cap - t->len : 0;
	const int n = vsnprintf(room > 0 ? t->buf + t->len : NULL, room, fmt, ap);
	va_end(ap);
	if (n > 0) {
		t->len += (size_t)n;
	}
}

/*!
 * One table cell: @p s cut to @p width characters (code points, so "—" and
 * "·" count as one column) and padded with spaces to @p width, then one space.
 * A cut never splits a UTF-8 sequence.
 */
static void
tb_cell(struct text_buf *t, const char *s, uint32_t width)
{
	char cell[256];
	size_t out = 0;
	uint32_t cols = 0;
	for (size_t i = 0; s[i] != '\0';) {
		size_t n = 1;
		const unsigned char c = (unsigned char)s[i];
		if (c >= 0xF0) {
			n = 4;
		} else if (c >= 0xE0) {
			n = 3;
		} else if (c >= 0xC0) {
			n = 2;
		}
		if (cols == width || out + n >= sizeof(cell)) {
			break;
		}
		size_t k = 0;
		while (k < n && s[i + k] != '\0') {
			cell[out++] = s[i + k];
			k++;
		}
		i += k;
		cols++;
	}
	while (cols < width && out + 1 < sizeof(cell)) {
		cell[out++] = ' ';
		cols++;
	}
	cell[out] = '\0';
	tb_printf(t, "%s ", cell);
}

//! "\\.\DISPLAY5" -> "DISPLAY5"; empty -> the hex id.
static const char *
short_screen_name(const struct xrt_status_snapshot *snap, uint64_t id, char *buf, size_t cap)
{
	for (uint32_t i = 0; i < snap->screen_count && i < XRT_STATUS_MAX_SCREENS; i++) {
		const struct xrt_status_screen *s = &snap->screens[i];
		if (s->id != id || s->device_name[0] == '\0') {
			continue;
		}
		const char *n = s->device_name;
		if (strncmp(n, "\\\\.\\", 4) == 0) {
			n += 4;
		}
		(void)snprintf(buf, cap, "%s", n);
		return buf;
	}
	if (id == 0) {
		(void)snprintf(buf, cap, "—");
	} else {
		(void)snprintf(buf, cap, "0x%016llx", (unsigned long long)id);
	}
	return buf;
}

//! A long serial as "QALA…0011"; short ones verbatim.
static void
abbrev_serial(const char *serial, char *buf, size_t cap)
{
	const size_t n = strlen(serial);
	if (n > 10) {
		(void)snprintf(buf, cap, "%.4s…%s", serial, serial + n - 4);
	} else {
		(void)snprintf(buf, cap, "%s", serial);
	}
}

static void
format_screen(struct text_buf *t, const struct xrt_status_screen *s)
{
	char desk[64];
	if (s->desktop.scale > 0.0f) {
		(void)snprintf(desk, sizeof(desk), "%ux%u @%d,%d x%g", s->desktop.width, s->desktop.height,
		               s->desktop.left, s->desktop.top, (double)s->desktop.scale);
	} else {
		(void)snprintf(desk, sizeof(desk), "%ux%u @%d,%d", s->desktop.width, s->desktop.height, s->desktop.left,
		               s->desktop.top);
	}
	char mode[32];
	if (s->native.refresh_mhz > 0) {
		(void)snprintf(mode, sizeof(mode), "%ux%u@%u", s->desktop.width, s->desktop.height,
		               (s->native.refresh_mhz + 500u) / 1000u);
	} else {
		(void)snprintf(mode, sizeof(mode), "%ux%u", s->desktop.width, s->desktop.height);
	}
	char claim[96];
	if (s->claim.plugin_id[0] != '\0') {
		(void)snprintf(claim, sizeof(claim), "%s %s", s->claim.plugin_id,
		               u_status_confidence_str(s->claim.confidence));
	} else {
		(void)snprintf(claim, sizeof(claim), "unclaimed");
	}
	const char *lens = s->vendor.present ? u_status_vendor_lens_str(s->vendor.lens) : "—";
	char vendor[64];
	if (s->vendor.present) {
		char ser[32];
		abbrev_serial(s->vendor.serial, ser, sizeof(ser));
		(void)snprintf(vendor, sizeof(vendor), "%s  %s", s->vendor.model, ser);
	} else {
		(void)snprintf(vendor, sizeof(vendor), "—");
	}
	char warns[160] = "";
	size_t wl = 0;
	for (uint32_t i = 0; i < s->warning_count && i < XRT_STATUS_MAX_WARNINGS; i++) {
		int n = snprintf(warns + wl, sizeof(warns) - wl, "%s%s", i > 0 ? "," : "", s->warnings[i].code);
		if (n < 0 || (size_t)n >= sizeof(warns) - wl) {
			break;
		}
		wl += (size_t)n;
	}
	if (warns[0] == '\0') {
		(void)snprintf(warns, sizeof(warns), "—");
	}

	char idx[16];
	(void)snprintf(idx, sizeof(idx), "%u", s->index);
	tb_printf(t, " ");
	tb_cell(t, idx, STATUS_COL_IDX);
	tb_cell(t, s->device_name[0] != '\0' ? s->device_name : "—", STATUS_COL_DEVICE);
	tb_cell(t, s->friendly_name[0] != '\0' ? s->friendly_name : "—", STATUS_COL_NAME);
	tb_cell(t, desk, STATUS_COL_DESKTOP);
	tb_cell(t, mode, STATUS_COL_MODE);
	tb_cell(t, claim, STATUS_COL_CLAIM);
	tb_cell(t, u_status_tracking_str(s->eye_tracking.state), STATUS_COL_TRACKING);
	tb_cell(t, lens, STATUS_COL_LENS);
	tb_cell(t, vendor, STATUS_COL_VENDOR);
	tb_printf(t, "%s\n", warns);

	// Roles + bound DPs.
	char roles[96] = "";
	size_t rl = 0;
	const struct
	{
		bool on;
		const char *name;
	} rs[] = {{s->roles.os_main, "OS main"},
	          {s->roles.runtime_default, "runtime default"},
	          {s->roles.vendor_primary, "vendor primary"}};
	for (size_t i = 0; i < sizeof(rs) / sizeof(rs[0]); i++) {
		if (!rs[i].on) {
			continue;
		}
		int n = snprintf(roles + rl, sizeof(roles) - rl, "%s%s", rl > 0 ? " · " : "", rs[i].name);
		if (n > 0 && (size_t)n < sizeof(roles) - rl) {
			rl += (size_t)n;
		}
	}
	tb_printf(t, "    roles: %s · DPs: ", rl > 0 ? roles : "—");
	if (s->dp_count == 0) {
		tb_printf(t, "none");
	}
	for (uint32_t i = 0; i < s->dp_count && i < XRT_STATUS_MAX_SCREEN_DPS; i++) {
		const struct xrt_status_dp *dp = &s->dps[i];
		tb_printf(t, "%sclient %u %s %s (%s)", i > 0 ? ", " : "", dp->client_id, u_status_dp_api_str(dp->api),
		          u_status_dp_kind_str(dp->kind), u_status_dp_backend_str(dp->backend));
	}
	tb_printf(t, "\n");

	for (uint32_t i = 0; i < s->warning_count && i < XRT_STATUS_MAX_WARNINGS; i++) {
		const struct xrt_status_warning *w = &s->warnings[i];
		tb_printf(t, "    [%s] %s: %s\n", u_status_level_str(w->level), w->code, w->text);
	}
}

static void
format_client(struct text_buf *t, const struct xrt_status_snapshot *snap, const struct xrt_status_client *c)
{
	char owner[80];
	short_screen_name(snap, c->owner_screen, owner, sizeof(owner));
	tb_printf(t, " %-2u %s  %s  %s  lease %s", c->id, c->name[0] != '\0' ? c->name : "?",
	          u_status_client_class_str(c->client_class, c->class_verified), u_status_presenter_str(c->presenter),
	          u_status_lease_str(c->lease));
	if (c->window.valid) {
		tb_printf(t, "  window %d,%d %ux%u", c->window.left, c->window.top, c->window.width, c->window.height);
	}
	tb_printf(t, "  owner %s\n", owner);

	tb_printf(t, "    segments gen %llu:", (unsigned long long)c->segments.generation);
	if (c->segments.count == 0) {
		tb_printf(t, " none");
	}
	for (uint32_t i = 0; i < c->segments.count && i < XRT_STATUS_MAX_SEGMENTS; i++) {
		const struct xrt_status_segment *it = &c->segments.items[i];
		char sn[80];
		short_screen_name(snap, it->screen, sn, sizeof(sn));
		tb_printf(t, " [%s %d,%d %ux%u %s eyes=%s]", sn, it->canvas.x, it->canvas.y, it->canvas.w, it->canvas.h,
		          it->woven ? "woven" : (it->has_dp ? "flat" : "flat no-DP"),
		          u_status_eye_source_str(it->eye_source));
	}
	tb_printf(t, "\n");
	tb_printf(t, "    views %u/%u (reported %u)   paint %llu present %llu skip %llu   weave: %s\n", c->views.active,
	          c->views.capacity, c->views.reported, (unsigned long long)c->integrity.paint,
	          (unsigned long long)c->integrity.present, (unsigned long long)c->integrity.skip,
	          c->integrity.weave_placement[0] != '\0' ? c->integrity.weave_placement : "—");
}

size_t
u_status_snapshot_format_text(const struct xrt_status_snapshot *snap, char *buf, size_t cap)
{
	struct text_buf t = {buf, buf != NULL ? cap : 0, 0};
	if (t.cap > 0) {
		t.buf[0] = '\0';
	}
	if (snap == NULL) {
		return 0;
	}

	// Header.
	tb_printf(&t, "DisplayXR %s (%s)  ·  source: %s", snap->runtime.version, snap->runtime.git_tag,
	          u_status_source_str(snap->source));
	if (snap->source == XRT_STATUS_SOURCE_SERVICE) {
		tb_printf(&t, " (gen %llu/%llu)", (unsigned long long)snap->generation.topology,
		          (unsigned long long)snap->generation.status);
	}
	tb_printf(&t, "  ·  plug-ins: ");
	if (snap->plugin_count == 0) {
		tb_printf(&t, "none");
	}
	for (uint32_t i = 0; i < snap->plugin_count && i < XRT_STATUS_MAX_PLUGINS; i++) {
		const struct xrt_status_plugin *p = &snap->plugins[i];
		const char *state = p->fallback ? "fallback"
		                    : p->platform_state != XRT_PLUGIN_PLATFORM_STATE_UNKNOWN
		                        ? u_status_platform_state_str(p->platform_state)
		                        : p->load;
		tb_printf(&t, "%s%s %s", i > 0 ? ", " : "", p->id, state);
	}
	tb_printf(&t, "\n");

	// Screens.
	uint32_t claimed = 0, verified = 0, tracking = 0;
	const uint32_t ns = snap->screen_count < XRT_STATUS_MAX_SCREENS ? snap->screen_count : XRT_STATUS_MAX_SCREENS;
	for (uint32_t i = 0; i < ns; i++) {
		const struct xrt_status_screen *s = &snap->screens[i];
		claimed += s->claim.plugin_id[0] != '\0' ? 1u : 0u;
		verified += s->claim.confidence == (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED ? 1u : 0u;
		tracking += s->eye_tracking.state == XRT_STATUS_TRACKING_TRACKING ? 1u : 0u;
	}
	tb_printf(&t, "SCREENS  %u monitor%s · %u claimed (%u VERIFIED) · %u tracking\n", ns, ns == 1 ? "" : "s",
	          claimed, verified, tracking);
	tb_printf(&t, " ");
	tb_cell(&t, "#", STATUS_COL_IDX);
	tb_cell(&t, "device", STATUS_COL_DEVICE);
	tb_cell(&t, "name", STATUS_COL_NAME);
	tb_cell(&t, "desktop", STATUS_COL_DESKTOP);
	tb_cell(&t, "mode", STATUS_COL_MODE);
	tb_cell(&t, "claim", STATUS_COL_CLAIM);
	tb_cell(&t, "tracking", STATUS_COL_TRACKING);
	tb_cell(&t, "lens", STATUS_COL_LENS);
	tb_cell(&t, "vendor", STATUS_COL_VENDOR);
	tb_printf(&t, "warnings\n");
	for (uint32_t i = 0; i < ns; i++) {
		format_screen(&t, &snap->screens[i]);
	}

	// Windows.
	const uint32_t nc = snap->client_count < XRT_STATUS_MAX_CLIENTS ? snap->client_count : XRT_STATUS_MAX_CLIENTS;
	if (snap->source == XRT_STATUS_SOURCE_HEADLESS) {
		tb_printf(&t, "WINDOWS  — (headless: no live clients)\n");
	} else {
		tb_printf(&t, "WINDOWS  %u client%s\n", nc, nc == 1 ? "" : "s");
		for (uint32_t i = 0; i < nc; i++) {
			format_client(&t, snap, &snap->clients[i]);
		}
	}

	// Workspace + system warnings.
	if (snap->workspace.enabled) {
		tb_printf(&t, "WORKSPACE  on · controller %s\n",
		          snap->workspace.controller[0] != '\0' ? snap->workspace.controller : "?");
	}
	if (snap->warning_count > 0) {
		tb_printf(&t, "WARNINGS\n");
		for (uint32_t i = 0; i < snap->warning_count && i < XRT_STATUS_MAX_WARNINGS; i++) {
			const struct xrt_status_warning *w = &snap->warnings[i];
			tb_printf(&t, "  [%s] %s: %s\n", u_status_level_str(w->level), w->code, w->text);
		}
	}
	return t.len;
}
