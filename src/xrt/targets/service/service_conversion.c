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
#include "service_spawn.h"

#include "xrt/xrt_config_os.h"

#include "conversion/conv_core.h"
#include "conversion/conv_host_paths.h"
#include "conversion/conv_model_store.h"
#include "conversion/conv_observer_autoconv.h"
#include "conversion/conv_provider_legacy_dp.h"
#include "conversion/conv_registry.h"

#include "server/ipc_server.h"
#include "server/ipc_server_conv.h"
#ifndef XRT_OS_WINDOWS
#include "server/ipc_server_conv_cpu.h"
#endif

#include "target_component_loader.h"
#include "target_conv_status.h"
#include "target_conversion_plugin_loader.h"

#include "os/os_process.h"
#include "os/os_threading.h"
#include "util/u_camera_consent.h"
#include "util/u_file_logging.h"
#include "util/u_logging.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef XRT_OS_WINDOWS
#include <strings.h> // strcasecmp
#endif

#ifdef XRT_OS_WINDOWS
#include "xrt/xrt_windows.h"
#endif

#ifdef CONV_HAVE_HOST_PROXY
#include "conversion/conv_host_proxy.h"
#endif
#ifdef SERVICE_HAVE_CONV_HOST_LANE_D3D11
#include "d3d11_service/d3d11_conv_host_lane.h"
#endif

//! The conversion-host satellite (ADR-053 D5), installed beside the service.
#define CONV_HOST_EXE "displayxr-conversion-host" SERVICE_SPAWN_EXE_SUFFIX

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

	/*!
	 * The legacy DP-lift adapter (#2003). Registered only once the GPU side
	 * attached AND the active display driver offers lift (#2769, see
	 * dp_lift_evaluate). dp_lift_mtx serialises that decision and guards the
	 * backend copy and the two "offer" fields; dp_lift_registered is read
	 * without it (it only ever goes false -> true within a service lifetime).
	 */
	struct os_mutex dp_lift_mtx;
	struct conv_dp_lift_backend dp_lift_backend; //!< the attached backend (snapshot NULL = none)
	bool dp_lift_offer_known;                    //!< dp_lift_offered holds an answer (logs on edges only)
	bool dp_lift_offered;                        //!< the last answer
	const struct xrt_conversion_plugin_iface *dp_lift_iface;
	struct xrt_conversion_plugin_instance *dp_lift_inst;
	bool dp_lift_registered;
	bool dp_lift_enabled; //!< the Enabled last applied to it (a live policy change compares)

	//! The describe cache as read at start (satellites restore from it as they register).
	uint8_t *cache_buf;
	size_t cache_len;

	//! Satellite proxies (owned by the core) and the ingest adapter they run on (#2034).
	struct conv_host *sat_hosts[XRT_CONV_LOADER_MAX_ENTRIES];
	char sat_ids[XRT_CONV_LOADER_MAX_ENTRIES][64]; //!< provider id of sat_hosts[i]
	uint32_t sat_count;
	uint64_t adapter_luid;

	char cache_path[600];

	//! Parallel to entries: the row is gone from the registry / manifests (an
	//! uninstall while the service runs). Watch thread only.
	bool entry_gone[XRT_CONV_LOADER_MAX_ENTRIES];

	/*! Registration changes seen while the provider's maintenance mutex was
	 *  held (discovery spec §3.8.1): the ids wait here, and the wait thread
	 *  pokes the component watch when a mutex is released. An installer writes
	 *  nothing after it releases the mutex, so no registry wake would come. */
	struct os_mutex maint_mtx; //!< guards maint_ids / maint_count / maint_stop
	struct os_cond maint_cond;
	struct os_thread maint_thread;
	bool maint_inited;
	bool maint_started;
	bool maint_stop;
	char maint_ids[XRT_CONV_LOADER_MAX_ENTRIES][64];
	uint32_t maint_count;
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
	os_mutex_lock(&g.mtx);
	const bool enabled = target_conv_policy_enabled(&g.policy, CONV_DP_LIFT_ID, true);
	os_mutex_unlock(&g.mtx);
	return enabled;
}

/*!
 * The ingest adapter is known: every satellite is (re)spawned on it, and gets
 * its D3D11 lane there (opened only now, so it can never sit on another adapter).
 */
static void
satellites_set_adapter(uint64_t luid)
{
	os_mutex_lock(&g.mtx);
	g.adapter_luid = luid;
	struct conv_host *hosts[XRT_CONV_LOADER_MAX_ENTRIES];
	const uint32_t n = g.sat_count;
	memcpy(hosts, g.sat_hosts, sizeof(hosts[0]) * n);
	os_mutex_unlock(&g.mtx);
	for (uint32_t i = 0; i < n; i++) {
#ifdef CONV_HAVE_HOST_PROXY
		const struct conv_host_proxy_gpu_ops *gp = NULL;
#ifdef SERVICE_HAVE_CONV_HOST_LANE_D3D11
		struct conv_host_proxy_gpu_ops gpu;
		memset(&gpu, 0, sizeof(gpu));
		if (d3d11_conv_host_lane_create(luid, &gpu)) {
			gp = &gpu;
		}
#endif
		conv_host_proxy_set_adapter(hosts[i], luid, gp);
#else
		(void)hosts;
#endif
	}
}

/*!
 * #2769: list the built-in `dp-lift` row only when the active display driver
 * offers lift.
 *
 * The test is "the active display plug-in exports the lift-only factory"
 * (xrt_plugin_iface::create_dp_d3d11_lift), read from the backend's snapshot:
 * two pointer reads, no vendor call. It is the same test the headless status
 * uses (target_conv_status.c), so `conv list` says the same thing with and
 * without a service. It deliberately does NOT create a display processor on a
 * NULL window to look at its lift slots: that is the cost this rule removes
 * for a driver with no converter. A plug-in that fills the lift slots exports
 * that factory (the two were introduced together); one that filled the slots
 * on its ordinary factory only, without the lift-only factory, is not listed.
 *
 * The answer can change within a service lifetime: the display re-probe may
 * adopt another plug-in (one installed after the service started, or the
 * vendor plug-in taking over from the fallback). So this runs when the GPU
 * side attaches, on every display re-probe and on a DisplayProcessors
 * registry change.
 *  - not offered -> offered: the row is registered then.
 *  - offered -> not offered: the conversion core has no "remove a provider"
 *    (streams and the describe cache hold provider indices), so a row that
 *    was registered stays for this service lifetime and answers ABSENT, as
 *    before this rule; it is gone at the next service start. Rare: it takes
 *    swapping the display driver under a running service.
 *
 * One WARN per change of the answer, never per call (the fallback re-probe
 * timer calls this every 10 s). Any thread; takes dp_lift_mtx.
 */
static void
dp_lift_evaluate(const char *why)
{
	if (g.core == NULL) {
		return;
	}
	os_mutex_lock(&g.dp_lift_mtx);
	if (g.dp_lift_backend.snapshot == NULL) {
		os_mutex_unlock(&g.dp_lift_mtx);
		return; // no D3D11 GPU side (yet): nothing could serve the row
	}
	struct conv_dp_lift_snapshot snap;
	g.dp_lift_backend.snapshot(g.dp_lift_backend.ud, &snap);
	const bool offered = snap.lift_only_factory;
	const bool changed = !g.dp_lift_offer_known || offered != g.dp_lift_offered;
	g.dp_lift_offer_known = true;
	g.dp_lift_offered = offered;

	if (!offered) {
		if (changed && g.dp_lift_registered) {
			U_LOG_W(
			    "[conv] dp-lift: the active display driver no longer offers lift (%s); the row stays "
			    "listed until the service restarts and answers absent",
			    why);
		} else if (changed) {
			U_LOG_W(
			    "[conv] dp-lift not listed: the active display driver offers no lift (no lift-only "
			    "factory; %s). Conversion comes from conversion providers only",
			    why);
		}
		os_mutex_unlock(&g.dp_lift_mtx);
		return;
	}
	if (g.dp_lift_registered) {
		os_mutex_unlock(&g.dp_lift_mtx);
		if (changed) {
			// Offered again after it went away: retry the row at once.
			U_LOG_W("[conv] dp-lift: the active display driver offers lift (%s): retrying the row", why);
			conv_core_provider_changed(g.core, CONV_DP_LIFT_ID);
		}
		return;
	}
	if (!conv_provider_legacy_dp_create(&g.dp_lift_backend, &g.dp_lift_iface, &g.dp_lift_inst)) {
		os_mutex_unlock(&g.dp_lift_mtx);
		return;
	}
	struct conv_provider_desc d;
	memset(&d, 0, sizeof(d));
	d.id = CONV_DP_LIFT_ID;
	d.probe_order = CONV_DP_LIFT_PROBE_ORDER;
	d.hosting = CONV_HOSTING_IN_SERVICE; // always: the display plug-in never leaves the service
	d.enabled = dp_lift_enabled_by_policy();
	g.dp_lift_enabled = d.enabled;
	d.flags = CONV_PROV_LICENCE_EXEMPT | CONV_PROV_LEGACY_UNITS;
	d.iface = g.dp_lift_iface;
	d.inst = g.dp_lift_inst;
	if (conv_core_add_provider(g.core, &d) >= 0) {
		g.dp_lift_registered = true;
		U_LOG_W(
		    "[conv] dp-lift registered: the display plug-in's lift slots (lift-only factory present; %s), "
		    "ProbeOrder %u, in-service%s",
		    why, CONV_DP_LIFT_PROBE_ORDER, d.enabled ? "" : " (disabled by policy)");
	}
	os_mutex_unlock(&g.dp_lift_mtx);
}

//! ipc_server_conv_display_plugins_hook_fn: the display re-probe ran (#2769).
static void
on_display_plugins_reprobed(void *ud)
{
	(void)ud;
	dp_lift_evaluate("display re-probe");
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
		os_mutex_lock(&g.dp_lift_mtx);
		memset(&g.dp_lift_backend, 0, sizeof(g.dp_lift_backend));
		g.dp_lift_offer_known = false;
		os_mutex_unlock(&g.dp_lift_mtx);
		service_conversion_set_gpu_alloc(NULL);
		return;
	}
	service_conversion_set_gpu_alloc(&gpu->alloc);
	if (gpu->adapter_luid != 0) {
		satellites_set_adapter(gpu->adapter_luid);
	}
	if (gpu->dp_lift.snapshot == NULL) {
		return;
	}
	os_mutex_lock(&g.dp_lift_mtx);
	g.dp_lift_backend = gpu->dp_lift;
	os_mutex_unlock(&g.dp_lift_mtx);
	if (g.dp_lift_registered) {
		// A re-created service system: the row exists, hand it the new backend.
		conv_provider_legacy_dp_attach(g.dp_lift_inst, &gpu->dp_lift);
		conv_core_provider_changed(g.core, CONV_DP_LIFT_ID);
		dp_lift_evaluate("GPU side re-attached"); // logs only if the answer changed
		return;
	}
	dp_lift_evaluate("GPU side attached");
}


/*
 *
 * Events and the status snapshot source.
 *
 */

static void
on_event(void *ud, uint64_t client_id, const struct xrt_conv_wire_event *ev)
{
	// Called with core locks held: queue only. Spec-4 task / provider events
	// go to the connections that opted in (ipc_server_conv's own leaf lock);
	// the status snapshot's `conversion` counter follows the core's
	// generations through conv_status_generation below.
	(void)ud;
	ipc_server_conv_on_event(ipc_server_get_conv(), client_id, ev);
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
	// N3c: the one options contract's store (shared with the headless pass and
	// every other class): the service's environment, then the per-user
	// overlay (both read live, so a `conv set` reaches the next re-describe),
	// then the row's Settings\* / manifest "settings" as enumerated.
	const char *row_value = NULL;
	for (uint32_t i = 0; i < g_host_slot[slot].setting_count && i < XRT_CONV_LOADER_MAX_SETTINGS; i++) {
#ifdef XRT_OS_WINDOWS
		if (_stricmp(g_host_slot[slot].settings[i].key, key) == 0) {
#else
		if (strcasecmp(g_host_slot[slot].settings[i].key, key) == 0) {
#endif
			row_value = g_host_slot[slot].settings[i].value;
			break;
		}
	}
	return target_component_get_setting_ex(target_conversion_plugin_component_class(), g_host_slot[slot].id, key,
	                                       row_value, buf, buf_size, NULL);
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

/*! ADR-057 §2: the user's per-provider consent, from the per-user consent
 *  store (the same one ADR-043's camera consumers use), keyed by the slot's id. */
static bool
host_get_consent_slot(uint32_t slot, uint32_t kind)
{
	return kind == XRT_CONV_CONSENT_OFFBOX && g_host_slot[slot].used &&
	       u_offbox_consent_granted(g_host_slot[slot].id);
}

#define HOST_GET_CONSENT(i)                                                                                            \
	static bool host_get_consent_##i(uint32_t kind)                                                                \
	{                                                                                                              \
		return host_get_consent_slot(i, kind);                                                                 \
	}
HOST_GET_CONSENT(0)
HOST_GET_CONSENT(1)
HOST_GET_CONSENT(2)
HOST_GET_CONSENT(3)
HOST_GET_CONSENT(4)
HOST_GET_CONSENT(5)
HOST_GET_CONSENT(6)
HOST_GET_CONSENT(7)
HOST_GET_CONSENT(8)
HOST_GET_CONSENT(9)
HOST_GET_CONSENT(10)
HOST_GET_CONSENT(11)
HOST_GET_CONSENT(12)
HOST_GET_CONSENT(13)
HOST_GET_CONSENT(14)
HOST_GET_CONSENT(15)

/*! #2194: get_path for the slot's provider (conv_host_paths.h): its own
 *  scratch directory, the process store's root, the service log's directory.
 *  The satellite host answers the same paths for its provider. */
static bool
host_get_path_slot(uint32_t slot, uint32_t kind, char *buf, uint32_t buf_size)
{
	if (slot >= HOST_SLOTS || !g_host_slot[slot].used) {
		if (buf != NULL && buf_size > 0) {
			buf[0] = '\0';
		}
		return false;
	}
	return conv_host_get_path(g_host_slot[slot].id, kind, u_file_logging_get_path(), buf, buf_size);
}

#define HOST_GET_PATH(i)                                                                                               \
	static bool host_get_path_##i(uint32_t kind, char *buf, uint32_t buf_size)                                     \
	{                                                                                                              \
		return host_get_path_slot(i, kind, buf, buf_size);                                                     \
	}
HOST_GET_PATH(0)
HOST_GET_PATH(1)
HOST_GET_PATH(2)
HOST_GET_PATH(3)
HOST_GET_PATH(4)
HOST_GET_PATH(5)
HOST_GET_PATH(6)
HOST_GET_PATH(7)
HOST_GET_PATH(8)
HOST_GET_PATH(9)
HOST_GET_PATH(10)
HOST_GET_PATH(11)
HOST_GET_PATH(12)
HOST_GET_PATH(13)
HOST_GET_PATH(14)
HOST_GET_PATH(15)

static bool (*const k_host_get_path[HOST_SLOTS])(uint32_t, char *, uint32_t) = {
    host_get_path_0,  host_get_path_1,  host_get_path_2,  host_get_path_3,  host_get_path_4,  host_get_path_5,
    host_get_path_6,  host_get_path_7,  host_get_path_8,  host_get_path_9,  host_get_path_10, host_get_path_11,
    host_get_path_12, host_get_path_13, host_get_path_14, host_get_path_15,
};

static bool (*const k_host_get_consent[HOST_SLOTS])(uint32_t) = {
    host_get_consent_0,  host_get_consent_1,  host_get_consent_2,  host_get_consent_3,
    host_get_consent_4,  host_get_consent_5,  host_get_consent_6,  host_get_consent_7,
    host_get_consent_8,  host_get_consent_9,  host_get_consent_10, host_get_consent_11,
    host_get_consent_12, host_get_consent_13, host_get_consent_14, host_get_consent_15,
};

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
	g_hosts[slot].get_consent = k_host_get_consent[slot];
	g_hosts[slot].get_path = k_host_get_path[slot];
	return &g_hosts[slot];
}

static void
init_host(void)
{
	g_host = *target_conversion_plugin_default_host();
	g_host.notify_changed = host_notify_changed;
	// ADR-053 P4 (#2108): the runtime-owned model store. The ops carry no
	// provider context, so one process store serves every in-service provider.
	g_host.model_resolve = conv_store_host_model_resolve;
	g_host.model_request = conv_store_host_model_request;
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

/*!
 * Where @p e runs (roadmap §3.2, §3.4; discovery spec §3.3 `Hosting`):
 * `DXR_CONV_<ID>_HOSTING` from the SERVICE's environment, then the registered
 * value; `auto` = satellite. The built-in dp-lift adapter never comes here.
 */
static uint32_t
resolve_hosting(const struct xrt_conversion_plugin_entry *e, bool *out_from_env)
{
	char env_name[128];
	const char *env_value = NULL;
	if (target_conversion_plugin_hosting_env_name(e->id, env_name, sizeof(env_name))) {
		env_value = getenv(env_name);
	}
	return target_conversion_plugin_resolve_hosting(e->hosting, env_value, out_from_env);
}

#ifdef CONV_HAVE_HOST_PROXY
/*!
 * conv_host_proxy_config::registration_current (#2793, discovery spec §3.8.1).
 * A satellite's supervisor is about to spawn its host and asks whether the
 * row the service holds is still the registered one. An uninstaller removes
 * the binary and the row under the provider's maintenance mutex; the
 * supervisor polls that mutex itself, so without this it started one host on
 * the binary that was leaving, between the release and the watch thread's
 * rescan (the host exited 11, "before it was up").
 *
 * Reads the registrations afresh and compares what rescan compares (`Version`
 * and `Binary`, and the row being there at all), so the spawn and the rescan
 * decide on the same row. Not current: the proxy takes the spawn back and
 * holds further ones, and the watch is poked so rescan re-reads the row now
 * and releases the hold (a rebind, or a row that reads the same again) or
 * keeps it (a removal). A provider the service holds no row for yet (its
 * proxy is created before its row is stored) has nothing to compare: current.
 * Supervisor thread; no proxy lock held; takes g.mtx only to read the held
 * row. One registry read per host start.
 */
static bool
sat_registration_current(void *ud, const char *id, const char *binary)
{
	(void)ud;
	(void)binary; // the held row's Binary is the proxy's: rebind_entry writes both
	struct xrt_conversion_plugin_entry *fresh = calloc(XRT_CONV_LOADER_MAX_ENTRIES, sizeof(*fresh));
	if (fresh == NULL) {
		return true; // cannot tell: as before this check existed
	}
	const int n = target_conversion_plugin_enumerate(fresh, XRT_CONV_LOADER_MAX_ENTRIES);
	const struct xrt_conversion_plugin_entry *row = NULL;
	for (int i = 0; i < n; i++) {
		if (strcmp(fresh[i].id, id) == 0) {
			row = &fresh[i];
			break;
		}
	}
	os_mutex_lock(&g.mtx);
	const int k = find_entry_locked(id);
	const bool gone = k >= 0 && g.entry_gone[k];
	const bool current = k < 0 || (row != NULL && !gone && strcmp(g.entries[k].version, row->version) == 0 &&
	                               strcmp(g.entries[k].binary_path, row->binary_path) == 0);
	os_mutex_unlock(&g.mtx);
	if (!current) {
		U_LOG_W(
		    "[conv] provider %s: its registration %s since the service last read it; no host is started "
		    "until the service has re-read it",
		    id,
		    row == NULL ? "was removed"
		    : gone      ? "is back"
		                : "changed");
		(void)service_component_watch_poke(TARGET_COMPONENT_CLASS_CONVERSION, SERVICE_COMPONENT_WATCH_POKED);
	}
	free(fresh);
	return current;
}
#endif

/*!
 * Build the satellite proxy for @p e: the provider DLL is NOT loaded in the
 * service; the proxy spawns `displayxr-conversion-host` (lazily) and answers
 * the describe from it. False, with @p why, when this install cannot host it.
 */
static bool
satellite_host_create(const struct xrt_conversion_plugin_entry *e, struct conv_host **out, char *why, size_t why_size)
{
	char host_exe[600];
	if (!service_spawn_sibling_path(CONV_HOST_EXE, host_exe, sizeof(host_exe)) ||
	    !service_spawn_file_exists(host_exe)) {
		(void)snprintf(why, why_size, "no %s beside the service", CONV_HOST_EXE);
		return false;
	}
#ifdef CONV_HAVE_HOST_PROXY
	struct conv_host_proxy_config cfg;
	conv_host_proxy_config_defaults(&cfg);
	cfg.provider_id = e->id;
	cfg.provider_binary = e->binary_path;
	cfg.host_exe = host_exe;
	cfg.registration_current = sat_registration_current; // #2793
	os_mutex_lock(&g.mtx);
	cfg.adapter_luid = g.adapter_luid; // 0 until the GPU side attached (satellites_set_adapter)
	os_mutex_unlock(&g.mtx);
	// The host writes its own log (DisplayXR_conversion-host_<id>.<pid>_<ts>.log).
#ifdef SERVICE_HAVE_CONV_HOST_LANE_D3D11
	// The GPU lane: NT-handle textures + the host's shared fence, observed on
	// the CPU (the fence rule) -- on the ingest adapter only, so not before it
	// is known. Without it the satellite serves CPU images only.
	struct conv_host_proxy_gpu_ops gpu;
	memset(&gpu, 0, sizeof(gpu));
	if (cfg.adapter_luid != 0 && d3d11_conv_host_lane_create(cfg.adapter_luid, &gpu)) {
		cfg.gpu = &gpu;
	}
#endif
	const xrt_result_t r = conv_host_proxy_create(&cfg, out);
#ifdef SERVICE_HAVE_CONV_HOST_LANE_D3D11
	if (r != XRT_SUCCESS && cfg.gpu != NULL) {
		gpu.destroy(gpu.ud);
	}
#endif
	if (r != XRT_SUCCESS) {
		(void)snprintf(why, why_size, "the host proxy could not be created (%d)", (int)r);
		return false;
	}
	return true;
#else
	(void)e;
	(void)out;
	(void)snprintf(why, why_size, "this build has no conversion-host proxy");
	return false;
#endif
}

//! Whether a satellite proxy is registered for provider @p id (caller holds g.mtx).
static bool
sat_registered_locked(const char *id)
{
	for (uint32_t j = 0; j < g.sat_count; j++) {
		if (strcmp(g.sat_ids[j], id) == 0) {
			return true;
		}
	}
	return false;
}

//! Negotiate + probe @p e (in-service) or build its satellite proxy, and add
//! it to the core (no g.mtx: may call a provider).
static void
add_entry(struct xrt_conversion_plugin_entry *e)
{
	if (target_conv_is_policy_subkey(e->id)) {
		return;
	}
	e->enabled = effective_enabled(e);

	struct conv_provider_desc d;
	memset(&d, 0, sizeof(d));
	d.id = e->id;
	d.probe_order = e->probe_order;
	d.supersedes = e->supersedes;
	d.binary_path = e->binary_path;
	struct stat sb;
	if (e->binary_path[0] != '\0' && stat(e->binary_path, &sb) == 0) {
		d.binary_size = (uint64_t)sb.st_size;
		d.binary_mtime = (uint64_t)sb.st_mtime;
	}

	bool from_env = false;
	const uint32_t want = resolve_hosting(e, &from_env);
	const bool loadable = e->status == XRT_CONV_LOAD_NOT_LOADED || e->status == XRT_CONV_LOAD_NEGOTIATED;
	if (want == XRT_CONV_HOSTING_SATELLITE && e->enabled && loadable) {
		char why[192] = {0};
		struct conv_host *host = NULL;
		if (e->status == XRT_CONV_LOAD_NOT_LOADED && satellite_host_create(e, &host, why, sizeof(why))) {
			// The host process negotiates (ABI gate: exit 10), probes and
			// describes; the service never maps the provider binary.
			d.hosting = CONV_HOSTING_SATELLITE;
			d.enabled = true;
			d.host = host;
			(void)snprintf(e->reason, sizeof(e->reason), "hosted in %s", CONV_HOST_EXE);
			U_LOG_W("[conv] provider %s: Hosting=satellite%s (%s)", e->id, from_env ? " (from the environment)" : "",
			        CONV_HOST_EXE);
			if (conv_core_add_provider(g.core, &d) < 0) {
				return; // the core destroyed the proxy
			}
			os_mutex_lock(&g.mtx);
			if (g.sat_count < XRT_CONV_LOADER_MAX_ENTRIES) {
				(void)snprintf(g.sat_ids[g.sat_count], sizeof(g.sat_ids[0]), "%s", e->id);
				g.sat_hosts[g.sat_count++] = host;
			}
			os_mutex_unlock(&g.mtx);
			// The describe cache answers until the first warm / create spawns the
			// host (roadmap §3.4 "Spawn"); restore this provider's rows now that it
			// is registered.
			if (g.cache_buf != NULL) {
				(void)conv_core_load_describe_cache(g.core, g.cache_buf, g.cache_len);
			}
			if (!conv_core_provider_described(g.core, e->id)) {
#ifdef CONV_HAVE_HOST_PROXY
				U_LOG_W("[conv] provider %s: no cached describe (first run or a new binary): spawning its "
				        "host once to describe it; the idle exit reclaims it",
				        e->id);
				conv_host_proxy_request_describe(host);
#endif
			}
			return;
		}
		if (e->status == XRT_CONV_LOAD_NEGOTIATED) {
			(void)snprintf(why, sizeof(why), "it is already loaded in the service");
		}
		U_LOG_W("[conv] provider %s: Hosting=satellite%s, but %s: hosting it in the service", e->id,
		        from_env ? " (from the environment)" : "", why);
	}
	d.hosting = CONV_HOSTING_IN_SERVICE;

	if (e->status == XRT_CONV_LOAD_NOT_LOADED) {
		(void)target_conversion_plugin_negotiate_one(e, host_for_entry(e));
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
		// Kept: providers registered after this point (satellites, every
		// provider at start) restore from it as they register.
		free(g.cache_buf);
		g.cache_buf = buf;
		g.cache_len = n;
		return;
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

/*
 *
 * Registration changes under a running service (discovery spec §3.8.1).
 *
 */

//! Is an installer holding `Global\DisplayXR-ConvMaintenance-<id>`? POSIX: never.
static bool
maintenance_held(const char *id)
{
#ifdef XRT_OS_WINDOWS
	char name[160];
	(void)snprintf(name, sizeof(name), "Global\\DisplayXR-ConvMaintenance-%s", id);
	return os_named_mutex_held(name);
#else
	(void)id;
	return false;
#endif
}

#define MAINT_POLL_NS (1000ull * 1000000ull)

//! Waits for the maintenance mutexes of the deferred ids; pokes the watch on a release.
static void *
maint_wait_main(void *ptr)
{
	(void)ptr;
	os_mutex_lock(&g.maint_mtx);
	while (!g.maint_stop) {
		if (g.maint_count == 0) {
			os_cond_wait(&g.maint_cond, &g.maint_mtx);
			continue;
		}
		char ids[XRT_CONV_LOADER_MAX_ENTRIES][64];
		const uint32_t n = g.maint_count;
		memcpy(ids, g.maint_ids, sizeof(ids));
		os_mutex_unlock(&g.maint_mtx);
		bool released[XRT_CONV_LOADER_MAX_ENTRIES] = {0};
		bool any = false;
		for (uint32_t i = 0; i < n; i++) {
			released[i] = !maintenance_held(ids[i]);
			any = any || released[i];
		}
		os_mutex_lock(&g.maint_mtx);
		if (any) {
			// Drop the released ids; a rescan that still finds one held re-adds it.
			uint32_t kept = 0;
			for (uint32_t i = 0; i < g.maint_count; i++) {
				bool drop = false;
				for (uint32_t j = 0; j < n; j++) {
					drop = drop || (released[j] && strcmp(ids[j], g.maint_ids[i]) == 0);
				}
				if (!drop) {
					if (kept != i) {
						memcpy(g.maint_ids[kept], g.maint_ids[i], sizeof(g.maint_ids[0]));
					}
					kept++;
				}
			}
			g.maint_count = kept;
			os_mutex_unlock(&g.maint_mtx);
			(void)service_component_watch_poke(TARGET_COMPONENT_CLASS_CONVERSION,
			                                   SERVICE_COMPONENT_WATCH_POKED);
			os_mutex_lock(&g.maint_mtx);
			continue;
		}
		(void)os_cond_wait_timeout_ns(&g.maint_cond, &g.maint_mtx, MAINT_POLL_NS);
	}
	os_mutex_unlock(&g.maint_mtx);
	return NULL;
}

//! Provider @p id's registration changed under a held maintenance mutex: act on its release (watch thread).
static void
maint_defer(const char *id)
{
	if (!g.maint_inited) {
		return; // no wait thread: the next registry change or `conv reselect` applies it
	}
	bool added = false;
	os_mutex_lock(&g.maint_mtx);
	bool have = false;
	for (uint32_t i = 0; i < g.maint_count; i++) {
		have = have || strcmp(g.maint_ids[i], id) == 0;
	}
	if (!have && g.maint_count < XRT_CONV_LOADER_MAX_ENTRIES) {
		(void)snprintf(g.maint_ids[g.maint_count++], sizeof(g.maint_ids[0]), "%s", id);
		added = true;
	}
	os_cond_signal(&g.maint_cond);
	os_mutex_unlock(&g.maint_mtx);
	if (!g.maint_started) {
		// Started at the first deferral (watch thread only), joined at stop.
		g.maint_started = os_thread_start(&g.maint_thread, maint_wait_main, NULL) == 0;
	}
	if (added) {
		U_LOG_W(
		    "[conv] provider %s: its registration changed while an update holds its maintenance mutex; "
		    "the service follows it when the mutex is released",
		    id);
	}
}

/*!
 * Row @p k's registration changed (`Binary` and / or `Version`), or the row is
 * back after a removal: re-read the whole row (watch thread, no lock held).
 *
 * A provider the service does not have mapped (satellite-hosted, or never
 * loaded) is rebound: the held row is replaced by @p e, the core's registry
 * takes the new binary key / ProbeOrder / Supersedes, and its satellite proxy
 * spawns the new binary from now on (a host still running the old one is
 * closed for update). The provenance of the new binary is then classified
 * where it always is: by the host process's load gate at the next spawn, and
 * by the status reader from the binary the snapshot now reports.
 *
 * A provider loaded IN the service keeps its mapped binary until the service
 * restarts (spec §4.1): only the row's Version / Binary are recorded, as
 * before, and the log says a restart is needed.
 */
static void
rebind_entry(int k, const struct xrt_conversion_plugin_entry *e, struct conv_host *sat, bool was_gone)
{
	os_mutex_lock(&g.mtx);
	const bool mapped = g.entries[k].status == XRT_CONV_LOAD_NEGOTIATED;
	const bool binary_changed = strcmp(g.entries[k].binary_path, e->binary_path) != 0;
	if (mapped) {
		memcpy(g.entries[k].version, e->version, sizeof(e->version));
		memcpy(g.entries[k].binary_path, e->binary_path, sizeof(e->binary_path));
	} else {
		// The whole row. `enabled` stays the value last applied: the
		// enable / disable reconciliation in rescan compares against it.
		const bool applied_enabled = g.entries[k].enabled;
		g.entries[k] = *e;
		g.entries[k].enabled = applied_enabled;
		if (sat != NULL) {
			(void)snprintf(g.entries[k].reason, sizeof(g.entries[k].reason), "hosted in %s", CONV_HOST_EXE);
		}
	}
	g.entry_gone[k] = false;
	os_mutex_unlock(&g.mtx);

	if (mapped) {
		if (binary_changed) {
			U_LOG_W(
			    "[conv] provider %s: its registration now names %s, but it is loaded in the service: "
			    "the mapped binary serves until the service restarts",
			    e->id, e->binary_path);
		}
	} else {
		struct conv_provider_desc d;
		memset(&d, 0, sizeof(d));
		d.id = e->id;
		d.probe_order = e->probe_order;
		d.supersedes = e->supersedes;
		d.binary_path = e->binary_path;
		struct stat sb;
		if (e->binary_path[0] != '\0' && stat(e->binary_path, &sb) == 0) {
			d.binary_size = (uint64_t)sb.st_size;
			d.binary_mtime = (uint64_t)sb.st_mtime;
		}
		const bool key_changed = conv_core_rebind_provider(g.core, &d);
#ifdef CONV_HAVE_HOST_PROXY
		if (sat != NULL) {
			(void)conv_host_proxy_rebind(sat, e->binary_path, key_changed);
		}
#else
		(void)sat;
#endif
		if (key_changed || was_gone) {
			U_LOG_W("[conv] provider %s: registration %s (version %s, binary %s); no service restart",
			        e->id, was_gone ? "back" : "rebound", e->version, e->binary_path);
		}
	}
	if (was_gone) {
		conv_core_set_provider_registered(g.core, e->id, true);
	}
}

/*!
 * #2029: re-read the §3.1 policy and hand it to the running core:
 * PreferredProvider / AllowResearchOnly (the next selection), and the
 * per-user Enabled of the built-in dp-lift adapter (rows of registered
 * providers are reconciled by rescan). Watch thread; no core lock held.
 */
//! The registry / manifests changed: re-enumerate and reconcile (watch thread).
static void
rescan(void)
{
	struct xrt_conversion_plugin_entry *fresh = calloc(XRT_CONV_LOADER_MAX_ENTRIES, sizeof(*fresh));
	if (fresh == NULL) {
		return;
	}
	const int n = target_conversion_plugin_enumerate(fresh, XRT_CONV_LOADER_MAX_ENTRIES);
	// New packs (a provider installed or updated) and the current download /
	// research policy reach the model store before any provider re-warms.
	os_mutex_lock(&g.mtx);
	const struct target_conv_policy policy = g.policy;
	os_mutex_unlock(&g.mtx);
	target_conv_store_configure(fresh, n, &policy);
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
		// #2029: the per-user Enabled (HKCU) counts as much as HKLM's.
		(void)target_conv_policy_apply_override(&g.policy, e);
		const int k = find_entry_locked(e->id);
		const bool known_enabled = k >= 0 ? g.entries[k].enabled : false;
		const bool known_loaded = k >= 0 && g.entries[k].status == XRT_CONV_LOAD_NEGOTIATED;
		// Roadmap §3.6 respawn rule (i): the provider's own row changed (an
		// installer writes `Version` last) -- not just any key under the root.
		struct conv_registration_change ch = {0};
		ch.known = k >= 0;
		ch.gone = k >= 0 && g.entry_gone[k];
		ch.present = true;
		ch.row_changed = k >= 0 && (strcmp(g.entries[k].version, e->version) != 0 ||
		                            strcmp(g.entries[k].binary_path, e->binary_path) != 0);
		struct conv_host *sat = NULL;
		for (uint32_t j = 0; k >= 0 && j < g.sat_count; j++) {
			if (strcmp(g.sat_ids[j], e->id) == 0) {
				sat = g.sat_hosts[j];
			}
		}
		os_mutex_unlock(&g.mtx);
		// Spec §3.8.1: a changed row is acted on only with its maintenance
		// mutex free -- mid-install, `Binary` may name a file that is not
		// there yet. Nothing of the held row is updated until then, so the
		// change is seen again at the release (maint_wait_main's poke).
		ch.maintenance_held = (!ch.known || ch.gone || ch.row_changed) && maintenance_held(e->id);
		const enum conv_registration_action action = conv_registration_action(&ch);
		if (action == CONV_REGISTRATION_DEFER) {
			maint_defer(e->id);
			continue;
		}
		const bool reregistered = action == CONV_REGISTRATION_REBIND || action == CONV_REGISTRATION_RESTORE;
		if (reregistered) {
			rebind_entry(k, e, sat, action == CONV_REGISTRATION_RESTORE);
		}
#ifdef CONV_HAVE_HOST_PROXY
		if (sat != NULL) {
			// #2793: the row is present and was re-read with its maintenance
			// mutex free (anything else was deferred above): a spawn the
			// proxy held on it may go. A rebind released it already; this
			// covers a row that reads the same again.
			conv_host_proxy_set_registration_pending(sat, false);
			bool clear_gate = reregistered;
			if (!clear_gate) {
				// #2153 (found live): a satellite whose host failed to LOAD --
				// its probe declined (`Settings\ENABLE` unset, a licence gate),
				// negotiate refused -- sits FAILED "until a configuration
				// change", and the change that fixes it is `conv set <id> KEY
				// value` or `conv enable <id>`: neither touches Version or
				// Binary, so rule (i) above never fired and the provider could
				// not be enabled without a service restart. Any edit under the
				// conversion store clears a load failure: the retry is one
				// short spawn (a declining host exits within milliseconds), and
				// the core's REGISTRY_CHANGED re-warm below is the demand that
				// spawns it. A crash loop keeps the own-row rule.
				struct conv_supervisor_status st;
				long pid = 0;
				conv_host_proxy_get_status(sat, &st, &pid);
				clear_gate =
				    st.state == CONV_SUP_FAILED && st.failure_reason != XRT_CONV_FAIL_CRASH_LOOP;
			}
			if (clear_gate) {
				conv_host_proxy_config_changed(sat);
			}
		}
#else
		(void)sat;
#endif
		if (k >= 0) {
			// Spec §3.8.1: a re-registered row the service never managed to
			// load (untrusted, an ABI reject, a failed negotiate) is loaded
			// again from its new binary, like a row enabled after start.
			const bool reload = reregistered && e->enabled && !known_loaded && sat == NULL;
			if (known_enabled != e->enabled || reload) {
				// A satellite-hosted provider is never NEGOTIATED in the
				// service (its host process negotiates), so "loaded" for it
				// means "its proxy is registered with the core". Without this
				// a `conv disable` + `conv enable` of a satellite provider
				// re-ran add_entry (a second proxy the core rejects) and then
				// re-enabled it with `false`: it stayed DISABLED until a
				// service restart.
				bool hosted = sat != NULL;
				if (e->enabled && !known_loaded && !hosted) {
					// Disabled at start, so never loaded: load it now
					// (negotiate + probe + register, or build its satellite
					// proxy), no restart.
					add_entry(e);
					os_mutex_lock(&g.mtx);
					g.entries[k] = *e;
					hosted = sat_registered_locked(e->id);
					os_mutex_unlock(&g.mtx);
				} else {
					os_mutex_lock(&g.mtx);
					g.entries[k].enabled = e->enabled;
					os_mutex_unlock(&g.mtx);
				}
				conv_core_set_provider_enabled(
				    g.core, e->id,
				    e->enabled && (known_loaded || hosted || e->status == XRT_CONV_LOAD_NEGOTIATED));
				U_LOG_W("[conv] provider %s %s by a registry change (no restart)", e->id,
				        known_enabled == e->enabled ? "loaded again"
				        : e->enabled                ? "enabled"
				                                    : "disabled");
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
	// A row gone from the registry / manifests (an uninstall). The core cannot
	// remove a provider -- an in-service one stays loaded (spec §4.1: leaked
	// until restart), a satellite proxy stays with its slot -- so it is marked
	// gone: no capability rows, never selected, not listed (spec §3.8.1).
	// g.entries is only rewritten on this (watch) thread.
	for (int k = 0; k < g.entry_count; k++) {
		bool present = false;
		for (int i = 0; i < n; i++) {
			if (strcmp(fresh[i].id, g.entries[k].id) == 0) {
				present = true;
				break;
			}
		}
		struct conv_registration_change ch = {0};
		ch.known = true;
		ch.gone = g.entry_gone[k];
		ch.present = present;
		ch.maintenance_held = !present && !ch.gone && maintenance_held(g.entries[k].id);
		const enum conv_registration_action action = conv_registration_action(&ch);
		if (action == CONV_REGISTRATION_DEFER) {
			maint_defer(g.entries[k].id);
			continue;
		}
		if (action != CONV_REGISTRATION_REMOVE) {
			continue;
		}
		os_mutex_lock(&g.mtx);
		g.entry_gone[k] = true;
		g.entries[k].enabled = false;
		struct conv_host *gone_sat = NULL;
		for (uint32_t j = 0; j < g.sat_count; j++) {
			if (strcmp(g.sat_ids[j], g.entries[k].id) == 0) {
				gone_sat = g.sat_hosts[j];
			}
		}
		os_mutex_unlock(&g.mtx);
#ifdef CONV_HAVE_HOST_PROXY
		if (gone_sat != NULL) {
			// #2793: no host is started for a provider that is not registered
			// (its binary is gone or leaving). Released when the row is back.
			conv_host_proxy_set_registration_pending(gone_sat, true);
		}
#else
		(void)gone_sat;
#endif
		conv_core_set_provider_enabled(g.core, g.entries[k].id, false);
		conv_core_set_provider_registered(g.core, g.entries[k].id, false);
		U_LOG_W(
		    "[conv] provider %s: its registration is gone (uninstalled): no longer listed or selected; "
		    "no service restart",
		    g.entries[k].id);
	}
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
	// Discovery spec §3.2 (#2029): DXR_CONV=0 -- or its alias DXR_LIFT=0 -- in
	// the SERVICE's own environment means no conversion service at all: no
	// core, no providers, no dp-lift row, no route. The v3 lift calls then
	// answer modes 0 / UNAVAILABLE. The pre-core lift thread reads only
	// DXR_LIFT, so DXR_CONV=0 sets that alias for this process too.
	const char *ks = target_conv_kill_switch();
	if (ks != NULL) {
#ifdef XRT_OS_WINDOWS
		// (The lift thread is the D3D11 service's: Windows only.)
		if (strcmp(ks, "DXR_CONV") == 0) {
			const char *lift = getenv("DXR_LIFT");
			if (lift == NULL || strcmp(lift, "0") != 0) {
				(void)_putenv_s("DXR_LIFT", "0");
			}
		}
#endif
		U_LOG_W(
		    "[conv] %s=0: no conversion service (kill switch, discovery spec §3.2); lift reports UNAVAILABLE",
		    ks);
		return;
	}
	if (os_mutex_init(&g.mtx) != 0 || os_mutex_init(&g.alloc_mtx) != 0 || os_cond_init(&g.alloc_cond) != 0 ||
	    os_mutex_init(&g.dp_lift_mtx) != 0) {
		U_LOG_E("[conv] could not create the conversion core's service locks");
		return;
	}
	g.started = true;
	g.maint_inited = false;
	if (os_mutex_init(&g.maint_mtx) == 0) {
		if (os_cond_init(&g.maint_cond) == 0 && os_thread_init(&g.maint_thread) == 0) {
			g.maint_inited = true;
		} else {
			os_mutex_destroy(&g.maint_mtx);
		}
	}
	g.maint_started = false;
	g.maint_stop = false;
	g.maint_count = 0;
	memset(g.entry_gone, 0, sizeof(g.entry_gone));
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
		(void)target_conv_policy_apply_override(&g.policy, &g.entries[i]); // #2029: HKCU Enabled
		add_entry(&g.entries[i]);
		if (g.entry_count != i) {
			g.entries[g.entry_count] = g.entries[i];
		}
		g.entry_count++;
	}
	cache_save();
	target_conv_store_configure(g.entries, g.entry_count, &g.policy);
	U_LOG_W("[conv] conversion core up: %d provider(s) registered, PrewarmOn=%u", g.entry_count, cfg.prewarm_on);

	if (ipc_server_conv_create(g.core, &g.sc) == XRT_SUCCESS) {
		ipc_server_conv_set_gpu_hook(g.sc, on_gpu, NULL);
		ipc_server_conv_set_display_plugins_hook(g.sc, on_display_plugins_reprobed, NULL);
		ipc_server_set_conv(g.sc);
#ifndef XRT_OS_WINDOWS
		// No D3D11 device here to install a GPU glue: the CPU glue engages
		// the core route for spec-4 streams with CPU images (roadmap §4.2,
		// CPU-first v1). The images stay the service allocator's CPU memory.
		struct ipc_server_conv_io_ops cpu_io;
		if (ipc_server_conv_cpu_io_get(&cpu_io)) {
			ipc_server_conv_set_io(g.sc, &cpu_io);
			U_LOG_W("[conv] CPU I/O glue installed: spec-4 lift streams take and give CPU images");
		} else {
			U_LOG_W("[conv] no CPU image transport on this platform: no lift stream is reachable");
		}
#endif
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
	if (g.maint_inited) {
		// The watch thread (the only caller of maint_defer) is already stopped.
		os_mutex_lock(&g.maint_mtx);
		g.maint_stop = true;
		os_cond_signal(&g.maint_cond);
		os_mutex_unlock(&g.maint_mtx);
		if (g.maint_started) {
			os_thread_join(&g.maint_thread);
		}
		os_thread_destroy(&g.maint_thread);
		os_cond_destroy(&g.maint_cond);
		os_mutex_destroy(&g.maint_mtx);
		g.maint_inited = false;
		g.maint_started = false;
		g.maint_count = 0;
	}
	cache_save();
	ipc_server_conv_destroy(&g.sc);
	// Providers die through their hosts; their binaries stay mapped for the
	// process lifetime (spec §4.1).
	conv_core_destroy(&g.core);
	free(g.entries);
	free(g.reg_scratch);
	free(g.status_scratch);
	free(g.cache_buf);
	g.cache_buf = NULL;
	g.cache_len = 0;
	g.sat_count = 0; // the proxies died with the core
	g.entries = NULL;
	g.reg_scratch = NULL;
	g.status_scratch = NULL;
	g.entry_count = 0;
	g.dp_lift_iface = NULL;
	g.dp_lift_inst = NULL; // destroyed by its host with the core
	g.dp_lift_registered = false;
	memset(&g.dp_lift_backend, 0, sizeof(g.dp_lift_backend));
	g.dp_lift_offer_known = false;
	g.dp_lift_offered = false;
	os_mutex_destroy(&g.dp_lift_mtx);
	os_cond_destroy(&g.alloc_cond);
	os_mutex_destroy(&g.alloc_mtx);
	os_mutex_destroy(&g.mtx);
	g.started = false;
}

void
service_conversion_display_plugins_changed(void)
{
	if (!g.started || g.core == NULL) {
		return;
	}
	if (!g.dp_lift_registered) {
		// #2769: not listed so far. A plug-in that offers lift may have been
		// installed; the display re-probe this change also triggers checks
		// again once the plug-in is actually adopted.
		dp_lift_evaluate("display plug-in registration changed");
		return;
	}
	U_LOG_I("[conv] display plug-in registration changed: retrying dp-lift");
	conv_core_provider_changed(g.core, CONV_DP_LIFT_ID);
}

struct conv_core *
service_conversion_core(void)
{
	return g.core;
}
