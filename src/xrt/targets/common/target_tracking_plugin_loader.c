// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Registry / manifest-driven loader for viewer-tracking providers (ADR-054 N6 Part 2).
 *
 * A thin class wrapper on the shared component loader, the way ADR-054 N2
 * intends: discovery, the #952 / #328 / #434 load path, the N4b provenance
 * gate and the ADR-020 ABI gate are `target_component_loader.c`'s. What is
 * left here is what only this class knows: its host iface, what `probe()`
 * means (a usable source exists), the keep-every-claimer rule, the
 * `PreferredPlugin` hoist and the scan tally the self-test reads.
 *
 * @ingroup target_common
 */

#include "target_tracking_plugin_loader.h"
#include "target_component_loader.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"

#include "util/u_logging.h"

#include "os/os_threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * The class row (ADR-054 §2).
 *
 */

static bool
tracking_name_ieq(const char *name, const char *lower)
{
	for (size_t i = 0;; i++) {
		const char a = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + ('a' - 'A') : name[i]);
		if (a != lower[i]) {
			return false;
		}
		if (a == '\0') {
			return true;
		}
	}
}

//! `Policy` holds root policy (`Policy\Lock`), never a provider row.
static bool
tracking_is_reserved_subkey(const char *name)
{
	return tracking_name_ieq(name, "policy");
}

static const struct target_component_class g_tracking_class = {
    .name = "viewer-tracking provider",
    .log_tag = "tracking plugin loader",
    .entrypoint = XRT_TRACKING_PLUGIN_ENTRYPOINT_NAME,
    .abi_current = XRT_TRACKING_PLUGIN_API_VERSION_CURRENT,
    .registry_root = "Software\\DisplayXR\\TrackingProviders",
    .manifest_kind = TARGET_PLUGIN_MANIFEST_TRACKING,
    // Android: deferred — vendor components ship inside the runtime APK there,
    // and no Android tracking provider exists yet.
    .android_prefix = NULL,
    .is_reserved_subkey = tracking_is_reserved_subkey,
    .read_extra = NULL,
    .env_class = "TRACKING", // DXR_TRACKING_<ID>_<KEY>
};

const struct target_component_class *
target_tracking_plugin_component_class(void)
{
	return &g_tracking_class;
}


/*
 *
 * Host iface (process-lifetime storage; a provider may keep the pointer).
 *
 */

static struct xrt_tracking_plugin_host_iface g_host;
static bool g_host_filled = false;

static struct os_mutex g_geo_mutex;
static bool g_geo_mutex_ready = false;
static struct xrt_tracking_host_display_geometry g_geo;
static bool g_geo_ready = false;

static void
geo_lock_init_once(void)
{
	if (!g_geo_mutex_ready) {
		g_geo_mutex_ready = os_mutex_init(&g_geo_mutex) == 0;
	}
}

static xrt_result_t
host_get_display_geometry(struct xrt_tracking_host_display_geometry *inout)
{
	if (inout == NULL || inout->struct_size < sizeof(uint32_t)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (g_geo_mutex_ready) {
		os_mutex_lock(&g_geo_mutex);
	}
	const bool ready = g_geo_ready;
	struct xrt_tracking_host_display_geometry snap = g_geo;
	if (g_geo_mutex_ready) {
		os_mutex_unlock(&g_geo_mutex);
	}
	if (!ready) {
		return XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY;
	}
	const uint32_t caller = inout->struct_size;
	size_t copy = caller < sizeof(snap) ? caller : sizeof(snap);
	memcpy(inout, &snap, copy);
	inout->struct_size = caller;
	return XRT_SUCCESS;
}

static bool
host_get_setting(const char *id, const char *key, char *buf, uint32_t buf_size)
{
	return target_component_get_setting(&g_tracking_class, id, key, buf, buf_size, NULL);
}

const struct xrt_tracking_plugin_host_iface *
target_tracking_plugin_get_host_iface(void)
{
	if (!g_host_filled) {
		geo_lock_init_once();
		memset(&g_host, 0, sizeof(g_host));
		g_host.struct_size = (uint32_t)sizeof(g_host);
		g_host.host_api_version = XRT_TRACKING_PLUGIN_API_VERSION_CURRENT;
		g_host.get_setting = host_get_setting;
		g_host.get_display_geometry = host_get_display_geometry;
		g_host_filled = true;
	}
	return &g_host;
}

void
target_tracking_plugin_set_display_geometry(const struct xrt_tracking_host_display_geometry *geometry)
{
	if (geometry == NULL) {
		return;
	}
	geo_lock_init_once();
	if (g_geo_mutex_ready) {
		os_mutex_lock(&g_geo_mutex);
	}
	g_geo = *geometry;
	g_geo.struct_size = (uint32_t)sizeof(g_geo);
	g_geo_ready = true;
	if (g_geo_mutex_ready) {
		os_mutex_unlock(&g_geo_mutex);
	}
}


/*
 *
 * Scan state.
 *
 */

static bool g_scanned = false;
static int g_scan_registered = 0;
static int g_scan_enabled = 0;
static int g_scan_declined = 0;
static int g_scan_failed = 0;
static int g_scan_untrusted = 0;

static struct tracking_slot
{
	const struct xrt_tracking_plugin_iface *iface;
	struct xrt_tracking_plugin_instance *inst;
	uint32_t priority;
	char row_id[64];
} g_slots[TARGET_COMPONENT_MAX_ENTRIES];
static int g_slot_count = 0;

bool
target_tracking_plugin_disabled_by_env(void)
{
	const char *env = getenv("DXR_TRACKING_PROVIDERS");
	return env != NULL && env[0] == '0' && env[1] == '\0';
}

static const char *
tracking_enabled_source(const struct target_component_entry *e)
{
	if ((e->flags & TARGET_COMPONENT_FLAG_USER_OVERLAY) != 0) {
		return "user";
	}
	if (!e->enabled) {
#ifdef XRT_OS_WINDOWS
		return "machine";
#else
		return "manifest";
#endif
	}
	return "default";
}

//! Move the row named @p preferred to the front (stable for the rest). True when one matched.
static bool
tracking_hoist_preferred(struct target_component_entry *entries, int n, const char *preferred)
{
	if (preferred == NULL || preferred[0] == '\0') {
		return false;
	}
	for (int i = 0; i < n; i++) {
		if (strcmp(entries[i].id, preferred) != 0) {
			continue;
		}
		if (i > 0) {
			struct target_component_entry *tmp = (struct target_component_entry *)malloc(sizeof(*tmp));
			if (tmp == NULL) {
				return true;
			}
			*tmp = entries[i];
			memmove(&entries[1], &entries[0], (size_t)i * sizeof(entries[0]));
			entries[0] = *tmp;
			free(tmp);
		}
		return true;
	}
	return false;
}

/*!
 * Load + negotiate + ABI gate + probe one row. NULL on any failure (module
 * unloaded again); @p out_declined = dispatched fine and said "no usable
 * source"; @p out_untrusted = refused by the provenance gate. A success leaks
 * the module for the process (the vtable lives in it).
 */
static const struct xrt_tracking_plugin_iface *
tracking_try_load_one(struct target_component_entry *e,
                      struct xrt_tracking_plugin_instance **out_inst,
                      bool *out_declined,
                      bool *out_untrusted)
{
	*out_inst = NULL;
	*out_declined = false;
	*out_untrusted = false;

	void *module = NULL;
	void *fn = NULL;
	const enum target_component_load_result lr =
	    target_component_load_binary(&g_tracking_class, e, &module, &fn, NULL, NULL, 0);
	if (lr != TARGET_COMPONENT_LOAD_OK) {
		*out_untrusted = lr == TARGET_COMPONENT_LOAD_UNTRUSTED;
		return NULL;
	}

	struct xrt_tracking_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = ((xrt_tracking_plugin_negotiate_fn_t)fn)(
	    XRT_TRACKING_PLUGIN_API_VERSION_CURRENT, target_tracking_plugin_get_host_iface(), &iface, &plugin_version);
	const enum target_component_abi_verdict verdict =
	    target_component_abi_gate(&g_tracking_class, e->id, xret, iface, plugin_version, NULL, 0);
	if (verdict != TARGET_COMPONENT_ABI_OK) {
		*out_declined = verdict == TARGET_COMPONENT_ABI_DECLINED;
		target_component_unload(module);
		return NULL;
	}
	// v1 slots are mandatory: a provider without start/stop cannot serve a viewer.
	if (iface->start == NULL || iface->stop == NULL) {
		U_LOG_W("tracking plugin loader:   %s: vtable has no start/stop — skipping.", e->id);
		target_component_unload(module);
		return NULL;
	}
	if (iface->probe != NULL) {
		xret = iface->probe(out_inst);
		if (xret == XRT_ERROR_PROBER_NOT_SUPPORTED) {
			U_LOG_I("tracking plugin loader:   %s: probe declined (no usable source).", e->id);
			*out_declined = true;
			target_component_unload(module);
			return NULL;
		}
		if (xret != XRT_SUCCESS) {
			U_LOG_W("tracking plugin loader:   %s: probe returned %d — skipping.", e->id, (int)xret);
			target_component_unload(module);
			return NULL;
		}
	}
	U_LOG_W(
	    "tracking plugin loader: resident viewer-tracking provider: id=%s name='%s' plugin_api=%u probe_order=%u "
	    "path=%s",
	    iface->id ? iface->id : e->id, iface->display_name ? iface->display_name : e->display_name, plugin_version,
	    e->probe_order, e->binary_path);
	return iface;
}

static void
tracking_scan(void)
{
	if (g_scanned) {
		return;
	}
	g_scanned = true;
	if (target_tracking_plugin_disabled_by_env()) {
		U_LOG_W("tracking plugin loader: DXR_TRACKING_PROVIDERS=0 — no viewer-tracking provider is loaded.");
		return;
	}

	struct target_component_entry *entries =
	    (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(*entries));
	if (entries == NULL) {
		return;
	}
	const int n = target_component_enumerate(&g_tracking_class, entries, TARGET_COMPONENT_MAX_ENTRIES);
	(void)target_component_apply_user_overlay(&g_tracking_class, entries, (size_t)n);

	char preferred[64] = {0};
	const bool have_pref = target_tracking_plugin_get_preferred(preferred, sizeof(preferred));
	if (have_pref && !tracking_hoist_preferred(entries, n, preferred)) {
		U_LOG_W("tracking plugin loader: PreferredPlugin '%s' is not a registered provider — ignored.",
		        preferred);
	}

	for (int i = 0; i < n; i++) {
		struct target_component_entry *e = &entries[i];
		if ((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0) {
			U_LOG_W("tracking plugin loader:   %s: %s — skipping.", e->source, e->reason);
			continue;
		}
		g_scan_registered++;
		if (!e->enabled) {
			U_LOG_I("tracking plugin loader:   %s disabled (Enabled=0, %s) — skipping.", e->id,
			        tracking_enabled_source(e));
			continue;
		}
		g_scan_enabled++;
		const bool is_pref = have_pref && strcmp(e->id, preferred) == 0;
		struct xrt_tracking_plugin_instance *inst = NULL;
		bool declined = false, untrusted = false;
		const struct xrt_tracking_plugin_iface *iface = tracking_try_load_one(e, &inst, &declined, &untrusted);
		if (iface != NULL) {
			if (g_slot_count < TARGET_COMPONENT_MAX_ENTRIES) {
				struct tracking_slot *s = &g_slots[g_slot_count++];
				s->iface = iface;
				s->inst = inst;
				s->priority = is_pref ? 0u : e->probe_order;
				snprintf(s->row_id, sizeof(s->row_id), "%s", e->id);
			}
		} else if (declined) {
			g_scan_declined++;
		} else if (untrusted) {
			g_scan_untrusted++;
		} else {
			g_scan_failed++;
		}
	}
	free(entries);
	U_LOG_I(
	    "tracking plugin loader: %d resident (%d registered, %d enabled, %d declined, %d failed, %d untrusted).",
	    g_slot_count, g_scan_registered, g_scan_enabled, g_scan_declined, g_scan_failed, g_scan_untrusted);
}

int
target_tracking_plugin_get_count(void)
{
	tracking_scan();
	return g_slot_count;
}

const struct xrt_tracking_plugin_iface *
target_tracking_plugin_get_iface(int index)
{
	tracking_scan();
	return (index >= 0 && index < g_slot_count) ? g_slots[index].iface : NULL;
}

struct xrt_tracking_plugin_instance *
target_tracking_plugin_get_instance(int index)
{
	tracking_scan();
	return (index >= 0 && index < g_slot_count) ? g_slots[index].inst : NULL;
}

const char *
target_tracking_plugin_get_row_id(int index)
{
	tracking_scan();
	return (index >= 0 && index < g_slot_count) ? g_slots[index].row_id : "";
}

uint32_t
target_tracking_plugin_get_priority(int index)
{
	tracking_scan();
	return (index >= 0 && index < g_slot_count) ? g_slots[index].priority : 0xFFFFFFFFu;
}

void
target_tracking_plugin_get_scan_result(
    int *out_registered, int *out_enabled, int *out_declined, int *out_failed, int *out_untrusted)
{
	tracking_scan();
	if (out_registered != NULL) {
		*out_registered = g_scan_registered;
	}
	if (out_enabled != NULL) {
		*out_enabled = g_scan_enabled;
	}
	if (out_declined != NULL) {
		*out_declined = g_scan_declined;
	}
	if (out_failed != NULL) {
		*out_failed = g_scan_failed;
	}
	if (out_untrusted != NULL) {
		*out_untrusted = g_scan_untrusted;
	}
}


/*
 *
 * Listing + settings (no load except describe's negotiate).
 *
 */

int
target_tracking_plugin_enumerate(struct target_tracking_plugin_desc *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}
	struct target_component_entry *entries =
	    (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(*entries));
	if (entries == NULL) {
		return 0;
	}
	const int n = target_component_enumerate(&g_tracking_class, entries, TARGET_COMPONENT_MAX_ENTRIES);
	(void)target_component_apply_user_overlay(&g_tracking_class, entries, (size_t)n);
	int count = 0;
	for (int i = 0; i < n && count < max; i++) {
		struct target_component_entry *e = &entries[i];
		if ((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0) {
			continue;
		}
		struct target_tracking_plugin_desc *d = &out[count++];
		memset(d, 0, sizeof(*d));
		snprintf(d->id, sizeof(d->id), "%s", e->id);
		snprintf(d->display_name, sizeof(d->display_name), "%s", e->display_name);
		snprintf(d->vendor, sizeof(d->vendor), "%s", e->vendor);
		snprintf(d->version, sizeof(d->version), "%s", e->version);
		snprintf(d->binary_path, sizeof(d->binary_path), "%s", e->binary_path);
		d->probe_order = e->probe_order;
		d->enabled = e->enabled;
		snprintf(d->enabled_source, sizeof(d->enabled_source), "%s", tracking_enabled_source(e));
		(void)target_component_check_provenance(&g_tracking_class, e);
		d->provenance = e->provenance;
		snprintf(d->provenance_reason, sizeof(d->provenance_reason), "%s", e->provenance_reason);
		snprintf(d->signer, sizeof(d->signer), "%s", e->signer);
	}
	free(entries);
	return count;
}

int
target_tracking_plugin_describe_settings(const char *id, struct target_component_setting_desc *out, int max)
{
	if (id == NULL || id[0] == '\0' || out == NULL || max <= 0) {
		return 0;
	}
	int n = target_component_manifest_settings_schema(&g_tracking_class, id, out, max);
	struct target_component_entry *entries =
	    (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(*entries));
	if (entries == NULL) {
		return n;
	}
	const int count = target_component_enumerate(&g_tracking_class, entries, TARGET_COMPONENT_MAX_ENTRIES);
	for (int i = 0; i < count; i++) {
		struct target_component_entry *e = &entries[i];
		if ((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0 || strcmp(e->id, id) != 0) {
			continue;
		}
		void *module = NULL;
		void *fn = NULL;
		if (target_component_load_binary_ex(&g_tracking_class, e, TARGET_COMPONENT_LOAD_FLAG_QUIET, &module,
		                                    &fn, NULL, NULL, 0) != TARGET_COMPONENT_LOAD_OK) {
			break;
		}
		struct xrt_tracking_plugin_iface *iface = NULL;
		uint32_t ver = 0;
		const xrt_result_t xret = ((xrt_tracking_plugin_negotiate_fn_t)fn)(
		    XRT_TRACKING_PLUGIN_API_VERSION_CURRENT, target_tracking_plugin_get_host_iface(), &iface, &ver);
		if (target_component_abi_gate(&g_tracking_class, e->id, xret, iface, ver, NULL, 0) ==
		        TARGET_COMPONENT_ABI_OK &&
		    XRT_TRACKING_PLUGIN_IFACE_HAS(iface, describe_settings)) {
			const struct xrt_component_setting_desc *descs = NULL;
			const uint32_t nd = iface->describe_settings(&descs);
			n = target_component_merge_setting_descs(out, n, max, descs, nd);
		}
		// Module loads are refcounted: a provider the scan made resident keeps
		// the scan's own (leaked) reference, so this unload only drops ours.
		target_component_unload(module);
		break;
	}
	free(entries);
	return n;
}


/*
 *
 * PreferredPlugin (class selection policy — the shared loader never selects).
 *
 */

#ifdef XRT_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

bool
target_tracking_plugin_get_preferred(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	wchar_t wbuf[64];
	DWORD bytes = sizeof(wbuf);
	LSTATUS rc = ERROR_FILE_NOT_FOUND;
	if (!target_component_policy_lock(&g_tracking_class)) {
		rc = RegGetValueW(HKEY_CURRENT_USER, L"Software\\DisplayXR\\TrackingProviders", L"PreferredPlugin",
		                  RRF_RT_REG_SZ, NULL, wbuf, &bytes);
	}
	if (rc != ERROR_SUCCESS || wbuf[0] == L'\0') {
		bytes = sizeof(wbuf);
		rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\TrackingProviders", L"PreferredPlugin",
		                  RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, wbuf, &bytes);
	}
	if (rc != ERROR_SUCCESS) {
		return false;
	}
	if (WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)cap, NULL, NULL) <= 0) {
		out[0] = '\0';
	}
	return out[0] != '\0';
}

#elif defined(XRT_OS_ANDROID)

bool
target_tracking_plugin_get_preferred(char *out, size_t cap)
{
	if (out != NULL && cap > 0) {
		out[0] = '\0';
	}
	return false;
}

#else /* POSIX desktop */

#include <limits.h>

bool
target_tracking_plugin_get_preferred(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	const char *env = getenv("XRT_PREFERRED_TRACKING_PLUGIN_ID");
	if (env != NULL && *env != '\0') {
		snprintf(out, cap, "%s", env);
		return true;
	}
	char dir[PATH_MAX];
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return false;
	}
	char path[PATH_MAX + 32];
	snprintf(path, sizeof(path), "%s/tracking-preferred", dir);
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	char buf[64] = {0};
	const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[got] = '\0';
	size_t len = strlen(buf);
	while (len > 0 &&
	       (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
		buf[--len] = '\0';
	}
	snprintf(out, cap, "%s", buf);
	return out[0] != '\0';
}

#endif
