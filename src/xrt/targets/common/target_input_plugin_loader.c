// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Registry / manifest-driven loader for input-provider plug-ins.
 *
 * The input-provider class loader (ADR-034), written as ADR-054 N2 intends:
 * discovery, the #952 / #328 / #434 load path and the ADR-020 ABI gate are the
 * shared component loader's (`target_component_loader.h`), driven by this
 * class's table row below. What is left here is what only this class knows:
 * the host iface a provider retains (#1380), what `probe()` means, the
 * keep-every-claimer rule (ADR-034 Amendment 3), the scan tally the self-test
 * reads, and the `ForceQwerty` gate.
 *
 * v1 has no refresh path: provider devices are created exactly once at system
 * build, so the DP loader's #342 mid-install refresh problem (factory pointers
 * baked into long-lived compositor state) does not arise here.
 *
 * Issue #823.
 *
 * @ingroup target_common
 */

#include "target_input_plugin_loader.h"
#include "target_component_loader.h"

#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_config_os.h"

#include "util/u_logging.h"

#include "os/os_threading.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * The class row (ADR-054 §2).
 *
 */

/*!
 * `Policy` holds root policy (`Policy\Lock`, honoured by the shared
 * per-user overlay), never a provider row. Case-insensitive, like the
 * conversion class's reserved names.
 */
static bool
input_is_reserved_subkey(const char *name)
{
	static const char policy[] = "policy";
	for (size_t i = 0;; i++) {
		const char a = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + ('a' - 'A') : name[i]);
		if (a != policy[i]) {
			return false;
		}
		if (a == '\0') {
			return true;
		}
	}
}

static const struct target_component_class g_input_class = {
    .name = "input provider",
    .log_tag = "input plugin loader",
    .entrypoint = XRT_INPUT_PLUGIN_ENTRYPOINT_NAME,
    .abi_current = XRT_INPUT_PLUGIN_API_VERSION_CURRENT,
    .registry_root = "Software\\DisplayXR\\InputProviders",
    .manifest_kind = TARGET_PLUGIN_MANIFEST_INPUT,
    // Android ships no input providers in v1 — the `libdxr*` convention is
    // DP + conversion only for now; the qwerty fallback / shell-side input
    // path is unaffected.
    .android_prefix = NULL,
    .is_reserved_subkey = input_is_reserved_subkey,
    .read_extra = NULL,
};


/*
 *
 * Shared state.
 *
 */

static int g_input_load_attempted = 0;

/*!
 * Outcome tally of the one discovery scan, for
 * @ref target_input_plugin_get_scan_result. "Nobody claimed the system"
 * is a normal state — every provider may legitimately decline (no
 * hardware of its type, or an opt-in like sim-input's `DXR_SIM_INPUT`
 * left unset) — so the self-test must not read it as a fault. What IS a
 * fault is a provider that could not even be dispatched: a missing entry
 * point, a failed load, or an ABI-major mismatch. Counting the two
 * separately is what lets `displayxr-cli selftest` tell them apart.
 */
static int g_input_scan_declined = 0;
static int g_input_scan_failed = 0;

/*!
 * Every provider that loaded, ABI-passed and probed successfully, in
 * priority order (ProbeOrder ascending; ties keep discovery order).
 *
 * ADR-034 Amendment 3: the scan no longer stops at the first success. A
 * box may have several modalities registered at once (a desk hand
 * tracker AND a pair of 6DOF controllers), and which one should drive
 * the hands is a *presence* question answered continuously by the
 * arbiter — not a load-order question answered once. Keeping every
 * candidate resident is what lets the roles fall from a higher-priority
 * modality to a lower one instead of straight to qwerty.
 *
 * Mirrors the DP loader's display-claim source set
 * (`target_plugin_loader.c`, `g_display_sources`), which likewise
 * consults every registered plug-in rather than only the active one.
 */
static struct input_provider_slot
{
	const struct xrt_input_plugin_iface *iface;
	struct xrt_input_plugin_instance *inst;
	uint32_t probe_order;
} g_input_providers[TARGET_COMPONENT_MAX_ENTRIES];

static int g_input_provider_count = 0;

/*!
 * Record a provider that loaded, ABI-passed and probed. Called by the
 * discovery pass in ProbeOrder, so the array ends up sorted.
 */
static void
input_note_provider(const struct xrt_input_plugin_iface *iface, struct xrt_input_plugin_instance *inst, uint32_t order)
{
	if (iface == NULL || g_input_provider_count >= TARGET_COMPONENT_MAX_ENTRIES) {
		return;
	}
	struct input_provider_slot *slot = &g_input_providers[g_input_provider_count++];
	slot->iface = iface;
	slot->inst = inst;
	slot->probe_order = order;
}



/*
 *
 * Host iface + display-geometry cache (#1380).
 *
 */

/*!
 * THE host iface, one per process, filled once and never moved.
 *
 * A provider is explicitly allowed to retain the `struct
 * xrt_input_plugin_host_iface *` it is handed at
 * `xrtInputPluginNegotiate` for the life of the process, so the storage
 * must outlive the negotiate call. It used to be a stack local in each
 * platform's `input_try_load_one` — a provider that kept the pointer
 * (the first external navigation provider does) was reading dead stack the moment negotiate returned.
 *
 * Every provider in the process sees the SAME iface; nothing in it is
 * per-provider, which is why sharing one is correct rather than merely
 * convenient.
 */
static struct xrt_input_plugin_host_iface g_input_host;
static bool g_input_host_filled = false;

/*!
 * The runtime-owned geometry cache behind
 * @ref xrt_input_plugin_host_iface::get_display_geometry.
 *
 * Written exactly once, by the system builder, from the display plug-in's
 * display info — after the DP head exists and BEFORE any provider's
 * `create_devices` runs. Read afterwards, potentially from a provider's
 * own thread, hence the mutex: the write is a one-shot publish, not a
 * live feed.
 */
static struct xrt_input_host_display_geometry g_input_geometry;
static bool g_input_geometry_ready = false;
static struct os_mutex g_input_geometry_mutex;
static bool g_input_geometry_mutex_ready = false;

/*!
 * Bring the geometry mutex up. Called from the two entry points that both
 * run long before any provider thread exists — the loader's negotiate path
 * and the builder's publish — so the guard itself never races.
 */
static void
input_geometry_lock_init_once(void)
{
	if (!g_input_geometry_mutex_ready) {
		g_input_geometry_mutex_ready = os_mutex_init(&g_input_geometry_mutex) == 0;
	}
}

static xrt_result_t
input_host_get_display_geometry(struct xrt_input_host_display_geometry *inout)
{
	// A caller that cannot even hold the leading `struct_size` has
	// nothing we could fill in, prefix rules or not. `xrt_result` has no
	// generic invalid-argument code; INPUT_UNSUPPORTED is the closest
	// existing one in this domain and is deliberately NOT the code a
	// too-early call gets, which is the distinction that matters here.
	if (inout == NULL || inout->struct_size < sizeof(uint32_t)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	if (g_input_geometry_mutex_ready) {
		os_mutex_lock(&g_input_geometry_mutex);
	}
	const bool ready = g_input_geometry_ready;
	struct xrt_input_host_display_geometry snapshot = g_input_geometry;
	if (g_input_geometry_mutex_ready) {
		os_mutex_unlock(&g_input_geometry_mutex);
	}

	if (!ready) {
		// Contract: leave the caller's struct untouched so a retry
		// later is the whole of the recovery.
		return XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY;
	}

	// Prefix semantics, the display-info convention: a provider built
	// against an older (shorter) header gets the fields it knows and
	// keeps its own `struct_size`.
	size_t copy = inout->struct_size;
	if (copy > sizeof(snapshot)) {
		copy = sizeof(snapshot);
	}
	const uint32_t caller_size = inout->struct_size;
	memcpy(inout, &snapshot, copy);
	inout->struct_size = caller_size;

	return XRT_SUCCESS;
}

/*!
 * Fill @ref g_input_host once and hand it out. Called from
 * `input_try_load_one` immediately before `negotiate`.
 */
static struct xrt_input_plugin_host_iface *
input_host_iface(void)
{
	if (!g_input_host_filled) {
		input_geometry_lock_init_once();
		memset(&g_input_host, 0, sizeof(g_input_host));
		g_input_host.struct_size = (uint32_t)sizeof(struct xrt_input_plugin_host_iface);
		g_input_host.host_api_version = XRT_INPUT_PLUGIN_API_VERSION_CURRENT;
		g_input_host.get_display_geometry = input_host_get_display_geometry;
		g_input_host_filled = true;
	}
	return &g_input_host;
}

void
target_input_plugin_set_display_geometry(const struct xrt_input_host_display_geometry *geometry)
{
	if (geometry == NULL) {
		return;
	}

	input_geometry_lock_init_once();

	if (g_input_geometry_mutex_ready) {
		os_mutex_lock(&g_input_geometry_mutex);
	}
	g_input_geometry = *geometry;
	g_input_geometry.struct_size = (uint32_t)sizeof(struct xrt_input_host_display_geometry);
	g_input_geometry_ready = true;
	if (g_input_geometry_mutex_ready) {
		os_mutex_unlock(&g_input_geometry_mutex);
	}

	// One-off init fact, once per process: a bug report needs to show
	// whether providers ever got geometry at all.
	U_LOG_W(
	    "input plugin loader: display geometry published to providers: %.4f x %.4f m, nominal viewer "
	    "(%.4f, %.4f, %.4f) m.",
	    (double)g_input_geometry.display_width_m, (double)g_input_geometry.display_height_m,
	    (double)g_input_geometry.nominal_viewer_x_m, (double)g_input_geometry.nominal_viewer_y_m,
	    (double)g_input_geometry.nominal_viewer_z_m);
}

const struct xrt_input_plugin_host_iface *
target_input_plugin_get_host_iface(void)
{
	return input_host_iface();
}



/*
 *
 * Discovery (every platform — the shared loader does the platform part).
 *
 */

/*!
 * Load + negotiate + ABI-check + probe one registered provider. Returns the
 * iface (writing the instance to *out_inst) on success, NULL on any failure
 * (the binary is unloaded again). Successful loads intentionally leak the
 * module for the process lifetime: the devices' vtables live in it.
 *
 * @p out_declined is true when the provider was dispatched correctly and
 * said "not my hardware" — negotiate or probe returned
 * XRT_ERROR_PROBER_NOT_SUPPORTED — as opposed to a load / ABI fault.
 */
static const struct xrt_input_plugin_iface *
input_try_load_one(struct target_component_entry *e,
                   struct xrt_input_plugin_instance **out_inst,
                   bool *out_declined)
{
	*out_inst = NULL;
	*out_declined = false;

	void *module = NULL;
	void *fn = NULL;
	if (target_component_load_binary(&g_input_class, e, &module, &fn, NULL, NULL, 0) != TARGET_COMPONENT_LOAD_OK) {
		return NULL;
	}
	xrt_input_plugin_negotiate_fn_t negotiate = (xrt_input_plugin_negotiate_fn_t)fn;

	// Process-lifetime host storage: providers may retain this pointer
	// (#1380). Never a stack local.
	struct xrt_input_plugin_host_iface *host = input_host_iface();

	struct xrt_input_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = negotiate(XRT_INPUT_PLUGIN_API_VERSION_CURRENT, host, &iface, &plugin_version);
	const enum target_component_abi_verdict verdict =
	    target_component_abi_gate(&g_input_class, e->id, xret, iface, plugin_version, NULL, 0);
	if (verdict != TARGET_COMPONENT_ABI_OK) {
		*out_declined = verdict == TARGET_COMPONENT_ABI_DECLINED;
		target_component_unload(module);
		return NULL;
	}

	if (iface->probe != NULL) {
		xret = iface->probe(out_inst);
		if (xret == XRT_ERROR_PROBER_NOT_SUPPORTED) {
			U_LOG_I("input plugin loader:   %s: probe declined (no matching hardware).", e->id);
			*out_declined = true;
			target_component_unload(module);
			return NULL;
		}
		if (xret != XRT_SUCCESS) {
			U_LOG_W("input plugin loader:   %s: probe returned %d — skipping.", e->id, (int)xret);
			target_component_unload(module);
			return NULL;
		}
	}

	U_LOG_W(
	    "input plugin loader: active input provider: id=%s name='%s' vendor='%s' version='%s' "
	    "plugin_api=%u probe_order=%u path=%s",
	    iface->id ? iface->id : e->id, iface->display_name ? iface->display_name : e->display_name,
	    iface->vendor ? iface->vendor : e->vendor, e->version, plugin_version, e->probe_order, e->binary_path);

	/* Module handle intentionally leaked, same lifetime rule as the DP
	 * loader — the devices' vtables live in the provider binary. */
	return iface;
}

/*!
 * Where a row's `enabled` came from, for `displayxr-cli input list` (N3a):
 * "user" when the shared per-user overlay applied, "machine" / "manifest"
 * when the platform's own store disabled it, else "default". The shared row
 * reader records the value, not its presence, so an explicit enable in
 * HKLM / the manifest reads as "default".
 */
static const char *
input_enabled_source(const struct target_component_entry *e)
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

/*!
 * N3a (ADR-054 verb `prefer`) — class selection policy, which the shared
 * loader deliberately never does: move the row whose id is @p preferred to
 * the front of the sorted @p entries, keeping the rest in order, so the scan
 * tries it first and notes it at arbitration priority 0 (the arbiter ranks
 * by that value, ties in insertion order). Returns true when a row matched.
 */
static bool
input_hoist_preferred(struct target_component_entry *entries, int n, const char *preferred)
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
				return true; // matched; leave the order alone rather than fail
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

static void
input_discover_all(void)
{
	struct target_component_entry *entries =
	    (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(*entries));
	if (entries == NULL) {
		return;
	}
	int n = target_component_enumerate(&g_input_class, entries, TARGET_COMPONENT_MAX_ENTRIES);
	if (n == 0) {
		U_LOG_I("input plugin loader: no registered input providers.");
		free(entries);
		return;
	}
	// N3a: per-user Enabled (HKCU over HKLM; Windows only) — `input enable|disable`.
	(void)target_component_apply_user_overlay(&g_input_class, entries, (size_t)n);

	// #1466: which provider outranks the other on a tie is an alphabetical
	// accident, and the vendor whose provider lost is the only person who
	// can fix it — so say so, once, with both ids.
	for (int i = 1; i < n; i++) {
		if (entries[i - 1].probe_order == entries[i].probe_order &&
		    (entries[i].flags & TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE) != 0) {
			U_LOG_W(
			    "input plugin loader: '%s' and '%s' share ProbeOrder %u — tie broken by id; "
			    "give coexisting providers distinct values.",
			    entries[i - 1].id, entries[i].id, entries[i].probe_order);
		}
	}

	// N3a: the PreferredPlugin override (`input use <id>`) is tried first and
	// ranked at priority 0; unregistered → ignored, disabled → skipped below.
	char preferred[64] = {0};
	const bool have_pref = target_input_plugin_get_preferred(preferred, sizeof(preferred));
	if (have_pref && !input_hoist_preferred(entries, n, preferred)) {
		U_LOG_W("input plugin loader: PreferredPlugin '%s' is not a registered provider — ignored.", preferred);
	}

	U_LOG_I("input plugin loader: %d registered provider(s); attempting in ProbeOrder ascending.", n);
	for (int i = 0; i < n; i++) {
		struct target_component_entry *e = &entries[i];
		const bool is_pref = have_pref && strcmp(e->id, preferred) == 0;
		if ((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0) {
			U_LOG_W("input plugin loader:   [%d/%d] %s: %s — skipping.", i + 1, n, e->source, e->reason);
			continue;
		}
		if (!e->enabled) {
			U_LOG_I("input plugin loader:   [%d/%d] %s disabled (Enabled=0, %s)%s — skipping.", i + 1, n,
			        e->id, input_enabled_source(e), is_pref ? " [preferred]" : "");
			continue;
		}
		U_LOG_I("input plugin loader:   [%d/%d] %s (ProbeOrder=%u%s, %s)", i + 1, n, e->id, e->probe_order,
		        is_pref ? ", preferred" : "", e->binary_path);
		struct xrt_input_plugin_instance *inst = NULL;
		bool declined = false;
		const struct xrt_input_plugin_iface *iface = input_try_load_one(e, &inst, &declined);
		if (iface != NULL) {
			// Keep going: every claiming provider is a live
			// candidate the arbiter can fall back to.
			input_note_provider(iface, inst, is_pref ? 0 : e->probe_order);
			continue;
		}
		if (declined) {
			g_input_scan_declined++;
		} else {
			g_input_scan_failed++;
		}
	}
	free(entries);

	if (g_input_provider_count == 0) {
		U_LOG_I(
		    "input plugin loader: no registered provider claimed the system (%d declined, %d failed to load) "
		    "— qwerty keeps the hand roles.",
		    g_input_scan_declined, g_input_scan_failed);
	}
}

int
target_input_plugin_enumerate(struct target_input_plugin_desc *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}

	struct target_component_entry *entries =
	    (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(*entries));
	if (entries == NULL) {
		return 0;
	}
	int n = target_component_enumerate(&g_input_class, entries, TARGET_COMPONENT_MAX_ENTRIES);
	(void)target_component_apply_user_overlay(&g_input_class, entries, (size_t)n); // N3a: what the scan sees

	int count = 0;
	for (int i = 0; i < n && count < max; i++) {
		const struct target_component_entry *e = &entries[i];
		if ((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0) {
			// A row the loader could never try is not a choice to list.
			continue;
		}
		struct target_input_plugin_desc *d = &out[count++];
		memset(d, 0, sizeof(*d));
		snprintf(d->id, sizeof(d->id), "%s", e->id);
		snprintf(d->display_name, sizeof(d->display_name), "%s", e->display_name);
		snprintf(d->vendor, sizeof(d->vendor), "%s", e->vendor);
		snprintf(d->version, sizeof(d->version), "%s", e->version);
		snprintf(d->binary_path, sizeof(d->binary_path), "%s", e->binary_path);
		d->probe_order = e->probe_order;
		d->enabled = e->enabled;
		snprintf(d->enabled_source, sizeof(d->enabled_source), "%s", input_enabled_source(e));
	}
	free(entries);
	return count;
}


/*
 *
 * The ForceQwerty gate (class-specific; the one platform split left here).
 *
 */

#ifdef XRT_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

bool
target_input_plugin_get_force_qwerty(void)
{
	DWORD value = 0;
	DWORD bytes = sizeof(value);
	LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\Input", L"ForceQwerty", RRF_RT_REG_DWORD,
	                          NULL, &value, &bytes);
	return rc == ERROR_SUCCESS && value != 0;
}

/*
 * N3a: `PreferredPlugin` — HKCU first (per user, `input use <id>`), then the
 * HKLM 64-bit view (`--machine`). Same root value name as the DP loader's
 * §2.1 override, under the InputProviders root. Class selection policy, so
 * class code — the shared loader never selects.
 */
bool
target_input_plugin_get_preferred(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';

	wchar_t wbuf[64];
	DWORD wbuf_bytes = sizeof(wbuf);
	LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, L"Software\\DisplayXR\\InputProviders", L"PreferredPlugin",
	                          RRF_RT_REG_SZ, NULL, wbuf, &wbuf_bytes);
	if (rc != ERROR_SUCCESS || wbuf[0] == L'\0') {
		wbuf_bytes = sizeof(wbuf);
		rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\InputProviders", L"PreferredPlugin",
		                  RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, wbuf, &wbuf_bytes);
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
target_input_plugin_get_force_qwerty(void)
{
	return false;
}

bool
target_input_plugin_get_preferred(char *out, size_t cap)
{
	if (out != NULL && cap > 0) {
		out[0] = '\0';
	}
	return false; // no providers, no override on Android in v1
}

#else /* POSIX: macOS / desktop Linux */

#include <limits.h>

bool
target_input_plugin_get_force_qwerty(void)
{
	char dir[PATH_MAX];
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return false;
	}
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/force_qwerty", dir);

	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	int c = fgetc(f);
	fclose(f);
	/* Presence forces qwerty; a leading '0' turns the file into a
	 * no-op so it can be toggled without deleting it. */
	return c != '0';
}

/*
 * N3a: the preferred provider — the DP loader's §3.3 pattern: an env var
 * for dev/CI (no writable filesystem needed), else a one-line file next to
 * `force_qwerty`, which is what `displayxr-cli input use <id>` / `input
 * reset` write.
 */
bool
target_input_plugin_get_preferred(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';

	const char *env = getenv("XRT_PREFERRED_INPUT_PLUGIN_ID");
	if (env != NULL && *env != '\0') {
		snprintf(out, cap, "%s", env);
		return out[0] != '\0';
	}

	char dir[PATH_MAX];
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return false;
	}
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/input-preferred", dir);

	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	char buf[64] = {0};
	size_t got = fread(buf, 1, sizeof(buf) - 1, f);
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

#endif /* platform-specific gate */


/*
 *
 * Public iface.
 *
 */

const struct xrt_input_plugin_iface *
target_input_plugin_get_active(void)
{
	return target_input_plugin_get_iface(0);
}

/*!
 * `DXR_INPUT_PROVIDERS=0` — skip input-provider discovery entirely (#1545,
 * #1523): no provider DLL is loaded, `get_count()` stays 0, the builder never
 * calls `create_devices`, and the hand roles sit on the qwerty floor. Behavior
 * is bit-identical to a box with no provider registered.
 *
 * Same effect as the `HKLM\Software\DisplayXR\Input\ForceQwerty` gate (§4.3 of
 * the discovery spec), deliberately as an **env var** rather than reusing it:
 * a CTS lane wants the suppression scoped to the one process it launches, and
 * a run killed mid-flight (which the harness does on timeout) must not be able
 * to leave the box with input machine-wide disabled.
 *
 * Read via CRT `getenv`, so it must be inherited from the launching process —
 * see the note on `DXR_PLUGIN_EXCLUSIVE` in `target_plugin_loader.c`.
 */
static bool
input_providers_disabled_by_env(void)
{
	const char *env = getenv("DXR_INPUT_PROVIDERS");
	return env != NULL && env[0] == '0' && env[1] == '\0';
}

const struct xrt_input_plugin_iface *
target_input_plugin_get_iface(int index)
{
	if (!g_input_load_attempted) {
		g_input_load_attempted = 1;
		if (input_providers_disabled_by_env()) {
			/* One WARN, once per process: the scan is one-shot, so
			 * this cannot bloat a log even across the hundreds of
			 * instances a CTS run creates. */
			U_LOG_W(
			    "input plugin loader: DXR_INPUT_PROVIDERS=0 — skipping input-provider discovery "
			    "entirely; no provider is loaded and the hand roles stay on qwerty.");
		} else {
			input_discover_all();
		}
	}
	if (index < 0 || index >= g_input_provider_count) {
		return NULL;
	}
	return g_input_providers[index].iface;
}

struct xrt_input_plugin_instance *
target_input_plugin_get_instance(int index)
{
	(void)target_input_plugin_get_iface(0); // Force discovery.
	if (index < 0 || index >= g_input_provider_count) {
		return NULL;
	}
	return g_input_providers[index].inst;
}

uint32_t
target_input_plugin_get_priority(int index)
{
	(void)target_input_plugin_get_iface(0); // Force discovery.
	if (index < 0 || index >= g_input_provider_count) {
		return 0xFFFFFFFFu;
	}
	return g_input_providers[index].probe_order;
}

int
target_input_plugin_get_count(void)
{
	(void)target_input_plugin_get_iface(0); // Force discovery.
	return g_input_provider_count;
}

struct xrt_input_plugin_instance *
target_input_plugin_get_active_instance(void)
{
	return target_input_plugin_get_instance(0);
}

void
target_input_plugin_get_scan_result(int *out_declined, int *out_failed)
{
	(void)target_input_plugin_get_active();
	if (out_declined != NULL) {
		*out_declined = g_input_scan_declined;
	}
	if (out_failed != NULL) {
		*out_failed = g_input_scan_failed;
	}
}
