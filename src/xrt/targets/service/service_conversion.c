// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The service's conversion core (see service_conversion.h).
 * @ingroup targets_service
 */

#include "service_conversion.h"
#include "service_component_watch.h"
#include "service_config.h"

#include "xrt/xrt_config_os.h"

#include "conversion/conv_core.h"
#include "conversion/conv_observer_autoconv.h"
#include "conversion/conv_provider_legacy_dp.h"
#include "conversion/conv_registry.h"

#include "server/ipc_server.h"
#include "server/ipc_server_conv.h"

#include "target_component_loader.h"
#include "target_conv_status.h"
#include "target_conversion_plugin_loader.h"

#include "os/os_threading.h"
#include "util/u_logging.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef XRT_OS_WINDOWS
#include "xrt/xrt_windows.h"
#endif

#define CACHE_FILENAME "conversion-describe-cache.bin"
#define CACHE_MAX_BYTES (4u * 1024u * 1024u)

static struct
{
	bool started;
	struct conv_core *core;
	struct ipc_server_conv *sc;

	//! Guards entries / entry_count / policy and the status scratch below.
	struct os_mutex mtx;
	struct xrt_conversion_plugin_entry *entries;
	int entry_count;
	struct target_conv_policy policy;
	struct conv_registry *reg_scratch;
	struct target_conv_status *status_scratch;

	//! #2003's allocator (NULL alloc = CPU); guarded by alloc_mtx.
	struct os_mutex alloc_mtx;
	struct os_cond alloc_cond;
	struct conv_image_alloc_ops gpu_alloc;
	uint32_t gpu_alloc_calls; //!< GPU allocator calls in flight (a removal waits for 0)

	//! The legacy DP-lift adapter (#2003), registered once the GPU side attached.
	const struct xrt_conversion_plugin_iface *dp_lift_iface;
	struct xrt_conversion_plugin_instance *dp_lift_inst;
	bool dp_lift_registered;

	char cache_path[600];
} g;


/*
 *
 * Image allocation: CPU until #2003 installs the GPU glue.
 *
 */

static uint32_t
bpp_of(uint32_t fmt)
{
	switch (fmt) {
	case XRT_CONV_FMT_R8_UNORM: return 1;
	case XRT_CONV_FMT_R16_FLOAT:
	case XRT_CONV_FMT_R16_UNORM: return 2;
	case XRT_CONV_FMT_RGBA16_FLOAT: return 8;
	default: return 4;
	}
}

static bool
cpu_alloc(const struct conv_image_request *req, struct xrt_conv_image *out)
{
	static uint64_t s_key = 0;
	const uint32_t layers = req->array_layers != 0 ? req->array_layers : 1;
	const uint64_t size = (uint64_t)req->width * req->height * bpp_of(req->format) * layers;
	if (size == 0 || size > (uint64_t)1 << 31) {
		return false;
	}
	void *p = calloc(1, (size_t)size);
	if (p == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->memory_kind = XRT_CONV_MEM_CPU;
	out->cpu_ptr = p;
	out->byte_size = size;
	out->width = req->width;
	out->height = req->height;
	out->format = req->format;
	out->row_stride = req->width * bpp_of(req->format);
	os_mutex_lock(&g.alloc_mtx);
	out->import_key = ++s_key | (1ull << 63); // disjoint from any GPU allocator's keys
	os_mutex_unlock(&g.alloc_mtx);
	out->color_space = req->role == 0 ? XRT_CONV_CS_SRGB : XRT_CONV_CS_DATA;
	return true;
}

//! Take the GPU allocator for one call (false = none installed).
static bool
gpu_enter(struct conv_image_alloc_ops *out)
{
	os_mutex_lock(&g.alloc_mtx);
	*out = g.gpu_alloc;
	const bool ok = out->alloc != NULL;
	if (ok) {
		g.gpu_alloc_calls++;
	}
	os_mutex_unlock(&g.alloc_mtx);
	return ok;
}

static void
gpu_leave(void)
{
	os_mutex_lock(&g.alloc_mtx);
	g.gpu_alloc_calls--;
	os_cond_broadcast(&g.alloc_cond);
	os_mutex_unlock(&g.alloc_mtx);
}

static bool
tramp_alloc(void *ud, const struct conv_image_request *req, struct xrt_conv_image *out)
{
	(void)ud;
	struct conv_image_alloc_ops gpu;
	if (gpu_enter(&gpu)) {
		const bool ok = gpu.alloc(gpu.ud, req, out);
		gpu_leave();
		if (ok) {
			return true;
		}
	}
	// The GPU glue declined (a provider that imports no shared texture with a
	// keyed mutex) or is not installed: CPU memory, when the provider takes it.
	if (req->import_memory_kinds != 0 && (req->import_memory_kinds & XRT_CONV_MEM_CPU) == 0) {
		return false;
	}
	return cpu_alloc(req, out);
}

static void
tramp_free(void *ud, struct xrt_conv_image *img)
{
	(void)ud;
	if (img->memory_kind == XRT_CONV_MEM_CPU && (img->import_key >> 63) != 0) {
		free(img->cpu_ptr);
		img->cpu_ptr = NULL;
		return;
	}
	struct conv_image_alloc_ops gpu;
	if (gpu_enter(&gpu)) {
		if (gpu.free != NULL) {
			gpu.free(gpu.ud, img);
		}
		gpu_leave();
	}
}

//! #2030: a CPU result into a GPU mirror (the GPU allocator made the mirror).
static bool
tramp_upload(void *ud, const struct xrt_conv_image *src, const struct xrt_conv_image *dst)
{
	(void)ud;
	bool ok = false;
	struct conv_image_alloc_ops gpu;
	if (gpu_enter(&gpu)) {
		ok = gpu.upload != NULL && gpu.upload(gpu.ud, src, dst);
		gpu_leave();
	}
	return ok;
}

void
service_conversion_set_gpu_alloc(const struct conv_image_alloc_ops *ops)
{
	if (!g.started) {
		return;
	}
	os_mutex_lock(&g.alloc_mtx);
	if (ops != NULL) {
		g.gpu_alloc = *ops;
	} else {
		memset(&g.gpu_alloc, 0, sizeof(g.gpu_alloc));
		// Nothing may still be inside the allocator that is going away.
		while (g.gpu_alloc_calls > 0) {
			os_cond_wait(&g.alloc_cond, &g.alloc_mtx);
		}
	}
	os_mutex_unlock(&g.alloc_mtx);
}

/*
 *
 * The GPU side + the legacy DP-lift adapter (#2003).
 *
 */

static bool
dp_lift_enabled_by_policy(void)
{
	bool enabled = true;
	os_mutex_lock(&g.mtx);
	if (!g.policy.policy_lock) {
		for (uint32_t i = 0; i < g.policy.override_count; i++) {
			if (strcmp(g.policy.overrides[i].id, CONV_DP_LIFT_ID) == 0) {
				enabled = g.policy.overrides[i].enabled;
			}
		}
	}
	os_mutex_unlock(&g.mtx);
	return enabled;
}

//! ipc_server_conv_gpu_hook_fn: the GPU side came (or goes).
static void
on_gpu(void *ud, const struct ipc_server_conv_gpu *gpu)
{
	(void)ud;
	if (g.core == NULL) {
		return;
	}
	if (gpu == NULL) {
		// The backend goes away with the system compositor: the adapter answers
		// "no module" from now on (in-flight calls drain first).
		if (g.dp_lift_inst != NULL) {
			conv_provider_legacy_dp_detach(g.dp_lift_inst);
		}
		service_conversion_set_gpu_alloc(NULL);
		return;
	}
	service_conversion_set_gpu_alloc(&gpu->alloc);
	if (gpu->dp_lift.snapshot == NULL) {
		return;
	}
	if (g.dp_lift_registered) {
		conv_provider_legacy_dp_attach(g.dp_lift_inst, &gpu->dp_lift);
		conv_core_provider_changed(g.core, CONV_DP_LIFT_ID);
		return;
	}
	if (!conv_provider_legacy_dp_create(&gpu->dp_lift, &g.dp_lift_iface, &g.dp_lift_inst)) {
		return;
	}
	struct conv_provider_desc d;
	memset(&d, 0, sizeof(d));
	d.id = CONV_DP_LIFT_ID;
	d.probe_order = CONV_DP_LIFT_PROBE_ORDER;
	d.hosting = CONV_HOSTING_IN_SERVICE; // always: the display plug-in never leaves the service
	d.enabled = dp_lift_enabled_by_policy();
	d.flags = CONV_PROV_LICENCE_EXEMPT | CONV_PROV_LEGACY_UNITS;
	d.iface = g.dp_lift_iface;
	d.inst = g.dp_lift_inst;
	if (conv_core_add_provider(g.core, &d) >= 0) {
		g.dp_lift_registered = true;
		U_LOG_W("[conv] dp-lift registered: the display plug-in's lift slots, ProbeOrder %u, in-service%s",
		        CONV_DP_LIFT_PROBE_ORDER, d.enabled ? "" : " (disabled by policy)");
	}
}


/*
 *
 * Events and the status snapshot source.
 *
 */

static void
on_event(void *ud, uint64_t client_id, const struct xrt_conv_wire_event *ev)
{
	// Called with core locks held: queue only. v3 has no event; the status
	// snapshot's `conversion` counter follows the core's generations through
	// conv_status_generation below, so nothing to do here yet (the spec-4
	// per-session event queue lands with the v4 handlers, P3).
	(void)ud;
	(void)client_id;
	(void)ev;
}

static uint64_t
conv_status_generation(void *ud)
{
	(void)ud;
	if (g.core == NULL) {
		return 0;
	}
	struct xrt_conv_wire_generation gen;
	conv_core_get_generation(g.core, &gen);
	// Both move: conv_gen on a table change, status_gen on every readiness transition.
	return gen.conv_gen + gen.status_gen;
}

//! target_conv_activity_fn over the live core (#2029: the streams / jobs columns).
static void
row_activity(void *ud, const char *provider_id, uint32_t cap_id, bool composed, uint32_t *streams, uint32_t *jobs)
{
	(void)ud;
	conv_core_row_activity(g.core, provider_id, cap_id, composed, streams, jobs);
}

static void
conv_status_fill(void *ud, struct xrt_status_live *live)
{
	(void)ud;
	live->conv_provider_count = 0;
	live->conv_task_count = 0;
	if (g.core == NULL || g.reg_scratch == NULL || g.status_scratch == NULL) {
		return;
	}
	os_mutex_lock(&g.mtx);
	conv_core_copy_registry(g.core, g.reg_scratch);
	target_conv_status_from_registry(g.reg_scratch, g.entries, g.entry_count, &g.policy, g.status_scratch);
	target_conv_status_fill_activity(g.status_scratch, row_activity, NULL);
	const struct target_conv_status *st = g.status_scratch;
	live->conv_provider_count = st->provider_count;
	memcpy(live->conv_providers, st->providers, sizeof(live->conv_providers));
	live->conv_task_count = st->task_count;
	memcpy(live->conv_tasks, st->tasks, sizeof(live->conv_tasks));
	os_mutex_unlock(&g.mtx);
}


/*
 *
 * Providers.
 *
 */

static void
host_notify_changed(uint32_t cap_id)
{
	// Any provider thread, possibly inside a describe that holds the core's
	// registry lock: only signal; the service's component watch thread
	// re-describes (on_policy_watch below).
	(void)cap_id;
	(void)service_component_watch_poke(TARGET_COMPONENT_CLASS_CONVERSION, SERVICE_COMPONENT_WATCH_PROVIDER_NOTIFY);
}

static struct xrt_conversion_plugin_host_iface g_host;

/*
 * Per-provider host context (#2003): get_setting carries no provider
 * argument (spec §2.1), so every provider is negotiated with its OWN host
 * iface whose get_setting is a trampoline bound to its slot. It answers
 * `DXR_CONV_<ID>_<KEY>` from the service's environment, then the provider's
 * registry `Settings\*` / manifest "settings" — the keys the headless pass
 * (target_conv_status) serves, so a provider that gates its probe on a
 * setting (the sim provider's ENABLE) behaves the same in the service.
 */
#define HOST_SLOTS XRT_CONV_LOADER_MAX_ENTRIES

static struct
{
	bool used;
	char id[64];
	uint32_t setting_count;
	struct xrt_conversion_plugin_setting settings[XRT_CONV_LOADER_MAX_SETTINGS];
} g_host_slot[HOST_SLOTS];

static struct xrt_conversion_plugin_host_iface g_hosts[HOST_SLOTS];

static bool
host_get_setting_slot(uint32_t slot, const char *key, char *buf, uint32_t buf_size)
{
	if (slot >= HOST_SLOTS || key == NULL || buf == NULL || buf_size == 0 || !g_host_slot[slot].used) {
		return false;
	}
	char env[160];
	const int n = snprintf(env, sizeof(env), "DXR_CONV_%s_%s", g_host_slot[slot].id, key);
	if (n < 0 || (size_t)n >= sizeof(env)) {
		return false;
	}
	for (char *c = env; *c != '\0'; c++) {
		*c = *c == '-' ? '_' : (char)toupper((unsigned char)*c);
	}
	const char *v = getenv(env);
	if (v != NULL && v[0] != '\0') {
		(void)snprintf(buf, buf_size, "%s", v);
		return true;
	}
	for (uint32_t i = 0; i < g_host_slot[slot].setting_count && i < XRT_CONV_LOADER_MAX_SETTINGS; i++) {
#ifdef XRT_OS_WINDOWS
		if (_stricmp(g_host_slot[slot].settings[i].key, key) == 0) {
#else
		if (strcmp(g_host_slot[slot].settings[i].key, key) == 0) {
#endif
			(void)snprintf(buf, buf_size, "%s", g_host_slot[slot].settings[i].value);
			return true;
		}
	}
	return false;
}

#define HOST_GET_SETTING(i)                                                                                            \
	static bool host_get_setting_##i(const char *key, char *buf, uint32_t buf_size)                                \
	{                                                                                                              \
		return host_get_setting_slot(i, key, buf, buf_size);                                                   \
	}
HOST_GET_SETTING(0)
HOST_GET_SETTING(1)
HOST_GET_SETTING(2)
HOST_GET_SETTING(3)
HOST_GET_SETTING(4)
HOST_GET_SETTING(5)
HOST_GET_SETTING(6)
HOST_GET_SETTING(7)
HOST_GET_SETTING(8)
HOST_GET_SETTING(9)
HOST_GET_SETTING(10)
HOST_GET_SETTING(11)
HOST_GET_SETTING(12)
HOST_GET_SETTING(13)
HOST_GET_SETTING(14)
HOST_GET_SETTING(15)

static bool (*const k_host_get_setting[HOST_SLOTS])(const char *, char *, uint32_t) = {
    host_get_setting_0,  host_get_setting_1,  host_get_setting_2,  host_get_setting_3,
    host_get_setting_4,  host_get_setting_5,  host_get_setting_6,  host_get_setting_7,
    host_get_setting_8,  host_get_setting_9,  host_get_setting_10, host_get_setting_11,
    host_get_setting_12, host_get_setting_13, host_get_setting_14, host_get_setting_15,
};
_Static_assert(HOST_SLOTS == 16, "one get_setting trampoline per loader entry");

//! The host iface @p e negotiates with: its own slot (settings copied), or the shared one.
static const struct xrt_conversion_plugin_host_iface *
host_for_entry(const struct xrt_conversion_plugin_entry *e)
{
	uint32_t slot = HOST_SLOTS;
	for (uint32_t i = 0; i < HOST_SLOTS; i++) {
		if (g_host_slot[i].used && strcmp(g_host_slot[i].id, e->id) == 0) {
			slot = i;
			break;
		}
	}
	for (uint32_t i = 0; slot == HOST_SLOTS && i < HOST_SLOTS; i++) {
		if (!g_host_slot[i].used) {
			slot = i;
		}
	}
	if (slot == HOST_SLOTS) {
		return &g_host;
	}
	// The provider keeps this pointer until process end: slots are never reused.
	g_host_slot[slot].used = true;
	(void)snprintf(g_host_slot[slot].id, sizeof(g_host_slot[slot].id), "%s", e->id);
	g_host_slot[slot].setting_count =
	    e->setting_count < XRT_CONV_LOADER_MAX_SETTINGS ? e->setting_count : XRT_CONV_LOADER_MAX_SETTINGS;
	memcpy(g_host_slot[slot].settings, e->settings, sizeof(g_host_slot[slot].settings));
	g_hosts[slot] = g_host;
	g_hosts[slot].get_setting = k_host_get_setting[slot];
	return &g_hosts[slot];
}

static void
init_host(void)
{
	g_host = *target_conversion_plugin_default_host();
	g_host.notify_changed = host_notify_changed;
}

//! Is @p id already registered in the core? (g.mtx held)
static int
find_entry_locked(const char *id)
{
	for (int i = 0; i < g.entry_count; i++) {
		if (strcmp(g.entries[i].id, id) == 0) {
			return i;
		}
	}
	return -1;
}

/*!
 * The row's EFFECTIVE `enabled`: the loader's HKLM / manifest value with the
 * per-user HKCU override on top (spec §3.1, HKCU wins unless `Policy\Lock`).
 * #2051 (the #2025 item): the loader reads only HKLM, and until now the
 * service took its value verbatim — so `conv disable <id>` (HKCU) changed
 * nothing in a running core; only the headless CLI pass applied the override.
 * Reads g.policy under g.mtx; call with no lock held.
 */
static bool
effective_enabled(const struct xrt_conversion_plugin_entry *e)
{
	os_mutex_lock(&g.mtx);
	const int ov = target_conv_policy_enabled_override(&g.policy, e->id);
	os_mutex_unlock(&g.mtx);
	return ov < 0 ? e->enabled : ov == 1;
}

//! Negotiate + probe @p e and add it to the core (no g.mtx: calls a provider).
static void
add_entry(struct xrt_conversion_plugin_entry *e)
{
	if (target_conv_is_policy_subkey(e->id)) {
		return;
	}
	e->enabled = effective_enabled(e);
	if (e->status == XRT_CONV_LOAD_NOT_LOADED) {
		(void)target_conversion_plugin_negotiate_one(e, host_for_entry(e));
	}

	struct conv_provider_desc d;
	memset(&d, 0, sizeof(d));
	d.id = e->id;
	d.probe_order = e->probe_order;
	d.hosting = e->hosting == XRT_CONV_HOSTING_SATELLITE   ? CONV_HOSTING_SATELLITE
	            : e->hosting == XRT_CONV_HOSTING_IN_SERVICE ? CONV_HOSTING_IN_SERVICE
	                                                        : CONV_HOSTING_AUTO;
	d.supersedes = e->supersedes;
	d.binary_path = e->binary_path;
	struct stat sb;
	if (e->binary_path[0] != '\0' && stat(e->binary_path, &sb) == 0) {
		d.binary_size = (uint64_t)sb.st_size;
		d.binary_mtime = (uint64_t)sb.st_mtime;
	}
	// Only a negotiated provider is selectable; the others are listed (their
	// cached rows, if any, stay DISABLED) so `conv list` shows why.
	d.enabled = e->enabled && e->status == XRT_CONV_LOAD_NEGOTIATED;

	if (e->status == XRT_CONV_LOAD_NEGOTIATED) {
		struct xrt_conversion_plugin_instance *inst = NULL;
		xrt_result_t pr = XRT_SUCCESS;
		if (XRT_CONV_IFACE_HAS(e->iface, probe) && e->iface->probe != NULL) {
			pr = e->iface->probe(&inst); // [S], <= 100 ms, no model load (spec §2.3)
		}
		if (pr == XRT_SUCCESS) {
			d.iface = e->iface;
			d.inst = inst;
		} else {
			d.enabled = false;
			(void)snprintf(e->reason, sizeof(e->reason), "probe returned %d", (int)pr);
			U_LOG_W("[conv] provider %s: probe returned %d; listed, not used", e->id, (int)pr);
		}
	}
	(void)conv_core_add_provider(g.core, &d);
}

static void
cache_load(void)
{
	FILE *f = fopen(g.cache_path, "rb");
	if (f == NULL) {
		return; // first run
	}
	uint8_t *buf = malloc(CACHE_MAX_BYTES);
	size_t n = buf != NULL ? fread(buf, 1, CACHE_MAX_BYTES, f) : 0;
	fclose(f);
	if (n > 0) {
		const int32_t r = conv_core_load_describe_cache(g.core, buf, n);
		U_LOG_I("[conv] describe cache: %d rows restored from %s", (int)r, g.cache_path);
	}
	free(buf);
}

static void
cache_save(void)
{
	if (g.core == NULL || g.cache_path[0] == '\0') {
		return;
	}
	const size_t need = conv_core_save_describe_cache(g.core, NULL, 0);
	if (need == 0 || need > CACHE_MAX_BYTES) {
		return;
	}
	uint8_t *buf = malloc(need);
	if (buf == NULL) {
		return;
	}
	const size_t n = conv_core_save_describe_cache(g.core, buf, need);
	FILE *f = fopen(g.cache_path, "wb");
	if (f != NULL) {
		(void)fwrite(buf, 1, n, f);
		fclose(f);
	}
	free(buf);
}

//! The registry / manifests changed: re-enumerate and reconcile (watch thread).
static void
rescan(void)
{
	struct xrt_conversion_plugin_entry *fresh = calloc(XRT_CONV_LOADER_MAX_ENTRIES, sizeof(*fresh));
	if (fresh == NULL) {
		return;
	}
	const int n = target_conversion_plugin_enumerate(fresh, XRT_CONV_LOADER_MAX_ENTRIES);
	for (int i = 0; i < n; i++) {
		struct xrt_conversion_plugin_entry *e = &fresh[i];
		if (target_conv_is_policy_subkey(e->id)) {
			continue;
		}
		// #2051: HKLM `Enabled` with the HKCU override applied — the value
		// `conv enable|disable` writes lives in HKCU, which the loader never
		// reads, so comparing the loader's value alone saw no change.
		e->enabled = effective_enabled(e);
		os_mutex_lock(&g.mtx);
		const int k = find_entry_locked(e->id);
		const bool known_enabled = k >= 0 ? g.entries[k].enabled : false;
		os_mutex_unlock(&g.mtx);
		if (k >= 0) {
			if (known_enabled != e->enabled) {
				U_LOG_W("[conv] provider %s: %s by policy — applies to the next stream / job create.",
				        e->id, e->enabled ? "enabled" : "disabled");
				conv_core_set_provider_enabled(g.core, e->id, e->enabled);
				os_mutex_lock(&g.mtx);
				g.entries[k].enabled = e->enabled;
				os_mutex_unlock(&g.mtx);
			}
			conv_core_provider_changed(g.core, e->id);
			continue;
		}
		os_mutex_lock(&g.mtx);
		const bool room = g.entry_count < XRT_CONV_LOADER_MAX_ENTRIES;
		os_mutex_unlock(&g.mtx);
		if (!room) {
			continue;
		}
		add_entry(e); // a newly installed provider (UNAVAILABLE -> READY without a restart)
		os_mutex_lock(&g.mtx);
		g.entries[g.entry_count++] = *e;
		os_mutex_unlock(&g.mtx);
	}
	// A row gone from the registry: the in-service provider stays loaded
	// (spec §4.1: leaked until restart) but is no longer selectable.
	os_mutex_lock(&g.mtx);
	for (int k = 0; k < g.entry_count; k++) {
		bool present = false;
		for (int i = 0; i < n; i++) {
			if (strcmp(fresh[i].id, g.entries[k].id) == 0) {
				present = true;
				break;
			}
		}
		if (!present && g.entries[k].enabled) {
			g.entries[k].enabled = false;
			os_mutex_unlock(&g.mtx);
			conv_core_set_provider_enabled(g.core, g.entries[k].id, false);
			os_mutex_lock(&g.mtx);
		}
	}
	os_mutex_unlock(&g.mtx);
	free(fresh);
	// The built-in adapter has no registry row; its per-user Enabled is a
	// policy override like any other (dp_lift_enabled_by_policy).
	if (g.dp_lift_registered) {
		conv_core_set_provider_enabled(g.core, CONV_DP_LIFT_ID, dp_lift_enabled_by_policy());
	}
	cache_save();
}

/*!
 * #2051 (the #2025 item): hand the re-read policy to the core. conv_core
 * COPIES `PreferredProvider\<task>` at create ("the copies are
 * authoritative") and had no setter, so the watch thread's
 * target_conv_policy_read refreshed g.policy — which only the status rows
 * print — while selection kept ranking from the create-time copy.
 */
static void
push_policy_to_core(void)
{
	const char *pref[XRT_CONV_TASK_RESERVED_FIRST] = {0};
	bool research = false;
	os_mutex_lock(&g.mtx);
	for (uint32_t t = 0; t < XRT_CONV_TASK_RESERVED_FIRST && t < TARGET_CONV_POLICY_TASKS; t++) {
		pref[t] = g.policy.preferred[t][0] != '\0' ? g.policy.preferred[t] : NULL;
	}
	research = g.policy.allow_research_only;
	// g.policy is only ever rewritten on this (watch) thread, so the
	// pointers stay valid across the unlock.
	os_mutex_unlock(&g.mtx);
	conv_core_set_policy(g.core, pref, research);
}

static void
redescribe_all(void)
{
	os_mutex_lock(&g.mtx);
	const int n = g.entry_count;
	char ids[XRT_CONV_LOADER_MAX_ENTRIES][64];
	for (int i = 0; i < n; i++) {
		memcpy(ids[i], g.entries[i].id, sizeof(ids[i]));
	}
	os_mutex_unlock(&g.mtx);
	for (int i = 0; i < n; i++) {
		conv_core_provider_changed(g.core, ids[i]);
	}
}

/*!
 * service_component_watch_fn for the conversion class (#2051). Replaces this
 * file's private Windows-only RegNotifyChangeKeyValue thread: the service's
 * one watch thread calls here (debounced) on an HKLM / HKCU
 * `ConversionProviders` change, on a POSIX manifest change, on the DIAG
 * reselect call, and on a provider's `notify_changed`.
 *
 * A policy change is applied in three steps the old thread did only one of:
 * re-read the policy (it did this), hand `PreferredProvider` /
 * `AllowResearchOnly` to the core (new — the core kept its create-time copy),
 * then rescan with the HKCU `Enabled` override applied to every row (new —
 * the loader's HKLM value alone never changes on `conv disable`). The next
 * stream / job create then selects under the new policy; a live stream never
 * moves (spec §3.4 rule 5).
 */
static void
on_policy_watch(void *ud, uint32_t class_id, uint32_t reason)
{
	(void)ud;
	(void)class_id;
	if (g.core == NULL) {
		return;
	}
	if (reason == SERVICE_COMPONENT_WATCH_PROVIDER_NOTIFY) {
		redescribe_all();
		return;
	}
	os_mutex_lock(&g.mtx);
	target_conv_policy_read(&g.policy);
	os_mutex_unlock(&g.mtx);
	push_policy_to_core();
	rescan();
	U_LOG_W("[conv] policy re-read (%s): preferences and Enabled apply to the next stream / job create.",
	        reason == SERVICE_COMPONENT_WATCH_POKED ? "reselect requested" : "registration or policy changed");
}

#ifdef XRT_OS_WINDOWS
//! PrewarmOn: the registry string, else interest on mains / first-create on battery.
static uint32_t
read_prewarm_on(void)
{
	char v[32] = {0};
	DWORD sz = sizeof(v);
	if (RegGetValueA(HKEY_CURRENT_USER, TARGET_CONV_REG_ROOT, "PrewarmOn", RRF_RT_REG_SZ, NULL, v, &sz) !=
	    ERROR_SUCCESS) {
		sz = sizeof(v);
		if (RegGetValueA(HKEY_LOCAL_MACHINE, TARGET_CONV_REG_ROOT, "PrewarmOn",
		                 RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, v, &sz) != ERROR_SUCCESS) {
			v[0] = '\0';
		}
	}
	if (_stricmp(v, "interest") == 0) {
		return CONV_PREWARM_INTEREST;
	}
	if (_stricmp(v, "first-create") == 0) {
		return CONV_PREWARM_FIRST_CREATE;
	}
	if (_stricmp(v, "start") == 0) {
		return CONV_PREWARM_START;
	}
	SYSTEM_POWER_STATUS ps;
	if (GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0) {
		return CONV_PREWARM_FIRST_CREATE; // on battery
	}
	return CONV_PREWARM_INTEREST;
}
#else
static uint32_t
read_prewarm_on(void)
{
	return CONV_PREWARM_INTEREST; // desktop default; POSIX root policy is a later phase
}
#endif


/*
 *
 * Lifetime.
 *
 */

void
service_conversion_start(void)
{
	if (g.started) {
		return;
	}
	if (os_mutex_init(&g.mtx) != 0 || os_mutex_init(&g.alloc_mtx) != 0 || os_cond_init(&g.alloc_cond) != 0) {
		U_LOG_E("[conv] could not create the conversion core's service locks");
		return;
	}
	g.started = true;
	init_host();
	target_conv_policy_read(&g.policy);

	g.entries = calloc(XRT_CONV_LOADER_MAX_ENTRIES, sizeof(*g.entries));
	g.reg_scratch = calloc(1, sizeof(*g.reg_scratch));
	g.status_scratch = calloc(1, sizeof(*g.status_scratch));
	if (g.entries == NULL || g.reg_scratch == NULL || g.status_scratch == NULL) {
		U_LOG_E("[conv] out of memory building the conversion core");
		return;
	}

	struct conv_core_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.alloc.alloc = tramp_alloc;
	cfg.alloc.free = tramp_free;
	cfg.alloc.upload = tramp_upload;
	// cfg.viewsyn / cfg.readback: installed once the D3D11 device exists
	// (on_gpu, conv_core_set_gpu). The S7 observer needs no device.
	cfg.create_convergence_observer = conv_observer_autoconv_factory;
	cfg.observer_ud = NULL;
	cfg.eyes_max_age_ns = 2ull * 1000ull * 1000ull * 1000ull; // older eyes: the nominal viewer
	{
		// DXR_LIFT_RECENTER=off / 0: viewpoint recentering forced OFF (as the lift thread read it).
		const char *rc = getenv("DXR_LIFT_RECENTER");
		cfg.recenter_off =
		    rc != NULL && (strcmp(rc, "0") == 0 || strcmp(rc, "off") == 0 || strcmp(rc, "OFF") == 0);
	}
	cfg.on_event = on_event;
	for (uint32_t t = 0; t < XRT_CONV_TASK_RESERVED_FIRST && t < TARGET_CONV_POLICY_TASKS; t++) {
		cfg.preferred[t] = g.policy.preferred[t][0] != '\0' ? g.policy.preferred[t] : NULL;
	}
	cfg.allow_research_only = g.policy.allow_research_only;
	cfg.prewarm_on = read_prewarm_on();
	cfg.rear_cap_m = -1.0f;
	if (conv_core_create(&cfg, &g.core) != XRT_SUCCESS) {
		U_LOG_E("[conv] conv_core_create failed; lift stays on the legacy path");
		g.core = NULL;
		return;
	}

	char cfg_path[512];
	if (service_config_path(cfg_path, sizeof(cfg_path))) {
		char *sep = strrchr(cfg_path, '/');
#ifdef XRT_OS_WINDOWS
		char *bsep = strrchr(cfg_path, '\\');
		if (bsep != NULL && (sep == NULL || bsep > sep)) {
			sep = bsep;
		}
#endif
		if (sep != NULL) {
			sep[1] = '\0';
			(void)snprintf(g.cache_path, sizeof(g.cache_path), "%s%s", cfg_path, CACHE_FILENAME);
			cache_load();
		}
	}

	const int n = target_conversion_plugin_enumerate(g.entries, XRT_CONV_LOADER_MAX_ENTRIES);
	g.entry_count = 0;
	for (int i = 0; i < n; i++) {
		if (target_conv_is_policy_subkey(g.entries[i].id)) {
			continue;
		}
		add_entry(&g.entries[i]);
		if (g.entry_count != i) {
			g.entries[g.entry_count] = g.entries[i];
		}
		g.entry_count++;
	}
	cache_save();
	U_LOG_W("[conv] conversion core up: %d provider(s) registered, PrewarmOn=%u", g.entry_count, cfg.prewarm_on);

	if (ipc_server_conv_create(g.core, &g.sc) == XRT_SUCCESS) {
		ipc_server_conv_set_gpu_hook(g.sc, on_gpu, NULL);
		ipc_server_set_conv(g.sc);
	}
	static const struct ipc_server_conv_status_source source = {
	    .fill = conv_status_fill,
	    .generation = conv_status_generation,
	    .userdata = NULL,
	};
	ipc_server_set_conv_status_source(&source);

	// #2051: registration / policy changes and provider notifies arrive from
	// the service's one component watch thread (started by main.c after this).
	service_component_watch_subscribe(TARGET_COMPONENT_CLASS_CONVERSION, on_policy_watch, NULL);
}

void
service_conversion_stop(void)
{
	if (!g.started) {
		return;
	}
	ipc_server_set_conv_status_source(NULL);
	ipc_server_set_conv(NULL);
	// main.c stops the watch thread before this; clearing the subscription
	// keeps a late poke from reaching a core that is going away.
	service_component_watch_subscribe(TARGET_COMPONENT_CLASS_CONVERSION, NULL, NULL);
	cache_save();
	ipc_server_conv_destroy(&g.sc);
	// Providers die through their hosts; their binaries stay mapped for the
	// process lifetime (spec §4.1).
	conv_core_destroy(&g.core);
	free(g.entries);
	free(g.reg_scratch);
	free(g.status_scratch);
	g.entries = NULL;
	g.reg_scratch = NULL;
	g.status_scratch = NULL;
	g.entry_count = 0;
	g.dp_lift_iface = NULL;
	g.dp_lift_inst = NULL; // destroyed by its host with the core
	g.dp_lift_registered = false;
	os_cond_destroy(&g.alloc_cond);
	os_mutex_destroy(&g.alloc_mtx);
	os_mutex_destroy(&g.mtx);
	g.started = false;
}

struct conv_core *
service_conversion_core(void)
{
	return g.core;
}
