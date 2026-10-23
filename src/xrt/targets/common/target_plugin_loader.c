// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-side loader for display-processor plug-ins.
 *
 * The display-processor class loader (ADR-019/020), written as ADR-054 N2
 * intends: discovery (registry rows / manifests / Android file names), the
 * #952 / #328 / #434 load path and the ADR-020 ABI gate are the shared
 * component loader's (`target_component_loader.h`), driven by this class's
 * table row (@ref g_dp_class). What is left here is what only this class
 * knows: the host iface a plug-in receives (incl. the Android JNI
 * callbacks), what `probe()` means, the ADR-045 per-plug-in status records,
 * the single-winner selection with its PreferredPlugin / DXR_PLUGIN_EXCLUSIVE
 * policy, the #342 refresh, the per-monitor display-claim source set (#69 /
 * ADR-015) and the #461 version-skew tripwire.
 *
 * First plug-in whose probe returns `XRT_SUCCESS` wins and is cached. The
 * plug-in's chosen instance handle (returned by `probe()`) is kept alongside
 * the iface so the runtime can hand it back to subsequent vtable calls. v1
 * hosts at most one ACTIVE plug-in per process; every other registered one
 * may still be loaded as a display-claim source.
 *
 * Issue #256; ADR-054 N2.
 *
 * @ingroup target_common
 */

#include "target_plugin_loader.h"
#include "target_component_loader.h"
#include "target_screen_pin.h"
#include "target_display_hotplug.h"
#include "xrt/xrt_config_have.h"

#ifdef XRT_HAVE_VULKAN
/* #1243 — Vulkan-free prototypes shared with the defining TU (vk_helpers.c),
 * so the declarations cannot silently diverge from the definitions. */
#include "vk/vk_abi_fingerprint.h"
#endif

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_display_status.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_config_os.h"

#include "os/os_threading.h"
#include "os/os_display_edid.h"
#include "os/os_display_desktop.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_setting.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Shared state.
 *
 */

static int g_load_attempted = 0;
static const struct xrt_plugin_iface *g_active_iface = NULL;
static struct xrt_plugin_instance *g_active_instance = NULL;

/*!
 * Winning ProbeOrder from the most recent successful discovery, or UINT32_MAX
 * when no plug-in is active (first-call path or empty discovery root).
 * @ref target_plugin_refresh_active uses it as a strict upper bound: a refresh
 * only adopts a plug-in whose ProbeOrder is STRICTLY LESS than this — meaning
 * we never re-probe the already-active plug-in, only ones that legitimately
 * out-rank it.
 */
static uint32_t g_active_probe_order = 0xFFFFFFFFu;

/*!
 * Guards @ref target_plugin_refresh_active against concurrent IPC-client
 * compositor creates in service mode. Initialized lazily on the
 * (single-threaded) first call to @ref target_plugin_get_active, which always
 * precedes any compositor-create-time refresh. Using os_mutex (not C11 atomics)
 * per the MinGW caveat in CLAUDE.md.
 */
static struct os_mutex g_refresh_mutex;
static int g_refresh_mutex_initialized = 0;

/*!
 * Discovery outcome, for the `vendor_dp` self-test check (#1212).
 *
 * The problem it solves: `target_plugin_get_active()` returning non-NULL says
 * only that SOMETHING claimed the system — including the vendor-neutral
 * sim_display fallback at ProbeOrder 200. On a device that has a vendor
 * display, that is the correct answer to the wrong question, and the self-test
 * reported a green PASS over a black screen.
 *
 * Discovery attempts candidates in ascending ProbeOrder and returns on the
 * first success, so ANY candidate attempted before the winner is by
 * construction a better-ranked plug-in that failed to load — an ABI reject
 * (the exact "hand-built plug-ins rot against the ABI gate" failure), a
 * dlopen/LoadLibrary failure, or a declined probe. Counting those is therefore
 * sufficient; no vendor names are involved, so this stays vendor-neutral and
 * works identically on Windows, POSIX and Android.
 *
 * Deliberately NOT built on @ref target_plugin_enumerate: that is a stub
 * returning 0 on Android, which is precisely the platform this check exists
 * for.
 */
static int g_rejected_count = 0;   //!< better-ranked candidates that FAILED TO LOAD
static int g_declined_count = 0;   //!< better-ranked candidates that loaded and declined
static uint32_t g_best_rejected_order = 0xFFFFFFFFu;
static char g_best_rejected_id[64] = {0};
static char g_best_rejected_reason[128] = {0};
/*! Set by try_load_one's failure paths, consumed by the discovery loop. */
static char g_last_reject_reason[128] = {0};
/*!
 * True when the most recent failure was a plug-in that loaded cleanly and then
 * DECLINED its probe. That is correct behaviour on hardware the plug-in does
 * not serve, not a misconfiguration, so it must not fail the self-test — the
 * distinction that keeps a dev box with a vendor plug-in registered but no
 * panel attached from going red.
 */
static bool g_last_reject_declined = false;

/*!
 * N3b: better-ranked plug-ins the first-call discovery skipped because they
 * are disabled (plus a disabled PreferredPlugin). Reported through
 * @ref target_plugin_get_discovery_summary so `selftest` can say "sim-display
 * is active because leia-sr is disabled" instead of leaving a vendor box's
 * fallback looking like a mystery. Never counted as a reject: a disabled
 * plug-in is the user's decision, not a misconfiguration.
 */
#define TARGET_PLUGIN_MAX_SKIPPED_DISABLED 8
static int g_skipped_disabled_count = 0;
static char g_skipped_disabled_ids[TARGET_PLUGIN_MAX_SKIPPED_DISABLED][64];
//! Registration id of the winning row (see the summary's `active_id`).
static char g_active_row_id[64] = {0};

static void
plugin_note_skipped_disabled(const char *id)
{
	for (int i = 0; i < g_skipped_disabled_count; i++) {
		if (strcmp(g_skipped_disabled_ids[i], id) == 0) {
			return;
		}
	}
	if (g_skipped_disabled_count < TARGET_PLUGIN_MAX_SKIPPED_DISABLED) {
		snprintf(g_skipped_disabled_ids[g_skipped_disabled_count], sizeof(g_skipped_disabled_ids[0]), "%s", id);
		g_skipped_disabled_count++;
	}
}

static void
plugin_note_reject(const char *id, uint32_t probe_order)
{
	if (g_last_reject_declined) {
		/* Loaded fine, said "not my hardware". Not a fault. */
		g_declined_count++;
		g_last_reject_declined = false;
		g_last_reject_reason[0] = '\0';
		return;
	}
	g_rejected_count++;
	if (probe_order < g_best_rejected_order) {
		g_best_rejected_order = probe_order;
		snprintf(g_best_rejected_id, sizeof(g_best_rejected_id), "%s", id != NULL ? id : "?");
		snprintf(g_best_rejected_reason, sizeof(g_best_rejected_reason), "%s",
		         g_last_reject_reason[0] != '\0' ? g_last_reject_reason : "load or probe failed");
	}
	g_last_reject_reason[0] = '\0';
}

/*!
 * `DXR_PLUGIN_EXCLUSIVE=<plugin-id>` — load ONLY the named display plug-in and
 * do not even `LoadLibrary`/`dlopen` any other registered one (#1545, #1523).
 *
 * This is NOT `PreferredPlugin` (#378) and NOT a ProbeOrder tweak: both of
 * those only change WHICH plug-in wins, while every other registered plug-in
 * is still loaded — `collect_display_sources_platform` deliberately consults
 * all of them for display claims (#69 / ADR-015). Loading is the problem this
 * variable exists for: on a box with a vendor plug-in installed, that load
 * drags the vendor's whole dependency chain into the process (Leia SR →
 * `SimulatedRealityOpenGL.dll` → `opengl32` → the NVIDIA GL ICD), which is
 * how a `-G d3d11` CTS run ends up faulting in an OpenGL ICD worker thread
 * (#1545). A CTS lane wants one plug-in resident and nothing else.
 *
 * Deliberately NO fallback on a miss, which is the other difference from
 * `PreferredPlugin`: a stale preference falls through to ProbeOrder so a bad
 * value can never brick discovery, but "exclusive" that quietly loaded
 * something else would defeat its only purpose. A typo therefore loads
 * nothing, and the WARN below names every registered id so the typo is
 * obvious in the log rather than in a mystery crash.
 *
 * Read with CRT `getenv` like the loader's other env overrides
 * (`XRT_PLUGIN_SEARCH_PATH`, `XRT_PREFERRED_PLUGIN_ID`), so it must be set in
 * the environment the process INHERITS — a launcher script, or the PowerShell
 * process env before `Start-Process` (what `scripts/run_cts.ps1` does). An
 * in-process client calling `SetEnvironmentVariableW` after startup is
 * silently ignored; see `docs/reference/adapter-selection.md` § *The getenv()
 * caveat*.
 */
static const char *
plugin_exclusive_id(void)
{
	/* Copied, not aliased: a later putenv/_putenv in the host process may
	 * free or move what getenv returned, and this pointer is consulted for
	 * the process lifetime (every refresh pass). 64 == plugin_entry::id. */
	static char s_id[64] = {0};
	static bool s_read = false;
	if (!s_read) {
		s_read = true;
		const char *env = getenv("DXR_PLUGIN_EXCLUSIVE");
		if (env != NULL && env[0] != '\0') {
			snprintf(s_id, sizeof(s_id), "%s", env);
			U_LOG_W(
			    "plugin loader: DXR_PLUGIN_EXCLUSIVE='%s' — loading ONLY that display plug-in; "
			    "every other registered plug-in is skipped (not loaded).",
			    s_id);
		}
	}
	return s_id[0] != '\0' ? s_id : NULL;
}

/*!
 * True when @p id is NOT the exclusive plug-in and must therefore be skipped
 * before any load attempt. Exact `strcmp`, matching the `PreferredPlugin`
 * comparison — the id is the registry subkey name on Windows and the
 * manifest's `id` on POSIX.
 *
 * Callers must skip such an entry WITHOUT calling @ref plugin_note_reject: a
 * deliberately-excluded plug-in is not a better-ranked candidate that failed,
 * and counting it would turn `displayxr-cli selftest`'s vendor-DP check red on
 * a lane that is working exactly as asked (#1212 keys off that counter).
 */
static bool
plugin_id_excluded(const char *id)
{
	const char *only = plugin_exclusive_id();
	if (only == NULL) {
		return false;
	}
	return id == NULL || strcmp(id, only) != 0;
}

/*!
 * One WARN when `DXR_PLUGIN_EXCLUSIVE` names an id nothing is registered
 * under. Since there is no fallback by design, that run loads no display
 * plug-in at all and every downstream failure ("Failed to initialize OpenXR",
 * `XRT_ERROR_DEVICE_CREATION_FAILED`) would otherwise look like a broken box
 * rather than a typo, so the message lists what IS registered.
 *
 * Call sites pass the ids they enumerated; harmless (and silent) when the
 * variable is unset or does match.
 */
static void
plugin_warn_exclusive_miss(const char *const *ids, int n)
{
	const char *only = plugin_exclusive_id();
	if (only == NULL) {
		return;
	}
	for (int i = 0; i < n; i++) {
		if (ids[i] != NULL && strcmp(ids[i], only) == 0) {
			return;
		}
	}
	/* Once per process: discovery also runs on the mid-install refresh
	 * (#342), which a service hits on every compositor create. */
	static bool s_warned = false;
	if (s_warned) {
		return;
	}
	s_warned = true;

	char list[512];
	size_t used = 0;
	list[0] = '\0';
	for (int i = 0; i < n && used + 1 < sizeof(list); i++) {
		int wrote = snprintf(list + used, sizeof(list) - used, "%s%s", used > 0 ? ", " : "",
		                     ids[i] != NULL ? ids[i] : "?");
		if (wrote < 0) {
			break;
		}
		used += (size_t)wrote;
	}

	U_LOG_W(
	    "plugin loader: DXR_PLUGIN_EXCLUSIVE='%s' matches none of the %d registered plug-in(s) [%s] — "
	    "NOTHING will be loaded (no fallback, by design). Check the id: it is the registry subkey name on "
	    "Windows and the manifest 'id' on POSIX.",
	    only, n, list);
}

void
target_plugin_get_discovery_summary(struct target_plugin_discovery_summary *out)
{
	if (out == NULL) {
		return;
	}
	memset(out, 0, sizeof(*out));
	out->rejected_count = g_rejected_count;
	out->declined_count = g_declined_count;
	out->active_probe_order = g_active_probe_order;
	out->best_rejected_order = g_best_rejected_order;
	snprintf(out->best_rejected_id, sizeof(out->best_rejected_id), "%s", g_best_rejected_id);
	snprintf(out->best_rejected_reason, sizeof(out->best_rejected_reason), "%s", g_best_rejected_reason);
	out->skipped_disabled_count = g_skipped_disabled_count;
	for (int i = 0; i < g_skipped_disabled_count && i < TARGET_PLUGIN_MAX_SKIPPED_DISABLED; i++) {
		snprintf(out->skipped_disabled_ids[i], sizeof(out->skipped_disabled_ids[i]), "%s",
		         g_skipped_disabled_ids[i]);
	}
	snprintf(out->active_id, sizeof(out->active_id), "%s", g_active_iface != NULL ? g_active_row_id : "");
}

/*
 *
 * Per-plug-in status records + platform state (ADR-045).
 *
 */

#define TARGET_PLUGIN_MAX_STATUS 16

/*!
 * One record per registered plug-in the loader has attempted (or skipped)
 * this process, keyed by id. Written by every load attempt — first discovery,
 * each refresh, the display-claim collection — and read by diagnostics
 * (`displayxr-cli`, the service tray) from other threads, so it has its own
 * small mutex rather than @ref g_refresh_mutex (a refresh can hold that one
 * for as long as a slow plug-in probe takes).
 */
static struct target_plugin_status g_status[TARGET_PLUGIN_MAX_STATUS];
static int g_status_count = 0;
//! Per record: a load of this id has been attempted at least once (the #434
//! breadcrumb and the #461 skew check WARN only on the first attempt).
static bool g_status_attempted[TARGET_PLUGIN_MAX_STATUS];
static struct os_mutex g_status_mutex;
static int g_status_mutex_initialized = 0;

static void
status_lock(void)
{
	if (g_status_mutex_initialized) {
		os_mutex_lock(&g_status_mutex);
	}
}

static void
status_unlock(void)
{
	if (g_status_mutex_initialized) {
		os_mutex_unlock(&g_status_mutex);
	}
}

//! Find or add the record for @p id. Caller holds the status lock. -1 if full.
static int
status_find_or_add_locked(const char *id)
{
	for (int i = 0; i < g_status_count; i++) {
		if (strcmp(g_status[i].id, id) == 0) {
			return i;
		}
	}
	if (g_status_count >= TARGET_PLUGIN_MAX_STATUS) {
		return -1;
	}
	int i = g_status_count++;
	memset(&g_status[i], 0, sizeof(g_status[i]));
	g_status_attempted[i] = false;
	snprintf(g_status[i].id, sizeof(g_status[i].id), "%s", id);
	return i;
}

/*!
 * Start an attempt at one registered plug-in: refresh its identity fields from
 * the discovery root. Returns true if this is the FIRST attempt at this id in
 * this process (callers keep their one-shot WARNs to that attempt).
 */
static bool
status_begin(const char *id, const char *display_name, const char *version, uint32_t probe_order)
{
	bool first = true;
	status_lock();
	int i = status_find_or_add_locked(id != NULL ? id : "?");
	if (i >= 0) {
		struct target_plugin_status *s = &g_status[i];
		snprintf(s->display_name, sizeof(s->display_name), "%s", display_name != NULL ? display_name : "");
		snprintf(s->version, sizeof(s->version), "%s", version != NULL ? version : "");
		s->probe_order = probe_order;
		first = !g_status_attempted[i];
		g_status_attempted[i] = true;
	}
	status_unlock();
	return first;
}

/*!
 * Record the outcome of an attempt. Returns true when it DIFFERS from the
 * previous outcome recorded for this id (or is the first), so a caller can log
 * a repeated failure at INFO instead of WARN: the service re-probes on world
 * events and on a slow timer while the fallback is active, and a vendor
 * plug-in whose platform is absent must cost one WARN per process, not one per
 * retry.
 */
static bool
status_finish(const char *id, enum target_plugin_load_result result, uint32_t os_error, const char *reason)
{
	bool changed = true;
	status_lock();
	int i = status_find_or_add_locked(id != NULL ? id : "?");
	if (i >= 0) {
		struct target_plugin_status *s = &g_status[i];
		changed = !(s->result == result && s->os_error == os_error);
		s->result = result;
		s->os_error = os_error;
		snprintf(s->reason, sizeof(s->reason), "%s", reason != NULL ? reason : "");
		if (result == TARGET_PLUGIN_RESULT_BINARY_MISSING ||
		    result == TARGET_PLUGIN_RESULT_DEPENDENCY_MISSING || result == TARGET_PLUGIN_RESULT_LOAD_FAILED ||
		    result == TARGET_PLUGIN_RESULT_PATH_REFUSED || result == TARGET_PLUGIN_RESULT_NO_ENTRY_POINT ||
		    result == TARGET_PLUGIN_RESULT_NEGOTIATE_FAILED || result == TARGET_PLUGIN_RESULT_ABI_MISMATCH) {
			// Never got far enough to ask: a stale state from an
			// earlier attempt would be a lie.
			s->platform_state = XRT_PLUGIN_PLATFORM_STATE_UNKNOWN;
			s->platform_flags = 0;
			s->hint[0] = '\0';
		}
	}
	status_unlock();
	return changed;
}

//! Mark @p id as the active plug-in; any previously ACTIVE record becomes CLAIMED.
static void
status_set_active(const char *id)
{
	status_lock();
	for (int i = 0; i < g_status_count; i++) {
		if (g_status[i].result == TARGET_PLUGIN_RESULT_ACTIVE) {
			g_status[i].result = TARGET_PLUGIN_RESULT_CLAIMED;
		}
	}
	int i = status_find_or_add_locked(id != NULL ? id : "?");
	if (i >= 0) {
		g_status[i].result = TARGET_PLUGIN_RESULT_ACTIVE;
	}
	status_unlock();
}

/*!
 * Ask @p iface for its platform state through the struct_size-gated optional
 * slot. Always fills @p out (UNKNOWN when not reported).
 */
static bool
query_platform_state(const struct xrt_plugin_iface *iface, struct xrt_plugin_platform_status *out)
{
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	if (iface == NULL ||
	    iface->struct_size <
	        offsetof(struct xrt_plugin_iface, get_platform_state) + sizeof(iface->get_platform_state) ||
	    iface->get_platform_state == NULL) {
		return false;
	}
	if (!iface->get_platform_state(out)) {
		memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		return false;
	}
	out->hint[sizeof(out->hint) - 1] = '\0';
	if (out->state > XRT_PLUGIN_PLATFORM_STATE_INCOMPATIBLE) {
		out->state = XRT_PLUGIN_PLATFORM_STATE_UNKNOWN;
	}
	return true;
}

/*!
 * Load-path hook, called after negotiate + the ABI gate and BEFORE probe():
 * record what the plug-in says about its platform, so a plug-in that is about
 * to decline can still say why. Logs only on a change of state.
 */
static void
status_record_platform_state(const char *id, const struct xrt_plugin_iface *iface)
{
	struct xrt_plugin_platform_status ps;
	bool reported = query_platform_state(iface, &ps);
	bool changed = false;
	// The DP factories the iface offers (plain struct fields — no call, so
	// valid before probe() and for a plug-in that is about to decline).
	uint32_t apis = 0;
	if (iface != NULL) {
		apis |= iface->create_dp_vk != NULL ? XRT_STATUS_API_BIT_VK : 0u;
		apis |= iface->create_dp_metal != NULL ? XRT_STATUS_API_BIT_METAL : 0u;
		apis |= iface->create_dp_gl != NULL ? XRT_STATUS_API_BIT_GL : 0u;
		apis |= iface->create_dp_d3d11 != NULL ? XRT_STATUS_API_BIT_D3D11 : 0u;
		apis |= iface->create_dp_d3d12 != NULL ? XRT_STATUS_API_BIT_D3D12 : 0u;
	}
	status_lock();
	int i = status_find_or_add_locked(id != NULL ? id : "?");
	if (i >= 0) {
		struct target_plugin_status *s = &g_status[i];
		s->dp_apis = apis;
		changed = s->platform_state != ps.state || strcmp(s->hint, ps.hint) != 0;
		s->platform_state = ps.state;
		s->platform_flags = ps.flags;
		snprintf(s->hint, sizeof(s->hint), "%s", ps.hint);
	}
	status_unlock();
	if (reported && changed) {
		U_LOG_W("plugin loader:   %s: platform state %s%s%s", id != NULL ? id : "?",
		        target_plugin_platform_state_str(ps.state), ps.hint[0] != '\0' ? " — " : "", ps.hint);
	}
}

bool
target_plugin_find_metal_only(char *out_id, size_t cap)
{
	if (out_id != NULL && cap > 0) {
		out_id[0] = '\0';
	}
	bool found = false;
	status_lock();
	for (int i = 0; i < g_status_count && !found; i++) {
		const struct target_plugin_status *s = &g_status[i];
		if ((s->platform_flags & XRT_PLUGIN_PLATFORM_FLAG_FALLBACK) != 0 ||
		    (s->dp_apis & XRT_STATUS_API_BIT_METAL) == 0 || (s->dp_apis & XRT_STATUS_API_BIT_VK) != 0) {
			continue;
		}
		found = true;
		if (out_id != NULL && cap > 0) {
			snprintf(out_id, cap, "%s", s->id);
		}
	}
	status_unlock();
	return found;
}

const char *
target_plugin_load_result_str(enum target_plugin_load_result r)
{
	switch (r) {
	case TARGET_PLUGIN_RESULT_NOT_ATTEMPTED: return "NOT_ATTEMPTED";
	case TARGET_PLUGIN_RESULT_ACTIVE: return "ACTIVE";
	case TARGET_PLUGIN_RESULT_CLAIMED: return "LOADED";
	case TARGET_PLUGIN_RESULT_DECLINED: return "DECLINED";
	case TARGET_PLUGIN_RESULT_BINARY_MISSING: return "BINARY_MISSING";
	case TARGET_PLUGIN_RESULT_DEPENDENCY_MISSING: return "DEPENDENCY_MISSING";
	case TARGET_PLUGIN_RESULT_LOAD_FAILED: return "LOAD_FAILED";
	case TARGET_PLUGIN_RESULT_PATH_REFUSED: return "PATH_REFUSED";
	case TARGET_PLUGIN_RESULT_NO_ENTRY_POINT: return "NO_ENTRY_POINT";
	case TARGET_PLUGIN_RESULT_NEGOTIATE_FAILED: return "NEGOTIATE_FAILED";
	case TARGET_PLUGIN_RESULT_ABI_MISMATCH: return "ABI_MISMATCH";
	case TARGET_PLUGIN_RESULT_PROBE_FAILED: return "PROBE_FAILED";
	case TARGET_PLUGIN_RESULT_UNTRUSTED: return "UNTRUSTED";
	case TARGET_PLUGIN_RESULT_DISABLED: return "DISABLED";
	}
	return "?";
}

const char *
target_plugin_platform_state_str(uint32_t state)
{
	switch (state) {
	case XRT_PLUGIN_PLATFORM_STATE_UNKNOWN: return "UNKNOWN";
	case XRT_PLUGIN_PLATFORM_STATE_READY: return "READY";
	case XRT_PLUGIN_PLATFORM_STATE_PLATFORM_ABSENT: return "PLATFORM_ABSENT";
	case XRT_PLUGIN_PLATFORM_STATE_PLATFORM_NOT_RUNNING: return "PLATFORM_NOT_RUNNING";
	case XRT_PLUGIN_PLATFORM_STATE_NO_DISPLAY: return "NO_DISPLAY";
	case XRT_PLUGIN_PLATFORM_STATE_INCOMPATIBLE: return "INCOMPATIBLE";
	default: return "UNKNOWN";
	}
}

bool
target_plugin_iface_is_fallback(const struct xrt_plugin_iface *iface)
{
	if (iface == NULL) {
		return false;
	}
	struct xrt_plugin_platform_status ps;
	if (query_platform_state(iface, &ps)) {
		return (ps.flags & XRT_PLUGIN_PLATFORM_FLAG_FALLBACK) != 0;
	}
	// A plug-in too old to report state: only the runtime's OWN simulation
	// plug-in is a fallback. Never a vendor id.
	return iface->id != NULL && strcmp(iface->id, "sim-display") == 0;
}

bool
target_plugin_query_active_platform_state(struct xrt_plugin_platform_status *out)
{
	if (out == NULL) {
		return false;
	}
	return query_platform_state(g_active_iface, out);
}

int
target_plugin_get_status(struct target_plugin_status *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}
	const struct xrt_plugin_iface *active = g_active_iface;
	struct xrt_plugin_platform_status live;
	bool live_ok = query_platform_state(active, &live);

	status_lock();
	int n = g_status_count < max ? g_status_count : max;
	for (int i = 0; i < n; i++) {
		out[i] = g_status[i];
		if (live_ok && out[i].result == TARGET_PLUGIN_RESULT_ACTIVE && active != NULL && active->id != NULL &&
		    strcmp(active->id, out[i].id) == 0) {
			out[i].platform_state = live.state;
			out[i].platform_flags = live.flags;
			snprintf(out[i].hint, sizeof(out[i].hint), "%s", live.hint);
		}
		out[i].fallback = (out[i].platform_flags & XRT_PLUGIN_PLATFORM_FLAG_FALLBACK) != 0 ||
		                  (out[i].platform_state == XRT_PLUGIN_PLATFORM_STATE_UNKNOWN &&
		                   strcmp(out[i].id, "sim-display") == 0);
	}
	status_unlock();
	return n;
}

/*!
 * Log a load-path line at WARN the first time an outcome is seen for a
 * plug-in, at INFO when it merely repeats (re-probes, ADR-045).
 */
#define PLUGIN_LOG_OUTCOME(changed, ...)                                                                               \
	do {                                                                                                           \
		if (changed) {                                                                                         \
			U_LOG_W(__VA_ARGS__);                                                                          \
		} else {                                                                                               \
			U_LOG_I(__VA_ARGS__);                                                                          \
		}                                                                                                      \
	} while (false)

/*!
 * Max plug-in sources consulted when building the per-display registry
 * (issue #69 / ADR-015). One per registered plug-in — a handful in practice.
 */
#define TARGET_PLUGIN_MAX_SOURCES 16

/*!
 * One loaded plug-in consulted for display claims. Distinct from the single
 * "active" plug-in (which drives create_device / get_display_info): the
 * display registry asks EVERY registered plug-in for its claims, so the
 * lower-confidence fallback (sim_display) and a vendor plug-in can both
 * contribute. The active plug-in is reused as one of these sources rather
 * than loaded twice.
 */
struct plugin_display_source
{
	const struct xrt_plugin_iface *iface;
	struct xrt_plugin_instance *inst;
	uint32_t probe_order; /* ascending = higher priority on a confidence tie */
};

/*!
 * Cached display-claim source set, built lazily on the first
 * @ref target_plugin_resolve_displays and reused for the process lifetime.
 * `< 0` means "not collected yet"; reset to -1 when
 * @ref target_plugin_refresh_active adopts a better plug-in so the next
 * resolve rebuilds. Guarded by @ref g_refresh_mutex.
 */
static struct plugin_display_source g_display_sources[TARGET_PLUGIN_MAX_SOURCES];
static int g_display_source_count = -1;

/*!
 * POSIX: the cached source set is ONLY the active plug-in, because it claimed
 * every monitor of the last resolve, so no other plug-in could have changed
 * the outcome (see @ref ensure_display_sources). Re-checked on every resolve,
 * since a later descriptor set may hold a monitor it does not claim. Guarded
 * by @ref g_refresh_mutex.
 */
static bool g_display_sources_active_only = false;


/*
 *
 * The class row (ADR-054 §2): what the shared component loader needs to know
 * about display processors. Discovery, the #952 / #328 / #434 load path and
 * the ADR-020 ABI gate are its (`target_component_loader.h`); everything from
 * here down is what only this class knows — the host iface a plug-in gets,
 * what `probe()` means, the ADR-045 status records, the display-claim source
 * set, the PreferredPlugin / DXR_PLUGIN_EXCLUSIVE selection policy.
 *
 */

static bool
dp_is_reserved_subkey(const char *name)
{
	// Policy lives beside the rows under the same root: the per-screen
	// `PreferredPlugin\<screen key>` pins (plugin-discovery.md §4) and a
	// future `Policy\Lock`. Neither is a plug-in and neither may be listed as
	// a malformed one.
	static const char *const reserved[] = {"PreferredPlugin", "Policy", NULL};
	if (name == NULL) {
		return false;
	}
	for (int i = 0; reserved[i] != NULL; i++) {
		const char *a = name;
		const char *b = reserved[i];
		while (*a != '\0' && *b != '\0') {
			int ca = (*a >= 'A' && *a <= 'Z') ? *a - 'A' + 'a' : *a;
			int cb = (*b >= 'A' && *b <= 'Z') ? *b - 'A' + 'a' : *b;
			if (ca != cb) {
				break;
			}
			a++;
			b++;
		}
		if (*a == '\0' && *b == '\0') {
			return true;
		}
	}
	return false;
}

static const struct target_component_class g_dp_class = {
    // "plug-in" rather than "display processor": the shared loader builds
    // "loading plug-in binary", "no plug-ins to try" and "the plug-in must be
    // rebuilt" from it, and those are the DP log lines diagnostics grep for.
    .name = "plug-in",
    .log_tag = "plugin loader",
    .entrypoint = XRT_PLUGIN_ENTRYPOINT_NAME,
    .abi_current = XRT_PLUGIN_API_VERSION_CURRENT,
    .registry_root = "Software\\DisplayXR\\DisplayProcessors",
    .manifest_kind = TARGET_PLUGIN_MANIFEST_DISPLAY,
    .android_prefix = TARGET_PLUGIN_ANDROID_PREFIX_DISPLAY,
    .is_reserved_subkey = dp_is_reserved_subkey,
    .read_extra = NULL,
    .env_class = "DP", // DXR_DP_<ID>_<KEY> (N3c)
};

//! One per-class entry array, heap-allocated (~11 KB a row).
static struct target_component_entry *
dp_entries_alloc(void)
{
	return (struct target_component_entry *)calloc(TARGET_COMPONENT_MAX_ENTRIES, sizeof(struct target_component_entry));
}


/*
 *
 * Platform islands. The only code left that differs per OS: the Android host
 * callbacks and lib-dir preload, the Windows #461 version-skew tripwire, and
 * the PreferredPlugin store (registry / per-user file / env).
 *
 */

#ifdef XRT_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

/* GetFileVersionInfoW / VerQueryValueW for the registry↔DLL version-skew
 * tripwire (#461). MSVC-style autolink keeps the dependency local to this
 * translation unit instead of threading version.lib through every target
 * that links target_common. (MinGW ignores the pragma; the cross-check
 * subset doesn't build this target.) */
#ifdef _MSC_VER
#pragma comment(lib, "version.lib")
#endif

/*!
 * Registry↔DLL version-skew tripwire (#461). Installers replace the plug-in
 * DLL and rewrite the registry `Version` in one transaction — EXCEPT when a
 * running process (typically displayxr-service) has the DLL mapped: NSIS
 * silent installs skip locked files with exit 0, leaving an old DLL on disk
 * under a new registry version (the v0.14.0 bundle incident). Compare the
 * registry semver against the DLL's embedded VERSIONINFO and WARN loudly on
 * mismatch. Quietly does nothing when the DLL ships no version resource
 * (sim-display today), the registry value isn't a semver, or the file is not
 * there (the load path reports that one).
 */
static void
check_registry_dll_version_skew(const struct target_component_entry *e)
{
	unsigned reg_major = 0, reg_minor = 0, reg_patch = 0;
	const char *v = e->version;
	if (*v == 'v') {
		v++;
	}
	if (sscanf(v, "%u.%u.%u", &reg_major, &reg_minor, &reg_patch) != 3) {
		return; /* empty / non-semver registry Version — nothing to compare */
	}

	wchar_t wpath[TARGET_COMPONENT_PATH_MAX];
	if (MultiByteToWideChar(CP_UTF8, 0, e->binary_path, -1, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) <= 0) {
		return;
	}

	DWORD ignored = 0;
	DWORD size = GetFileVersionInfoSizeW(wpath, &ignored);
	if (size == 0) {
		return; /* DLL ships no VERSIONINFO resource — nothing to compare */
	}
	void *buf = malloc(size);
	if (buf == NULL) {
		return;
	}
	VS_FIXEDFILEINFO *ffi = NULL;
	UINT ffi_len = 0;
	if (GetFileVersionInfoW(wpath, 0, size, buf) && VerQueryValueW(buf, L"\\", (LPVOID *)&ffi, &ffi_len) &&
	    ffi != NULL && ffi_len >= sizeof(*ffi)) {
		unsigned dll_major = HIWORD(ffi->dwFileVersionMS);
		unsigned dll_minor = LOWORD(ffi->dwFileVersionMS);
		unsigned dll_patch = HIWORD(ffi->dwFileVersionLS);
		if (dll_major != reg_major || dll_minor != reg_minor || dll_patch != reg_patch) {
			U_LOG_W("plugin loader:   %s: registry Version '%s' != DLL file version "
			        "%u.%u.%u (%s) — an installer likely failed to replace a locked "
			        "DLL; reinstall the plug-in with displayxr-service stopped (#461).",
			        e->id, e->version, dll_major, dll_minor, dll_patch, e->binary_path);
		}
	}
	free(buf);
}

//! Nothing to do before discovery on Windows: the shared load path pins the core DLL (#328) per load.
static void
dp_platform_prepare_discovery(void)
{
}

#elif defined(XRT_OS_ANDROID)

#include "android/android_globals.h"

#include <jni.h>

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Host-iface callback: hand the plug-in the runtime's Android `JavaVM *`
 * (as `void *`). A plug-in that statically links aux_android has its own
 * private, never-populated VM globals, so it can't call
 * `android_globals_get_vm()` itself — it must go through the host iface.
 * Thin cast wrapper so the function pointer matches the `void *(*)(void)`
 * field type (`android_globals_get_activity` already returns `void *`).
 */
static void *
plugin_host_get_android_vm(void)
{
	return (void *)android_globals_get_vm();
}

/*
 * Host-iface callback: hand the plug-in an Android `Context` (as a jobject
 * `void *`) for vendor-SDK init — e.g. CNSDK's loader, which binds the
 * on-device LeiaSR face-tracking service (the same out-of-process flow the
 * stock CNSDK apps use) and needs only an `android.content.Context`, not an
 * Activity. In-process the host is an Activity (returned verbatim, so the
 * plug-in's Activity-only paths like orientation-locking still work); in the
 * out-of-process SERVICE there is no Activity, only the Service `Context`
 * stored by nativeStartServer → android_globals_store_vm_and_context. The
 * callback is named *_activity for back-compat with the host-iface slot, but
 * it returns the Activity when present and otherwise the Service Context.
 * Without this, get_android_activity() returned NULL in the service process →
 * CNSDK aborted with "android context and JavaVM must be specified" → no
 * tracked eyes (#510 M2).
 */
static void *
plugin_host_get_android_activity(void)
{
	void *activity = android_globals_get_activity();
	return activity != NULL ? activity : android_globals_get_context();
}

/*
 * runtime#1079 — is `package_name` visible to THIS process?
 *
 * Android resolves `<queries>` per calling uid. In-process (ADR-036 D2) the
 * vendor DP runs in the app's uid, so a vendor service package the runtime APK
 * declares is still invisible unless the APP declares it too. A vendor core
 * loader that queries it, swallows the NameNotFoundException and then makes one
 * more JNI call takes the whole app down via CheckJNI, from inside closed code.
 *
 * This lets a plug-in ask BEFORE it hands control to that loader. Every JNI step
 * is exception-checked, and the probe reports "not visible" for any failure —
 * the caller's contract is a clean bool, never a pending exception.
 */
static bool
plugin_host_android_package_is_visible(const char *package_name)
{
	if (package_name == NULL) {
		return false;
	}
	JavaVM *vm = (JavaVM *)android_globals_get_vm();
	// The APP's Context is the one whose visibility decides this; the
	// class-host Context is deliberately NOT used (it runs under the same uid
	// but exists only for class loading, and using it here would suggest
	// otherwise).
	jobject ctx = (jobject)plugin_host_get_android_activity();
	if (vm == NULL || ctx == NULL) {
		return false;
	}

	JNIEnv *env = NULL;
	bool attached = false;
	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
		if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
			return false;
		}
		attached = true;
	}
	if (env == NULL) {
		return false;
	}
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
	}

	bool visible = false;
	jclass ctx_cls = (*env)->GetObjectClass(env, ctx);
	if (ctx_cls != NULL) {
		jmethodID get_pm = (*env)->GetMethodID(env, ctx_cls, "getPackageManager",
		                                       "()Landroid/content/pm/PackageManager;");
		jobject pm = (get_pm != NULL) ? (*env)->CallObjectMethod(env, ctx, get_pm) : NULL;
		if ((*env)->ExceptionCheck(env)) {
			(*env)->ExceptionClear(env);
			pm = NULL;
		}
		if (pm != NULL) {
			jclass pm_cls = (*env)->GetObjectClass(env, pm);
			jmethodID get_info =
			    (pm_cls != NULL)
			        ? (*env)->GetMethodID(env, pm_cls, "getPackageInfo",
			                              "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;")
			        : NULL;
			if (get_info != NULL) {
				jstring jpkg = (*env)->NewStringUTF(env, package_name);
				if (jpkg != NULL) {
					jobject info = (*env)->CallObjectMethod(env, pm, get_info, jpkg, 0);
					// NameNotFoundException is the EXPECTED negative answer, not
					// an error: clear it and report false.
					if ((*env)->ExceptionCheck(env)) {
						(*env)->ExceptionClear(env);
						info = NULL;
					}
					visible = (info != NULL);
					if (info != NULL) {
						(*env)->DeleteLocalRef(env, info);
					}
					(*env)->DeleteLocalRef(env, jpkg);
				}
			}
			if (pm_cls != NULL) {
				(*env)->DeleteLocalRef(env, pm_cls);
			}
			(*env)->DeleteLocalRef(env, pm);
		}
		(*env)->DeleteLocalRef(env, ctx_cls);
	}
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
	}
	if (attached) {
		(*vm)->DetachCurrentThread(vm);
	}
	return visible;
}

/*
 * ADR-036 D2 / #1037 — the runtime APK's package name, derived from the
 * directory the runtime `.so` was loaded from
 * (`/data/app/~~<h>==/<pkg>-<h>==/lib/<abi>`). Android package names cannot
 * contain '-', so the path segment up to the first '-' is the package.
 * Remembered once from discover_active_plugin()'s search root.
 */
static char g_runtime_pkg[256] = {0};
static void *g_class_host_ctx = NULL;
static bool g_class_host_ctx_tried = false;

static void
remember_runtime_package_from_lib_dir(const char *lib_dir)
{
	if (lib_dir == NULL || g_runtime_pkg[0] != '\0') {
		return;
	}
	/* .../<pkg>-<hash>==/lib/<abi> — the segment just before "/lib/". */
	const char *lib = strstr(lib_dir, "/lib/");
	if (lib == NULL) {
		return;
	}
	const char *seg_end = lib;
	const char *seg_start = seg_end;
	while (seg_start > lib_dir && seg_start[-1] != '/') {
		seg_start--;
	}
	size_t n = 0;
	while (seg_start + n < seg_end && seg_start[n] != '-' && n + 1 < sizeof(g_runtime_pkg)) {
		g_runtime_pkg[n] = seg_start[n];
		n++;
	}
	g_runtime_pkg[n] = '\0';
	/* Sanity: a package name always has at least one dot. An
	 * XRT_PLUGIN_SEARCH_PATH override or an unexpected layout lands here
	 * and simply leaves the slot unavailable. */
	if (strchr(g_runtime_pkg, '.') == NULL) {
		g_runtime_pkg[0] = '\0';
		return;
	}
	U_LOG_I("plugin loader: runtime package '%s' (class-host Context source)", g_runtime_pkg);
}

/*
 * Read `Context.getPackageName()` off `ctx`. Returns false on any JNI
 * trouble, leaving `out` untouched.
 */
static bool
context_package_name(JNIEnv *env, jobject ctx, char *out, size_t out_size)
{
	// Under CheckJNI both a NULL object and an already-pending exception turn
	// the GetObjectClass below into a process ABORT rather than an error
	// return, so neither may reach it. Clearing an inherited exception here is
	// deliberate: we did not raise it, we cannot handle it, and carrying it
	// into the next JNI call is what makes an unrelated component the one that
	// dies. (runtime#1079.)
	if (ctx == NULL) {
		return false;
	}
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
	}
	jclass cls = (*env)->GetObjectClass(env, ctx);
	if (cls == NULL) {
		(*env)->ExceptionClear(env);
		return false;
	}
	jmethodID mid = (*env)->GetMethodID(env, cls, "getPackageName", "()Ljava/lang/String;");
	if (mid == NULL) {
		(*env)->ExceptionClear(env);
		(*env)->DeleteLocalRef(env, cls);
		return false;
	}
	jstring name = (jstring)(*env)->CallObjectMethod(env, ctx, mid);
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
		name = NULL;
	}
	(*env)->DeleteLocalRef(env, cls);
	if (name == NULL) {
		return false;
	}
	const char *utf = (*env)->GetStringUTFChars(env, name, NULL);
	bool ok = false;
	if (utf != NULL) {
		snprintf(out, out_size, "%s", utf);
		(*env)->ReleaseStringUTFChars(env, name, utf);
		ok = true;
	}
	(*env)->DeleteLocalRef(env, name);
	return ok;
}

/*
 * Host-iface callback: an Android `Context` whose `getClassLoader()` resolves
 * classes shipped in the RUNTIME's APK, so a vendor plug-in running in the
 * APP's process can load vendor Java glue the app does not ship (ADR-025's
 * requirement, met the ADR-036 D2/D5 way). See the `xrt_plugin.h` docstring
 * for the full contract — in particular that this is for CLASS LOADING ONLY;
 * Activity-typed vendor calls keep using @ref plugin_host_get_android_activity.
 *
 * Implemented with plain framework JNI —
 * `Context.createPackageContext(pkg, CONTEXT_INCLUDE_CODE|CONTEXT_IGNORE_SECURITY)`,
 * the same mechanism `loadClassFromRuntimeApk` (ipc/android/ipc_client_android.cpp)
 * uses to host `org.freedesktop.monado.ipc.Client`. No runtime Java class is
 * involved, which matters because in-process there is no runtime Java at all:
 * only the dlopen'd `.so`.
 *
 * Out-of-process (the service) the runtime package IS this process, so the
 * host Context is returned as-is and no cross-package Context is created.
 *
 * The result is a JNI global ref cached for the process lifetime and owned
 * here; plug-ins must not delete it.
 */
static void *
plugin_host_get_android_class_host_context(void)
{
	if (g_class_host_ctx_tried) {
		return g_class_host_ctx;
	}

	JavaVM *vm = (JavaVM *)android_globals_get_vm();
	jobject host = (jobject)plugin_host_get_android_activity();
	if (vm == NULL || host == NULL || g_runtime_pkg[0] == '\0') {
		g_class_host_ctx_tried = true;
		return NULL;
	}

	JNIEnv *env = NULL;
	bool attached = false;
	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
		if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
			g_class_host_ctx_tried = true;
			return NULL;
		}
		attached = true;
	}

	jobject out = NULL;
	char host_pkg[256] = {0};
	if (context_package_name(env, host, host_pkg, sizeof(host_pkg)) && strcmp(host_pkg, g_runtime_pkg) == 0) {
		/* Out-of-process: the runtime package is this very process. */
		out = (*env)->NewGlobalRef(env, host);
		if (out != NULL) {
			U_LOG_I("plugin loader: class-host Context is this process ('%s').", g_runtime_pkg);
		}
	} else {
		jclass ctx_cls = (*env)->FindClass(env, "android/content/Context");
		if (ctx_cls != NULL) {
			jmethodID mid = (*env)->GetMethodID(env, ctx_cls, "createPackageContext",
			                                    "(Ljava/lang/String;I)Landroid/content/Context;");
			if (mid != NULL) {
				jstring pkg = (*env)->NewStringUTF(env, g_runtime_pkg);
				/* CONTEXT_INCLUDE_CODE (1) | CONTEXT_IGNORE_SECURITY (2) */
				jobject local = (*env)->CallObjectMethod(env, host, mid, pkg, 3);
				if ((*env)->ExceptionCheck(env)) {
					(*env)->ExceptionDescribe(env);
					(*env)->ExceptionClear(env);
					local = NULL;
				}
				if (local != NULL) {
					out = (*env)->NewGlobalRef(env, local);
					(*env)->DeleteLocalRef(env, local);
				}
				if (pkg != NULL) {
					(*env)->DeleteLocalRef(env, pkg);
				}
			} else {
				(*env)->ExceptionClear(env);
			}
			(*env)->DeleteLocalRef(env, ctx_cls);
		} else {
			(*env)->ExceptionClear(env);
		}

		if (out != NULL) {
			U_LOG_W("plugin loader: class-host Context for '%s' created (ADR-036 D2).", g_runtime_pkg);
		} else {
			U_LOG_W("plugin loader: createPackageContext('%s') failed; using app classloader.",
			        g_runtime_pkg);
		}
	}

	if (attached) {
		(*vm)->DetachCurrentThread(vm);
	}

	g_class_host_ctx = (void *)out;
	g_class_host_ctx_tried = true;
	return g_class_host_ctx;
}

/*!
 * Locate the runtime `.so`'s own lib dir via `dladdr` so we can
 * enumerate sibling plug-in `.so`s in the runtime APK's
 * `/data/app/.../lib/<abi>/`. Writes the dirname into `out_dir`.
 * Returns true on success.
 */
static bool
get_runtime_lib_dir(char *out_dir, size_t out_size)
{
	Dl_info info;
	memset(&info, 0, sizeof(info));
	if (dladdr((const void *)&get_runtime_lib_dir, &info) == 0 || info.dli_fname == NULL) {
		return false;
	}

	const char *slash = strrchr(info.dli_fname, '/');
	if (slash == NULL) {
		return false;
	}
	size_t dir_len = (size_t)(slash - info.dli_fname);
	if (dir_len == 0 || dir_len >= out_size) {
		return false;
	}
	memcpy(out_dir, info.dli_fname, dir_len);
	out_dir[dir_len] = '\0';
	return true;
}

/*!
 * Preload sibling `.so` files from the runtime's lib dir by absolute
 * path before the plug-in's `dlopen` triggers DT_NEEDED resolution.
 *
 * Why: the Android linker namespace (`clns-<n>`) where the plug-in
 * is loaded is the *calling app's* namespace — not the runtime APK's.
 * That namespace's `default_library_paths` only includes the calling
 * app's own `/data/app/<app-pkg>/lib/<ABI>/`. So a plug-in shipped in
 * the runtime APK's lib dir, whose DT_NEEDED references vendor .so
 * files in the SAME runtime APK's lib dir, cannot resolve those
 * names — the linker doesn't know to look there.
 *
 * Workaround: explicitly `dlopen` each sibling `.so` by absolute path
 * before the main plug-in load. `dlopen` with an absolute path
 * bypasses path search and loads the library directly into the
 * current namespace's soinfo table. When the linker subsequently
 * resolves the plug-in's DT_NEEDED references, it finds the
 * already-loaded library by soname and skips path search entirely.
 *
 * Skips:
 *   - the runtime DLL itself (already loaded by the OpenXR loader);
 *   - other plug-in candidates (`libdxrp*_*.so`) — those go through
 *     the normal discovery + probe flow;
 *   - conversion providers (`libdxrc*`, ADR-053): they live only in the
 *     runtime APK's `:conversion` process, and the app-side runtime never
 *     maps one (conversion-provider-discovery.md §3.6).
 *
 * Idempotent: dlopen of an already-loaded library returns the
 * existing handle without reloading.
 *
 * Long-term fix: the Android linker exposes `android_create_namespace`
 * + `android_dlopen_ext` to create custom namespaces with explicit
 * `library_search_paths`. That API is NDK-private though
 * (`<android/dlext.h>` flags are public but `android_create_namespace`
 * isn't), so we use the preload approach until a public NDK API
 * surfaces or until the OpenXR loader itself sets up a permissive
 * runtime namespace.
 */
static void
preload_runtime_lib_dir(const char *lib_dir)
{
#define MAX_PRELOAD_CANDIDATES 64
	char *names[MAX_PRELOAD_CANDIDATES];
	int loaded[MAX_PRELOAD_CANDIDATES] = {0};
	char *last_err[MAX_PRELOAD_CANDIDATES] = {0};
	int n_candidates = 0;

	DIR *d = opendir(lib_dir);
	if (d == NULL) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n_candidates < MAX_PRELOAD_CANDIDATES) {
		if (de->d_type != DT_REG) {
			continue;
		}
		const char *name = de->d_name;
		size_t n = strlen(name);
		if (n < 4 || strcmp(name + n - 3, ".so") != 0) {
			continue;
		}
		/* Skip plug-in candidates — they go through the load path. */
		if (strncmp(name, TARGET_PLUGIN_ANDROID_PREFIX_DISPLAY, 7) == 0) {
			continue;
		}
		if (strncmp(name, TARGET_PLUGIN_ANDROID_PREFIX_CONVERSION, 7) == 0) {
			continue;
		}
		if (strncmp(name, TARGET_PLUGIN_ANDROID_PREFIX_INPUT, 7) == 0) {
			continue;
		}
		/* Skip the runtime DLL: already loaded, dlopen of it is
		 * harmless but the log line is noise. */
		if (strncmp(name, "openxr_displayxr.so", 19) == 0 ||
		    strncmp(name, "libopenxr_displayxr.so", 22) == 0) {
			continue;
		}
		names[n_candidates] = strdup(name);
		if (names[n_candidates] == NULL) {
			break;
		}
		n_candidates++;
	}
	closedir(d);

	/* Fixed-point iteration: a vendor .so often has DT_NEEDED entries
	 * pointing at sibling .so files (e.g. CNSDK's libleiaSDK depends on
	 * libblink, which depends on libSNPE + liblicense_utils). readdir
	 * order is filesystem-dependent and may visit a dependent before
	 * its deps, so a single pass would fail to load it. Loop until a
	 * full pass produces no new successful loads. Worst case O(N²)
	 * dlopen attempts for N libs, but N is small (≤20) and dlopen of
	 * an already-loaded soinfo is essentially free. */
	int total_loaded = 0;
	bool progress = true;
	while (progress) {
		progress = false;
		for (int i = 0; i < n_candidates; i++) {
			if (loaded[i]) {
				continue;
			}
			char path[PATH_MAX];
			(void)snprintf(path, sizeof(path), "%s/%s", lib_dir, names[i]);
			void *h = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
			if (h != NULL) {
				U_LOG_I("plugin loader: preloaded %s", names[i]);
				loaded[i] = 1;
				total_loaded++;
				progress = true;
				free(last_err[i]);
				last_err[i] = NULL;
			} else {
				/* Cache for final report; another pass might succeed. */
				const char *err = dlerror();
				free(last_err[i]);
				last_err[i] = err ? strdup(err) : NULL;
			}
		}
	}

	for (int i = 0; i < n_candidates; i++) {
		if (!loaded[i]) {
			U_LOG_W("plugin loader: preload %s failed after fixed-point: %s", names[i],
			        last_err[i] ? last_err[i] : "(no dlerror)");
		}
		free(names[i]);
		free(last_err[i]);
	}
	U_LOG_I("plugin loader: preload complete (%d/%d loaded)", total_loaded, n_candidates);
#undef MAX_PRELOAD_CANDIDATES
}

/*!
 * Android, once per process before the first discovery: bring the runtime
 * APK's sibling vendor `.so` files into the namespace (so a plug-in's
 * DT_NEEDED resolves) and remember the runtime package for the class-host
 * Context (#1037). Same root the shared loader scans: `XRT_PLUGIN_SEARCH_PATH`
 * if set, else the runtime library's own lib dir.
 */
static void
dp_platform_prepare_discovery(void)
{
	static bool done = false;
	if (done) {
		return;
	}
	done = true;

	char root[PATH_MAX] = {0};
	const char *override = getenv("XRT_PLUGIN_SEARCH_PATH");
	if (override != NULL && *override != '\0') {
		snprintf(root, sizeof(root), "%s", override);
	} else if (!get_runtime_lib_dir(root, sizeof(root))) {
		U_LOG_W("plugin loader: dladdr could not locate runtime lib dir — no plug-ins to try.");
		return;
	}
	struct stat st;
	if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
		U_LOG_I("plugin loader: discovery root '%s' absent — no plug-ins to try.", root);
		return;
	}
	preload_runtime_lib_dir(root);
	remember_runtime_package_from_lib_dir(root);
}

#else /* !XRT_OS_WINDOWS && !XRT_OS_ANDROID — macOS / Linux */

#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

//! Nothing to do before discovery on desktop POSIX.
static void
dp_platform_prepare_discovery(void)
{
}

#endif /* platform islands */


/*
 *
 * Load + negotiate + ABI-check + probe (every platform).
 *
 */

/*!
 * The host iface every display processor receives at negotiate. Filled per
 * call; a DP is not allowed to retain it past negotiate (unlike input
 * providers), so a local is fine.
 */
/*!
 * The one options contract's read slot (N3c): a plug-in's per-component
 * settings through the shared store — `DXR_DP_<ID>_<KEY>` in this process's
 * environment, the per-user overlay, the row's `Settings\*` / manifest
 * `settings`. Never the plug-in's own getenv. Lives in the runtime for the
 * process lifetime, so a plug-in may copy the pointer at negotiate.
 */
static bool
dp_host_get_setting(const char *id, const char *key, char *buf, uint32_t buf_size)
{
	return target_component_get_setting(&g_dp_class, id, key, buf, buf_size, NULL);
}

static void
dp_fill_host_iface(struct xrt_plugin_host_iface *host)
{
	memset(host, 0, sizeof(*host));
	host->struct_size = (uint32_t)sizeof(struct xrt_plugin_host_iface);
	host->host_api_version = XRT_PLUGIN_API_VERSION_CURRENT;
	// ADR-047 Amendment 8: a plug-in may ask for the per-monitor claim sweep.
	host->request_display_rescan = target_display_hotplug_plugin_request;
	// ADR-054 decision 7 (N3c): the plug-in's settings come through here.
	host->get_setting = dp_host_get_setting;
#if defined(XRT_OS_ANDROID)
	// Android plug-ins need the host's JavaVM/activity for vendor-SDK init
	// (their own statically-linked aux_android copy is never populated).
	host->get_android_vm = plugin_host_get_android_vm;
	// Activity when in-process; Service Context when out-of-process (CNSDK only
	// needs an android.content.Context to bind the on-device tracking service).
	host->get_android_activity = plugin_host_get_android_activity;
	// #1037: a Context whose classloader is the runtime APK's, so an
	// in-process vendor plug-in can resolve vendor Java glue the app does
	// not ship. Class loading only — see xrt_plugin.h.
	host->get_android_class_host_context = plugin_host_get_android_class_host_context;
	host->android_package_is_visible = plugin_host_android_package_is_visible;
#endif
}

const struct target_component_class *
target_plugin_component_class(void)
{
	return &g_dp_class;
}

int
target_plugin_describe_settings(const char *id, struct target_component_setting_desc *out, int max)
{
	if (id == NULL || id[0] == '\0' || out == NULL || max <= 0) {
		return 0;
	}
	// The manifest's schema first (POSIX), then whatever the binary declares.
	int n = target_component_manifest_settings_schema(&g_dp_class, id, out, max);

	struct target_component_entry *entries = dp_entries_alloc();
	if (entries == NULL) {
		return n;
	}
	const int count = target_component_enumerate(&g_dp_class, entries, TARGET_COMPONENT_MAX_ENTRIES);
	for (int i = 0; i < count; i++) {
		struct target_component_entry *e = &entries[i];
		if ((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0 || strcmp(e->id, id) != 0) {
			continue;
		}
		// Negotiate only (no probe, no instance): a disabled or declining
		// plug-in still declares its settings — that is how one finds the
		// key to enable it. Quiet: this is a lister, not the discovery scan.
		void *module = NULL;
		void *fn = NULL;
		if (target_component_load_binary_ex(&g_dp_class, e, TARGET_COMPONENT_LOAD_FLAG_QUIET, &module, &fn,
		                                    NULL, NULL, 0) != TARGET_COMPONENT_LOAD_OK) {
			break;
		}
		struct xrt_plugin_host_iface host;
		dp_fill_host_iface(&host);
		struct xrt_plugin_iface *iface = NULL;
		uint32_t plugin_version = 0;
		const xrt_result_t xret =
		    ((xrt_plugin_negotiate_fn_t)fn)(XRT_PLUGIN_API_VERSION_CURRENT, &host, &iface, &plugin_version);
		if (target_component_abi_gate(&g_dp_class, e->id, xret, iface, plugin_version, NULL, 0) ==
		        TARGET_COMPONENT_ABI_OK &&
		    xrt_plugin_iface_has_describe_settings(iface)) {
			const struct xrt_component_setting_desc *descs = NULL;
			const uint32_t nd = iface->describe_settings(&descs);
			n = target_component_merge_setting_descs(out, n, max, descs, nd);
		}
		target_component_unload(module);
		break;
	}
	free(entries);
	return n;
}

/*!
 * ADR-054 N4b: copy the provenance verdict the shared load path left on the
 * entry into the plug-in's ADR-045 status record, for `displayxr-cli info` /
 * `selftest --json` and the dashboard.
 */
static void
status_record_provenance(const char *id, const struct target_component_entry *e)
{
	status_lock();
	int i = status_find_or_add_locked(id != NULL ? id : "?");
	if (i >= 0) {
		struct target_plugin_status *s = &g_status[i];
		s->provenance = e->provenance;
		snprintf(s->signer, sizeof(s->signer), "%s", e->signer);
		snprintf(s->provenance_reason, sizeof(s->provenance_reason), "%s", e->provenance_reason);
	}
	status_unlock();
}

/*!
 * Load + negotiate + ABI-check + probe one registered plug-in. Returns the
 * iface (writing the instance to *out_inst and the negotiated ABI version to
 * *out_version) on success, NULL (and unloads the binary) on any failure. No
 * "active plug-in" logging — callers add their own role-specific line, so
 * this can serve both the single-winner discovery and the load-all display
 * probe (#69). Successful loads intentionally leak the module for the
 * process lifetime so the iface's function pointers remain callable.
 *
 * The shared load path does the exists check, the #952 guard, the #328 pin,
 * the #434 preload and the entry-point lookup; the ADR-020 gate classifies
 * negotiate. What is this class's: the ADR-045 status record per attempt
 * (one WARN per process for a binary that keeps failing the same way — the
 * service re-probes on world events), the #461 skew check, the host iface,
 * platform-state capture before probe, and probe itself.
 */
static const struct xrt_plugin_iface *
load_and_probe_one(struct target_component_entry *e,
                   struct xrt_plugin_instance **out_inst,
                   uint32_t *out_version)
{
	*out_inst = NULL;
	if (out_version != NULL) {
		*out_version = 0;
	}

	// Per-attempt reject context, consumed by plugin_note_reject. Reset here
	// so an attempt from the display-claim collection (which never consumes
	// it) cannot leak a stale reason / "declined" mark into discovery.
	g_last_reject_reason[0] = '\0';
	g_last_reject_declined = false;

	const bool first_attempt = status_begin(e->id, e->display_name, e->version, e->probe_order);

#ifdef XRT_OS_WINDOWS
	// #461: warn if the registry-declared version doesn't match the DLL on
	// disk (an installer skipped a locked file). Diagnostic only — the ABI
	// negotiation below remains the actual compatibility gate.
	if (first_attempt) {
		check_registry_dll_version_skew(e);
	}
#endif

	void *module = NULL;
	void *fn = NULL;
	uint32_t os_error = 0;
	char why[160] = {0};
	const enum target_component_load_result lr =
	    target_component_load_binary_ex(&g_dp_class, e, first_attempt ? 0u : TARGET_COMPONENT_LOAD_FLAG_QUIET,
	                                    &module, &fn, &os_error, why, sizeof(why));
	// ADR-054 N4b: whatever happened next, the provenance verdict the load
	// path computed is a fact about the registration worth showing.
	status_record_provenance(e->id, e);
	if (lr != TARGET_COMPONENT_LOAD_OK) {
		enum target_plugin_load_result result = TARGET_PLUGIN_RESULT_LOAD_FAILED;
		const char *recorded = NULL;
		switch (lr) {
		case TARGET_COMPONENT_LOAD_BINARY_MISSING:
			// ADR-045: an orphan registration (Binary deleted, key left
			// behind) is a fact to skip, not an error to repeat.
			result = TARGET_PLUGIN_RESULT_BINARY_MISSING;
			break;
		case TARGET_COMPONENT_LOAD_DEPENDENCY_MISSING:
			// The binary exists, so a library IT imports is missing —
			// typically the vendor platform the plug-in should resolve
			// lazily (ADR-045 rule 1).
			result = TARGET_PLUGIN_RESULT_DEPENDENCY_MISSING;
			break;
		case TARGET_COMPONENT_LOAD_PATH_REFUSED:
			result = TARGET_PLUGIN_RESULT_PATH_REFUSED;
			recorded = "dev/build-tree path refused (#952)";
			break;
		case TARGET_COMPONENT_LOAD_NO_ENTRY_POINT: result = TARGET_PLUGIN_RESULT_NO_ENTRY_POINT; break;
		case TARGET_COMPONENT_LOAD_UNTRUSTED:
			// ADR-054 N4b, enforcing: the reason names the allowlist row.
			result = TARGET_PLUGIN_RESULT_UNTRUSTED;
			recorded = why;
			break;
		case TARGET_COMPONENT_LOAD_FAILED:
		default: result = TARGET_PLUGIN_RESULT_LOAD_FAILED; break;
		}
		const bool changed = status_finish(e->id, result, os_error, recorded);
		if (changed && !first_attempt) {
			// The shared path logged this retry at INFO; a CHANGED outcome
			// on a retry is still news (ADR-045 re-probe rule).
			U_LOG_W("plugin loader:   %s: load outcome changed to %s%s%s", e->id,
			        target_plugin_load_result_str(result), why[0] != '\0' ? " — " : "", why);
		}
		snprintf(g_last_reject_reason, sizeof(g_last_reject_reason), "%s", why);
		return NULL;
	}
	xrt_plugin_negotiate_fn_t negotiate = (xrt_plugin_negotiate_fn_t)fn;

	struct xrt_plugin_host_iface host;
	dp_fill_host_iface(&host);

	struct xrt_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = negotiate(XRT_PLUGIN_API_VERSION_CURRENT, &host, &iface, &plugin_version);

	/* ADR-020 rule 3, classified the same way for every class. The reason
	 * text lands in g_last_reject_reason for the vendor_dp self-test (#1212),
	 * so a rejected vendor plug-in reads as an ABI mismatch rather than a
	 * bare failure. */
	switch (target_component_abi_gate(&g_dp_class, e->id, xret, iface, plugin_version, g_last_reject_reason,
	                                  sizeof(g_last_reject_reason))) {
	case TARGET_COMPONENT_ABI_DECLINED:
		/* A plug-in that says "not my platform" at negotiate is behaving
		 * correctly on a box without that panel — the same shape as a
		 * declined probe, and recorded the same way. */
		g_last_reject_declined = true;
		status_finish(e->id, TARGET_PLUGIN_RESULT_DECLINED, 0, NULL);
		target_component_unload(module);
		return NULL;
	case TARGET_COMPONENT_ABI_MISMATCH:
		status_finish(e->id, TARGET_PLUGIN_RESULT_ABI_MISMATCH, 0, g_last_reject_reason);
		target_component_unload(module);
		return NULL;
	case TARGET_COMPONENT_ABI_NEGOTIATE_FAILED:
		status_finish(e->id, TARGET_PLUGIN_RESULT_NEGOTIATE_FAILED, 0, NULL);
		target_component_unload(module);
		return NULL;
	case TARGET_COMPONENT_ABI_OK: break;
	}

	// ADR-045: ask BEFORE probe(), so a plug-in about to decline can say why.
	status_record_platform_state(e->id, iface);

	if (iface->probe != NULL) {
		xret = iface->probe(out_inst);
		if (xret == XRT_ERROR_PROBER_NOT_SUPPORTED) {
			U_LOG_I("plugin loader:   %s: probe declined (no matching device).", e->id);
			/* #1212: a plug-in that LOADED and then said "not my
			 * hardware" is behaving correctly on a box without that
			 * panel. Only a failed LOAD is a misconfiguration, so
			 * mark this so the vendor_dp self-test does not fail a
			 * dev box that merely has a vendor plug-in registered. */
			g_last_reject_declined = true;
			status_finish(e->id, TARGET_PLUGIN_RESULT_DECLINED, 0, NULL);
			target_component_unload(module);
			return NULL;
		}
		if (xret != XRT_SUCCESS) {
			bool changed = status_finish(e->id, TARGET_PLUGIN_RESULT_PROBE_FAILED, (uint32_t)xret, NULL);
			PLUGIN_LOG_OUTCOME(changed, "plugin loader:   %s: probe returned %d — skipping.", e->id,
			                   (int)xret);
			target_component_unload(module);
			return NULL;
		}
	}

	if (out_version != NULL) {
		*out_version = plugin_version;
	}
	status_finish(e->id, TARGET_PLUGIN_RESULT_CLAIMED, 0, NULL);

	/* Module handle intentionally leaked: the iface's function pointers
	 * remain reachable into the binary for the process's lifetime. */
	return iface;
}

/*!
 * Try one registered plug-in as the single active winner. Thin wrapper over
 * @ref load_and_probe_one that adds the canonical "active plug-in:" log line
 * (parsed by diagnostics) and the ACTIVE status. Returns the iface on
 * success, NULL on any failure.
 */
static const struct xrt_plugin_iface *
try_load_one(struct target_component_entry *e, struct xrt_plugin_instance **out_inst)
{
	uint32_t plugin_version = 0;
	const struct xrt_plugin_iface *iface = load_and_probe_one(e, out_inst, &plugin_version);
	if (iface == NULL) {
		return NULL;
	}

	U_LOG_W(
	    "plugin loader: active plug-in: id=%s name='%s' vendor='%s' version='%s' "
	    "plugin_api=%u probe_order=%u path=%s",
	    iface->id ? iface->id : e->id, iface->display_name ? iface->display_name : e->display_name,
	    iface->vendor ? iface->vendor : e->vendor, e->version, plugin_version, e->probe_order, e->binary_path);
	status_set_active(e->id);

	return iface;
}


/*
 *
 * Discovery (every platform).
 *
 */

/*!
 * Where a row's `enabled` came from (the input loader's vocabulary, so
 * `dp list` and `input list` read the same): "user" when the shared per-user
 * overlay applied, "machine" / "manifest" when the platform's own store
 * disabled it, else "default".
 */
static const char *
dp_enabled_source(const struct target_component_entry *e)
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
 * A registered-but-disabled plug-in costs one WARN per process per id, not
 * one per scan: `dp_enumerate` runs on every world event the service sees
 * (hotplug rescans, compositor-create refreshes), and a `U_LOG_W` there
 * would be the per-frame-log-bloat class of bug.
 */
static char g_disabled_logged[TARGET_COMPONENT_MAX_ENTRIES][64];
static int g_disabled_logged_count = 0;

static void
dp_log_disabled_once(const struct target_component_entry *e)
{
	bool first = true;
	for (int i = 0; i < g_disabled_logged_count; i++) {
		if (strcmp(g_disabled_logged[i], e->id) == 0) {
			first = false;
			break;
		}
	}
	if (first && g_disabled_logged_count < TARGET_COMPONENT_MAX_ENTRIES) {
		snprintf(g_disabled_logged[g_disabled_logged_count], sizeof(g_disabled_logged[0]), "%s", e->id);
		g_disabled_logged_count++;
	}
	PLUGIN_LOG_OUTCOME(first, "plugin loader:   %s disabled (Enabled=0, %s; ProbeOrder=%u) — never tried.", e->id,
	                   dp_enabled_source(e), e->probe_order);
}

/*!
 * Enumerate this class's registrations through the shared loader. Returns the
 * count (0 = nothing registered, or no discovery root at all). Malformed rows
 * come back flagged; the callers below skip them (a row the loader could never
 * try is not a candidate) and say so once.
 *
 * `Enabled` (N3b, ADR-054 verb `enable|disable`; plugin-discovery.md §2 /
 * §2.1 / §3.1) is honoured here for EVERY scan — the single-winner discovery,
 * the display-claim collection, the hotplug rescan and `dp list` — through
 * the shared per-user overlay (HKCU over HKLM, `Policy\Lock` aside; a no-op
 * off Windows, where the manifest's `enabled` is the switch) and
 * @ref dp_entry_usable. A disabled plug-in is logged once per process per id
 * (WARN the first time, INFO after: the service re-enumerates on world
 * events) and is never loaded, so a vendor DLL the user turned off never
 * enters the process.
 */
static int
dp_enumerate(struct target_component_entry *entries, bool loud)
{
	dp_platform_prepare_discovery();
	const int n = target_component_enumerate(&g_dp_class, entries, TARGET_COMPONENT_MAX_ENTRIES);
	(void)target_component_apply_user_overlay(&g_dp_class, entries, (size_t)n);
	for (int i = 0; i < n; i++) {
		if ((entries[i].flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0) {
			PLUGIN_LOG_OUTCOME(loud, "plugin loader:   %s: %s — skipping.", entries[i].source,
			                   entries[i].reason);
		} else if (!entries[i].enabled) {
			dp_log_disabled_once(&entries[i]);
		}
	}
	return n;
}

//! A row every scan may try: well-formed AND enabled (N3b).
static bool
dp_entry_usable(const struct target_component_entry *e)
{
	return (e->flags & TARGET_COMPONENT_FLAG_MALFORMED) == 0 && e->enabled;
}

static const struct xrt_plugin_iface *
discover_active_plugin(struct xrt_plugin_instance **out_inst, uint32_t max_probe_order)
{
	*out_inst = NULL;

	struct target_component_entry *entries = dp_entries_alloc();
	if (entries == NULL) {
		return NULL;
	}
	// First-call path (max == UINT32_MAX) is the loud one; the refresh path
	// re-enumerates on every compositor create and keeps quiet (#342).
	const bool first_call = max_probe_order == 0xFFFFFFFFu;
	const int n = dp_enumerate(entries, first_call);
	if (n == 0) {
		U_LOG_I("plugin loader: no registered plug-ins — no plug-ins to try.");
		free(entries);
		return NULL;
	}

	// DXR_PLUGIN_EXCLUSIVE (#1545): warn once if the pin matches nothing.
	{
		const char *ids[TARGET_COMPONENT_MAX_ENTRIES];
		for (int i = 0; i < n; i++) {
			ids[i] = entries[i].id;
		}
		plugin_warn_exclusive_miss(ids, n);
	}

	// N3b: the skipped-disabled summary describes the first-call pass; a
	// refresh re-walks the same rows and must not double-count them.
	if (first_call) {
		g_skipped_disabled_count = 0;
	}

	// PreferredPlugin override (#378): try the user-pinned plug-in before
	// the ProbeOrder sort. A stale or failed preference falls through to
	// the normal order, so a bad value can never brick discovery.
	char preferred[64];
	if (target_plugin_get_preferred(preferred, sizeof(preferred))) {
		for (int i = 0; i < n; i++) {
			if (strcmp(entries[i].id, preferred) != 0) {
				continue;
			}
			if ((entries[i].flags & TARGET_COMPONENT_FLAG_MALFORMED) == 0 && !entries[i].enabled) {
				// N3b: `Enabled=0` is the stronger verb — a disabled
				// preference falls through to ProbeOrder, said ONCE
				// (the refresh path never reaches here: a set
				// preference short-circuits target_plugin_refresh_active).
				PLUGIN_LOG_OUTCOME(first_call,
				                   "plugin loader: PreferredPlugin '%s' is disabled (Enabled=0, %s) — "
				                   "falling through to ProbeOrder.",
				                   preferred, dp_enabled_source(&entries[i]));
				if (first_call) {
					plugin_note_skipped_disabled(entries[i].id);
					status_begin(entries[i].id, entries[i].display_name, entries[i].version,
					             entries[i].probe_order);
					(void)status_finish(entries[i].id, TARGET_PLUGIN_RESULT_DISABLED, 0,
					                    entries[i].reason);
				}
				break;
			}
			if (!dp_entry_usable(&entries[i])) {
				break;
			}
			// An exclusive pin outranks the user preference: the
			// point is that nothing else is LOADED, and honoring a
			// preference here would load exactly the DLL the lane
			// asked to keep out of the process (#1545).
			if (plugin_id_excluded(entries[i].id)) {
				break;
			}
			// Refresh path: only honor the preference if it is also
			// strictly-better; the sticky-preference guard in
			// target_plugin_refresh_active normally prevents reaching
			// here on a refresh at all.
			if (entries[i].probe_order >= max_probe_order) {
				break;
			}
			U_LOG_W("plugin loader: PreferredPlugin override — attempting id='%s' first.", preferred);
			const struct xrt_plugin_iface *iface = try_load_one(&entries[i], out_inst);
			if (iface != NULL) {
				g_active_probe_order = entries[i].probe_order;
				snprintf(g_active_row_id, sizeof(g_active_row_id), "%s", entries[i].id);
				free(entries);
				return iface;
			}
			U_LOG_W("plugin loader: preferred plug-in '%s' failed — falling back to ProbeOrder.",
			        preferred);
			break;
		}
	}

	U_LOG_I("plugin loader: %d registered plug-in(s); attempting in ProbeOrder ascending.", n);
	for (int i = 0; i < n; i++) {
		// Refresh path (#342) passes the active plug-in's ProbeOrder as
		// max so we only re-attempt strictly-better candidates and never
		// re-probe the already-active one. First-call path passes
		// UINT32_MAX, so no entry is skipped.
		if (entries[i].probe_order >= max_probe_order) {
			continue;
		}
		if ((entries[i].flags & TARGET_COMPONENT_FLAG_MALFORMED) == 0 && !entries[i].enabled) {
			// N3b: entries are in ProbeOrder, so a disabled one met
			// before the winner out-ranks it — the summary names it,
			// and its ADR-045 record reads DISABLED rather than the
			// NOT_ATTEMPTED a lister would otherwise infer. Logged by
			// dp_enumerate (once per process); never a reject.
			if (first_call) {
				plugin_note_skipped_disabled(entries[i].id);
				status_begin(entries[i].id, entries[i].display_name, entries[i].version,
				             entries[i].probe_order);
				(void)status_finish(entries[i].id, TARGET_PLUGIN_RESULT_DISABLED, 0, entries[i].reason);
			}
			continue;
		}
		if (!dp_entry_usable(&entries[i])) {
			continue;
		}
		/* #1545: skipped BEFORE the load, and deliberately without
		 * plugin_note_reject — an excluded plug-in is not a failure. */
		if (plugin_id_excluded(entries[i].id)) {
			U_LOG_I("plugin loader:   [%d/%d] %s skipped (DXR_PLUGIN_EXCLUSIVE).", i + 1, n, entries[i].id);
			continue;
		}
		U_LOG_I("plugin loader:   [%d/%d] %s (ProbeOrder=%u, %s)", i + 1, n, entries[i].id,
		        entries[i].probe_order, entries[i].binary_path);
		const struct xrt_plugin_iface *iface = try_load_one(&entries[i], out_inst);
		if (iface != NULL) {
			g_active_probe_order = entries[i].probe_order;
			snprintf(g_active_row_id, sizeof(g_active_row_id), "%s", entries[i].id);
			free(entries);
			return iface;
		}
		/* #1212: attempted in ascending ProbeOrder and failed, so this is
		 * by construction a better-ranked plug-in than whatever wins
		 * below. The self-test's vendor_dp check keys off this. */
		plugin_note_reject(entries[i].id, entries[i].probe_order);
	}
	free(entries);

	// A refresh (max_probe_order bounded by the current winner) legitimately
	// attempts nothing when nothing better is registered — that is not a
	// fallback, so do not shout (monkey-test F2: 48 false alarms per soak).
	if (first_call) {
		U_LOG_W("plugin loader: no registered plug-in claimed the system — falling back to static drivers.");
	}
	return NULL;
}

#if !defined(XRT_OS_ANDROID)
/* Android has one plug-in and no per-monitor claim sources (the resolve path
 * below never collects there), so the two collectors would be unused code. */

/*!
 * Load EVERY registered plug-in and return them as display-claim sources for
 * the per-monitor registry (#69 / ADR-015, multi-screen M0). Unlike
 * @ref discover_active_plugin (which stops at the first winner), this consults
 * all of them so the fallback (sim_display) and a vendor plug-in can both
 * contribute claims. The already-active plug-in is reused — not loaded twice —
 * and the remainder are loaded via @ref load_and_probe_one (modules leaked,
 * consistent with the single-winner path). Plug-ins whose `probe()` declines
 * contribute nothing. `DXR_PLUGIN_EXCLUSIVE` keeps every other plug-in out of
 * the process exactly as it does for discovery. Returns the source count,
 * sorted ascending by ProbeOrder (so a confidence tie resolves to the
 * lower-ProbeOrder plug-in).
 *
 * Caller must hold @ref g_refresh_mutex and have already loaded the active
 * plug-in (so `g_active_*` are set for the reuse path).
 */
static int
collect_display_sources_platform(struct plugin_display_source *out, int max)
{
	struct target_component_entry *entries = dp_entries_alloc();
	if (entries == NULL) {
		return 0;
	}
	const int n = dp_enumerate(entries, false);

	const char *active_id = (g_active_iface != NULL && g_active_iface->id != NULL) ? g_active_iface->id : NULL;
	bool active_seen = false;

	int count = 0;
	for (int i = 0; i < n && count < max; i++) {
		if (!dp_entry_usable(&entries[i])) {
			continue;
		}
		// #1545: THE load site this variable exists for. Claim
		// collection consults every registered plug-in, so without this
		// an exclusive lane would still load the vendor DLL (and its GL
		// ICD) even though it never becomes the active DP.
		if (plugin_id_excluded(entries[i].id)) {
			continue;
		}
		// Reuse the already-loaded active plug-in rather than loading a
		// second instance of it.
		if (active_id != NULL && strcmp(entries[i].id, active_id) == 0) {
			out[count].iface = g_active_iface;
			out[count].inst = g_active_instance;
			out[count].probe_order = g_active_probe_order;
			count++;
			active_seen = true;
			continue;
		}

		uint32_t ver = 0;
		struct xrt_plugin_instance *inst = NULL;
		const struct xrt_plugin_iface *iface = load_and_probe_one(&entries[i], &inst, &ver);
		if (iface == NULL) {
			continue; // declined / failed → contributes no claims
		}
		U_LOG_I("plugin loader: display-claim source id=%s (ProbeOrder=%u)", entries[i].id,
		        entries[i].probe_order);
		out[count].iface = iface;
		out[count].inst = inst;
		out[count].probe_order = entries[i].probe_order;
		count++;
	}
	free(entries);

	// An active plug-in that no registration names any more (removed after
	// it loaded) is still the one devices come from: keep it as a source.
	if (!active_seen && g_active_iface != NULL && count < max) {
		out[count].iface = g_active_iface;
		out[count].inst = g_active_instance;
		out[count].probe_order = g_active_probe_order;
		count++;
	}
	return count;
}

/*!
 * Display hotplug (ADR-047 Amendment 8): add to @p out (holding @p count
 * sources) every registered plug-in that is not a display-claim source yet — a
 * vendor plug-in that declined at instance create because its panel was not
 * attached, or one registered since. Sources already in the set are kept as
 * they are and never re-probed or destroyed: a live segment DP may have been
 * made from their instance. The set stays in ascending ProbeOrder (rule 3's
 * hotplug carve-out reads "ranked before the active plug-in" off that order).
 * `DXR_PLUGIN_EXCLUSIVE` is honoured exactly as in discovery.
 *
 * Caller holds @ref g_refresh_mutex. Returns the new count. Platform-neutral
 * now; which platforms call it is decided in
 * @ref target_plugin_rescan_claim_sources.
 */
static int
rescan_display_sources_platform(struct plugin_display_source *out, int count, int max)
{
	struct target_component_entry *entries = dp_entries_alloc();
	if (entries == NULL) {
		return count;
	}
	const int n = dp_enumerate(entries, false);

	for (int i = 0; i < n && count < max; i++) {
		if (!dp_entry_usable(&entries[i])) {
			continue;
		}
		if (plugin_id_excluded(entries[i].id)) {
			continue;
		}
		bool have = false;
		for (int k = 0; k < count && !have; k++) {
			const struct xrt_plugin_iface *sif = out[k].iface;
			have = sif != NULL && sif->id != NULL && strcmp(sif->id, entries[i].id) == 0;
		}
		if (have) {
			continue;
		}

		uint32_t ver = 0;
		struct xrt_plugin_instance *inst = NULL;
		const struct xrt_plugin_iface *iface = load_and_probe_one(&entries[i], &inst, &ver);
		if (iface == NULL) {
			continue; // still declines (no panel yet) / failed → no claims
		}
		U_LOG_W(
		    "plugin loader: display hotplug — '%s' (ProbeOrder=%u) now loads as a display-claim source; it was "
		    "not one before (declined, failed, or registered since)",
		    entries[i].id, entries[i].probe_order);

		// Insert in ascending ProbeOrder (after any equal one).
		int at = count;
		while (at > 0 && out[at - 1].probe_order > entries[i].probe_order) {
			out[at] = out[at - 1];
			at--;
		}
		out[at].iface = iface;
		out[at].inst = inst;
		out[at].probe_order = entries[i].probe_order;
		count++;
	}
	free(entries);
	return count;
}

#endif /* !XRT_OS_ANDROID — display-claim sources */


/*
 *
 * Public enumeration (every platform).
 *
 */

int
target_plugin_enumerate(struct target_plugin_desc *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}

	struct target_component_entry *entries = dp_entries_alloc();
	if (entries == NULL) {
		return 0;
	}
	const int n = dp_enumerate(entries, false);

	int count = 0;
	for (int i = 0; i < n && count < max; i++) {
		if ((entries[i].flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0) {
			continue; // a row the loader could never try is not a choice to list
		}
		// A DISABLED row IS listed (N3b): `dp list` must show what `dp
		// disable` did, and `dp enable` must find it.
		struct target_plugin_desc *d = &out[count++];
		memset(d, 0, sizeof(*d));
		snprintf(d->id, sizeof(d->id), "%s", entries[i].id);
		snprintf(d->display_name, sizeof(d->display_name), "%s", entries[i].display_name);
		snprintf(d->vendor, sizeof(d->vendor), "%s", entries[i].vendor);
		snprintf(d->version, sizeof(d->version), "%s", entries[i].version);
		snprintf(d->binary_path, sizeof(d->binary_path), "%s", entries[i].binary_path);
		d->probe_order = entries[i].probe_order;
		d->enabled = entries[i].enabled;
		snprintf(d->enabled_source, sizeof(d->enabled_source), "%s", dp_enabled_source(&entries[i]));
		// ADR-054 N4b: the load path classifies what it loads; a lister
		// asks explicitly (thread-safe: the service's status snapshot reaches
		// this from IPC threads; it re-hashes, but the signature check is
		// cached per file identity + digest).
		(void)target_component_check_provenance(&g_dp_class, &entries[i]);
		d->provenance = entries[i].provenance;
		snprintf(d->provenance_reason, sizeof(d->provenance_reason), "%s", entries[i].provenance_reason);
		snprintf(d->signer, sizeof(d->signer), "%s", entries[i].signer);
	}
	free(entries);
	return count;
}


/*
 *
 * The PreferredPlugin store — the one per-platform public surface left.
 *
 */

#ifdef XRT_OS_WINDOWS

bool
target_plugin_get_preferred(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';

	// N3b: HKCU first (per user, `dp use <id>` without admin), then the HKLM
	// 64-bit view (`dp use --machine`). Only this value and the per-row
	// `Enabled` are ever read from HKCU — the rows stay HKLM-only, so a
	// per-user key can never inject a DLL (plugin-discovery.md §2.1).
	wchar_t wbuf[64];
	DWORD wbuf_bytes = sizeof(wbuf);
	LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, L"Software\\DisplayXR\\DisplayProcessors", L"PreferredPlugin",
	                          RRF_RT_REG_SZ, NULL, wbuf, &wbuf_bytes);
	if (rc != ERROR_SUCCESS || wbuf[0] == L'\0') {
		wbuf_bytes = sizeof(wbuf);
		rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\DisplayProcessors", L"PreferredPlugin",
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

xrt_result_t
target_plugin_set_preferred(const char *id)
{
	if (id == NULL || id[0] == '\0') {
		return target_plugin_clear_preferred();
	}

	wchar_t wid[64];
	if (MultiByteToWideChar(CP_UTF8, 0, id, -1, wid, (int)(sizeof(wid) / sizeof(wid[0]))) <= 0) {
		return XRT_ERROR_IPC_FAILURE;
	}

	// 64-bit view: the runtime reader runs as a 64-bit process (no
	// redirection), so we must write where it reads. Create the root key
	// if a clean machine never installed a DisplayProcessor.
	HKEY root;
	LSTATUS rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\DisplayProcessors", 0, NULL, 0,
	                             KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &root, NULL);
	if (rc != ERROR_SUCCESS) {
		return rc == ERROR_ACCESS_DENIED ? XRT_ERROR_NOT_AUTHORIZED : XRT_ERROR_IPC_FAILURE;
	}

	DWORD bytes = (DWORD)((wcslen(wid) + 1) * sizeof(wchar_t));
	rc = RegSetValueExW(root, L"PreferredPlugin", 0, REG_SZ, (const BYTE *)wid, bytes);
	RegCloseKey(root);
	if (rc != ERROR_SUCCESS) {
		return rc == ERROR_ACCESS_DENIED ? XRT_ERROR_NOT_AUTHORIZED : XRT_ERROR_IPC_FAILURE;
	}
	return XRT_SUCCESS;
}

xrt_result_t
target_plugin_clear_preferred(void)
{
	HKEY root;
	LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\DisplayProcessors", 0,
	                           KEY_SET_VALUE | KEY_WOW64_64KEY, &root);
	if (rc == ERROR_FILE_NOT_FOUND) {
		return XRT_SUCCESS; // no root key → nothing to clear
	}
	if (rc != ERROR_SUCCESS) {
		return rc == ERROR_ACCESS_DENIED ? XRT_ERROR_NOT_AUTHORIZED : XRT_ERROR_IPC_FAILURE;
	}

	rc = RegDeleteValueW(root, L"PreferredPlugin");
	RegCloseKey(root);
	if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
		return rc == ERROR_ACCESS_DENIED ? XRT_ERROR_NOT_AUTHORIZED : XRT_ERROR_IPC_FAILURE;
	}
	return XRT_SUCCESS;
}

#elif defined(XRT_OS_ANDROID)

/*
 * Android discovery is convention-driven (filename ProbeOrder) and the
 * diagnostic CLI / Dashboard don't ship there in v1, so the writable override
 * is a stub; the read honors the env override so a dev/emulator can still
 * pin a plug-in.
 */

bool
target_plugin_get_preferred(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	const char *env = getenv("XRT_PREFERRED_PLUGIN_ID");
	if (env != NULL && *env != '\0') {
		snprintf(out, cap, "%s", env);
		return out[0] != '\0';
	}
	return false;
}

xrt_result_t
target_plugin_set_preferred(const char *id)
{
	(void)id;
	return XRT_ERROR_NOT_IMPLEMENTED;
}

xrt_result_t
target_plugin_clear_preferred(void)
{
	return XRT_ERROR_NOT_IMPLEMENTED;
}

#else /* macOS / Linux: env override, else a `preferred` file in the per-user dir */

bool
target_plugin_get_preferred(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';

	/* Env var wins — a dev/CI override that needs no writable filesystem. */
	const char *env = getenv("XRT_PREFERRED_PLUGIN_ID");
	if (env != NULL && *env != '\0') {
		snprintf(out, cap, "%s", env);
		return out[0] != '\0';
	}

	char dir[PATH_MAX];
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return false;
	}
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/preferred", dir);

	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	char buf[64] = {0};
	size_t got = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[got] = '\0';
	size_t len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ' ||
	                   buf[len - 1] == '\t')) {
		buf[--len] = '\0';
	}
	snprintf(out, cap, "%s", buf);
	return out[0] != '\0';
}

xrt_result_t
target_plugin_set_preferred(const char *id)
{
	if (id == NULL || id[0] == '\0') {
		return target_plugin_clear_preferred();
	}

	char dir[PATH_MAX];
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return XRT_ERROR_IPC_FAILURE;
	}
	/* Best-effort: create the manifest dir if absent (single level). */
	(void)mkdir(dir, 0755);

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/preferred", dir);
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		return errno == EACCES ? XRT_ERROR_NOT_AUTHORIZED : XRT_ERROR_IPC_FAILURE;
	}
	fprintf(f, "%s\n", id);
	fclose(f);
	return XRT_SUCCESS;
}

xrt_result_t
target_plugin_clear_preferred(void)
{
	char dir[PATH_MAX];
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return XRT_SUCCESS; /* nowhere it could live → nothing to clear */
	}
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/preferred", dir);
	if (remove(path) != 0 && errno != ENOENT) {
		return errno == EACCES ? XRT_ERROR_NOT_AUTHORIZED : XRT_ERROR_IPC_FAILURE;
	}
	return XRT_SUCCESS;
}

#endif /* PreferredPlugin store */



/*
 *
 * Public iface.
 *
 */

const struct xrt_plugin_iface *
target_plugin_get_active(void)
{
	if (g_load_attempted) {
		return g_active_iface;
	}
	g_load_attempted = 1;

	// Init the refresh mutex here, while we're still single-threaded
	// (xrCreateInstance is per the OpenXR spec). Subsequent compositor-
	// create-time refreshes can then lock it from multiple IPC clients.
	if (!g_refresh_mutex_initialized) {
		if (os_mutex_init(&g_refresh_mutex) == 0) {
			g_refresh_mutex_initialized = 1;
		} else {
			U_LOG_W("plugin loader: os_mutex_init failed — refresh path will skip locking.");
		}
	}
	// ADR-045 status records are read from diagnostics threads (service tray).
	if (!g_status_mutex_initialized && os_mutex_init(&g_status_mutex) == 0) {
		g_status_mutex_initialized = 1;
	}

	g_active_iface = discover_active_plugin(&g_active_instance, 0xFFFFFFFFu /* try all */);
	return g_active_iface;
}

struct xrt_plugin_instance *
target_plugin_get_active_instance(void)
{
	/*
	 * Force a discovery pass if the caller hits this before
	 * target_plugin_get_active(). Order-independent at the cost of
	 * one extra branch.
	 */
	(void)target_plugin_get_active();
	return g_active_instance;
}

const struct xrt_plugin_iface *
target_plugin_refresh_active(void)
{
	// Force the one-shot discovery (also inits the mutex) if the caller
	// hits this before the runtime's xrCreateInstance — defensive only;
	// in practice instance-create always precedes session-create.
	(void)target_plugin_get_active();

	// PreferredPlugin override (#378): a pinned preference is sticky. The
	// #342 refresh only ever adopts a strictly-better (lower ProbeOrder)
	// plug-in, which would silently undo a preference for a higher-order
	// plug-in (e.g. pinning sim-display=200 while leia-sr=50 is present).
	// Switching the preference is a deliberate act that takes effect on the
	// next process, not via a mid-session re-scan — so when a preference is
	// set, never auto-adopt.
	char preferred[64];
	if (target_plugin_get_preferred(preferred, sizeof(preferred))) {
		return g_active_iface;
	}

	// ADR-045 D3 — no live swap. Re-selection exists to get OFF the fallback
	// (a vendor plug-in registered, or its platform came up, after this
	// process selected the fallback). A non-fallback active plug-in is kept
	// even when it reports NO_DISPLAY: diagnostics surface that state and its
	// DP passes pixels through, and nothing is re-probed here.
	if (g_active_iface != NULL && !target_plugin_iface_is_fallback(g_active_iface)) {
		return g_active_iface;
	}

	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}

	uint32_t prev_order = g_active_probe_order;
	struct xrt_plugin_instance *new_inst = NULL;
	const struct xrt_plugin_iface *cand = discover_active_plugin(&new_inst, prev_order);
	if (cand != NULL) {
		// `g_active_probe_order` was already updated by discover to the
		// new (lower) winner. The previous plug-in's DLL is intentionally
		// leaked — its vtable may still be reachable from existing live
		// DPs the compositor hasn't recreated yet.
		U_LOG_W("plugin loader: refresh adopted better plug-in id=%s (ProbeOrder %u → %u) — "
		        "re-scan after mid-install (#342).",
		        cand->id ? cand->id : "?", prev_order, g_active_probe_order);
		g_active_iface = cand;
		g_active_instance = new_inst;
		// Invalidate the display-claim source cache so the next
		// target_plugin_resolve_displays rebuilds it against the new winner.
		g_display_source_count = -1;
		g_display_sources_active_only = false;
	}

	const struct xrt_plugin_iface *result = g_active_iface;

	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
	}
	return result;
}


/*
 *
 * Per-display resolution (issue #69 / ADR-015).
 *
 */

/*!
 * Monitor id (also the `displayId` of `XR_DXR_display_info` v22). FNV-1a-64.
 *
 * Where the platform names the connector (desktop Linux: the DRM connector
 * WITH its card prefix, e.g. "card1-HDMI-A-1", else the RandR output name) the
 * id hashes the EDID identity
 * — manufacturer, product, serial — plus that connector, and NOT the desktop
 * position: the connector is unique per machine, so the id survives the user
 * rearranging monitors and the service rebuilding its registry on the next
 * client connect, and still separates two identical panels (different ports).
 * It changes only when a different panel is plugged into that port, the
 * panel moves to another port, or the kernel renumbers the card (a GPU added
 * or removed, a different driver probe order across boots) (multi-screen M1).
 *
 * With no connector name (Windows today) the id stays what it has always
 * been — manufacturer + product + screen position, position being the only
 * thing that separates two identical panels there — so Windows ids are
 * unchanged, and a rearrangement there does change them (documented in
 * XR_DXR_display_info v22).
 */
static uint64_t
monitor_id_from_edid(
    uint16_t mfr, uint16_t product, uint32_t serial, int32_t left, int32_t top, const char *platform_key)
{
	uint64_t h = 1469598103934665603ULL; /* FNV-1a-64 offset basis */
	const uint64_t prime = 1099511628211ULL;
	const bool have_key = platform_key != NULL && platform_key[0] != '\0';
	uint8_t bytes[12];
	size_t n = 0;
	memcpy(&bytes[0], &mfr, sizeof(mfr));
	memcpy(&bytes[2], &product, sizeof(product));
	if (have_key) {
		memcpy(&bytes[4], &serial, sizeof(serial));
		n = 8;
	} else {
		memcpy(&bytes[4], &left, sizeof(left));
		memcpy(&bytes[8], &top, sizeof(top));
		n = 12;
	}
	for (size_t i = 0; i < n; i++) {
		h ^= (uint64_t)bytes[i];
		h *= prime;
	}
	// The connector key exactly as the platform records it — on desktop
	// Linux with its DRM card prefix ("card1-HDMI-A-1"), so two GPUs that both
	// expose an "HDMI-A-1" still yield different ids.
	for (const char *c = platform_key; have_key && *c != '\0'; c++) {
		h ^= (uint64_t)(uint8_t)*c;
		h *= prime;
	}
	return h;
}

/*!
 * Runtime-private side table: the full @ref os_display_edid_monitor behind
 * each descriptor the last @ref target_plugin_build_descriptors produced, keyed
 * by monitor id. The descriptor is plug-in ABI and stays as it is; this is
 * where the connector name, mm and device mode live for the runtime's own
 * use (the back-compat claim below). Guarded by @ref g_refresh_mutex.
 */
static struct
{
	uint64_t monitor_id;
	struct os_display_edid_monitor mon;
	struct xrt_display_descriptor desc;
	//! Stable screen key (target_screen_keys_build_ex), the per-screen preference's identity.
	char key[TARGET_SCREEN_KEY_MAX];
	//! The EDID-form key (pre-#2250); equals @ref key unless a vendor serial keys the monitor.
	char legacy_key[TARGET_SCREEN_KEY_MAX];
	//! Sanitised vendor serial from a claim (target_screen_key_serial_pick), "" = none; kept across re-probes.
	char vendor_serial[TARGET_SCREEN_KEY_SERIAL_MAX + 1];
	//! OS device name the key was qualified with (Windows GDI name, else output / connector).
	char device_name[64];
	//! Every plug-in that claimed this monitor in the last resolve, in source order (dashboard).
	struct target_plugin_screen_candidate cands[TARGET_PLUGIN_MAX_SOURCES];
	uint32_t cand_count;
	//! @ref cands is from a resolve over every loadable plug-in (not the POSIX active-only shortcut).
	bool cands_valid;
	//! When this monitor's EDID was last actually read (os_monotonic_get_ns), ADR-051 D6.
	uint64_t edid_read_ns;
	//! @ref mon's EDID identity is carried forward from an earlier read (target_plugin_edid_carry).
	bool edid_carried;
} g_monitor_side[XRT_DP_REGISTRY_MAX_ENTRIES];
static uint32_t g_monitor_side_count = 0;

/*!
 * (Re)derive every side-table monitor's screen key and EDID-form key from its
 * EDID identity, device name and remembered vendor serial (#2250). Caller
 * holds @ref g_refresh_mutex.
 */
static void
monitor_side_rebuild_keys(void)
{
	struct target_screen_key_input key_in[XRT_DP_REGISTRY_MAX_ENTRIES];
	char keys[XRT_DP_REGISTRY_MAX_ENTRIES][TARGET_SCREEN_KEY_MAX];
	char legacy[XRT_DP_REGISTRY_MAX_ENTRIES][TARGET_SCREEN_KEY_MAX];
	const uint32_t n = g_monitor_side_count;
	for (uint32_t i = 0; i < n; i++) {
		key_in[i].manufacturer_id = g_monitor_side[i].mon.manufacturer_id;
		key_in[i].product_id = g_monitor_side[i].mon.product_id;
		key_in[i].serial = g_monitor_side[i].mon.serial_number;
		key_in[i].device_name = g_monitor_side[i].device_name;
		key_in[i].vendor_serial = g_monitor_side[i].vendor_serial;
	}
	target_screen_keys_build_ex(key_in, n, keys, legacy);
	for (uint32_t i = 0; i < n; i++) {
		(void)snprintf(g_monitor_side[i].key, sizeof(g_monitor_side[i].key), "%s", keys[i]);
		(void)snprintf(g_monitor_side[i].legacy_key, sizeof(g_monitor_side[i].legacy_key), "%s", legacy[i]);
	}
}

/*!
 * What the ACTIVE plug-in said about its panel (`get_display_info`), noted by
 * @ref target_plugin_note_active_panel. Steers the active plug-in's
 * back-compat claim off-Windows. Guarded by @ref g_refresh_mutex.
 */
static struct os_display_panel_hint g_active_panel_hint;
static bool g_active_panel_valid = false;

void
target_plugin_note_active_panel(const struct xrt_plugin_display_info *pdi)
{
	if (pdi == NULL) {
		return;
	}
	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}
	g_active_panel_hint.screen_left = pdi->display_screen_left;
	g_active_panel_hint.screen_top = pdi->display_screen_top;
	g_active_panel_hint.pixel_width = pdi->display_pixel_width;
	g_active_panel_hint.pixel_height = pdi->display_pixel_height;
	g_active_panel_hint.width_m = pdi->display_width_m;
	g_active_panel_hint.height_m = pdi->display_height_m;
	g_active_panel_valid = pdi->display_pixel_width > 0 && pdi->display_pixel_height > 0;
	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
	}
}

/*!
 * Log how each monitor was identified (desktop Linux: which RandR <-> DRM
 * join rule fired), at INFO and only when the set changes, so a service that
 * re-enumerates on every client connect logs it once. Silent when no record
 * carries platform identity (Windows), whose enumerator has its own logs.
 */
static void
log_monitor_join_once(const struct os_display_edid_list *list)
{
	static uint64_t s_last = 0;
	uint64_t h = 1469598103934665603ULL;
	bool any = false;
	for (uint32_t i = 0; i < list->count; i++) {
		const struct os_display_edid_monitor *m = &list->monitors[i];
		any = any || m->join != OS_EDID_JOIN_NONE || m->output_name[0] != '\0';
		const uint64_t parts[] = {m->manufacturer_id,
		                          m->product_id,
		                          m->serial_number,
		                          (uint64_t)(uint32_t)m->screen_left,
		                          (uint64_t)(uint32_t)m->screen_top,
		                          m->pixel_width,
		                          (uint64_t)m->join};
		for (size_t k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
			h ^= parts[k];
			h *= 1099511628211ULL;
		}
	}
	h ^= list->count;
	if (!any || h == s_last) {
		return;
	}
	s_last = h;

	for (uint32_t i = 0; i < list->count; i++) {
		const struct os_display_edid_monitor *m = &list->monitors[i];
		U_LOG_I(
		    "plugin loader: monitor %u output='%s' connector='%s' join=%s mfr=0x%04x product=0x%04x "
		    "serial=0x%08x %ux%u%s at (%d,%d)%s %ux%u mm",
		    i, m->output_name, m->connector, os_display_edid_join_str(m->join), m->manufacturer_id,
		    m->product_id, m->serial_number, m->pixel_width, m->pixel_height, m->is_primary ? " primary" : "",
		    (int)m->screen_left, (int)m->screen_top, m->origin_unknown ? " (origin unknown)" : "",
		    m->physical_width_mm, m->physical_height_mm);
	}
}

/*!
 * The OS device name of an enumerated monitor, for its screen key: the GDI
 * name of the monitor at its origin on Windows (the EDID record carries no
 * output name there), else its RandR output / macOS UUID, else its connector.
 */
static void
monitor_device_name(const struct os_display_edid_monitor *m, char *out, size_t cap)
{
	out[0] = '\0';
#ifdef XRT_OS_WINDOWS
	struct os_display_desktop_info dm;
	memset(&dm, 0, sizeof(dm));
	if (os_display_desktop_info_at(m->screen_left, m->screen_top, &dm) && dm.left == m->screen_left &&
	    dm.top == m->screen_top) {
		(void)snprintf(out, cap, "%.*s", (int)(cap - 1), dm.device_name);
		return;
	}
#endif
	(void)snprintf(out, cap, "%s", m->output_name[0] != '\0' ? m->output_name : m->connector);
}

static bool
edid_triple_zero(const struct os_display_edid_monitor *m)
{
	return m->manufacturer_id == 0 && m->product_id == 0 && m->serial_number == 0;
}

static bool
same_name(const char *a, const char *b)
{
	return a[0] != '\0' && b[0] != '\0' && strcmp(a, b) == 0;
}

enum target_edid_carry
target_plugin_edid_carry(const struct os_display_edid_monitor *prev,
                         uint64_t prev_read_ns,
                         struct os_display_edid_monitor *cur,
                         uint64_t now_ns)
{
	if (prev == NULL || cur == NULL || !edid_triple_zero(cur) || edid_triple_zero(prev)) {
		return TARGET_EDID_CARRY_NONE;
	}
	// The same output: the RandR output name (or macOS UUID) when both carry
	// one, else the DRM connector. An unreadable EDID can also lose the
	// DRM join, so the connector alone is not required.
	const bool same_output =
	    same_name(prev->output_name, cur->output_name) || same_name(prev->connector, cur->connector);
	const bool same_rect = prev->screen_left == cur->screen_left && prev->screen_top == cur->screen_top &&
	                       prev->pixel_width == cur->pixel_width && prev->pixel_height == cur->pixel_height &&
	                       !cur->origin_unknown;
	if (!same_output || !same_rect) {
		return TARGET_EDID_CARRY_NONE;
	}
	if (now_ns < prev_read_ns || now_ns - prev_read_ns > TARGET_EDID_CARRY_WINDOW_NS) {
		return TARGET_EDID_CARRY_EXPIRED;
	}
	cur->manufacturer_id = prev->manufacturer_id;
	cur->product_id = prev->product_id;
	cur->serial_number = prev->serial_number;
	if (cur->connector[0] == '\0') {
		(void)snprintf(cur->connector, sizeof(cur->connector), "%s", prev->connector);
	}
	if (cur->output_name[0] == '\0') {
		(void)snprintf(cur->output_name, sizeof(cur->output_name), "%s", prev->output_name);
	}
	if (cur->display_name[0] == '\0') {
		(void)snprintf(cur->display_name, sizeof(cur->display_name), "%s", prev->display_name);
	}
	if (cur->physical_width_mm == 0 || cur->physical_height_mm == 0) {
		cur->physical_width_mm = prev->physical_width_mm;
		cur->physical_height_mm = prev->physical_height_mm;
	}
	if (cur->native_width == 0 || cur->native_height == 0) {
		cur->native_width = prev->native_width;
		cur->native_height = prev->native_height;
	}
	if (cur->preferred_width == 0 || cur->preferred_height == 0) {
		cur->preferred_width = prev->preferred_width;
		cur->preferred_height = prev->preferred_height;
	}
	if (cur->join == OS_EDID_JOIN_NONE) {
		cur->join = prev->join;
	}
	return TARGET_EDID_CARRY_KEPT;
}

uint32_t
target_plugin_build_descriptors(const struct os_display_edid_list *list,
                                struct xrt_display_descriptor *out,
                                uint32_t max)
{
	if (list == NULL || out == NULL || max == 0) {
		return 0;
	}
	log_monitor_join_once(list);
	uint32_t n = list->count;
	if (n > max) {
		n = max;
	}
	if (n > OS_DISPLAY_EDID_MAX_MONITORS) {
		n = OS_DISPLAY_EDID_MAX_MONITORS;
	}
	const uint64_t now_ns = os_monotonic_get_ns();
	struct os_display_edid_monitor mons[OS_DISPLAY_EDID_MAX_MONITORS];
	uint64_t read_ns[OS_DISPLAY_EDID_MAX_MONITORS];
	bool carried[OS_DISPLAY_EDID_MAX_MONITORS];
	uint32_t carried_count = 0;

	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}
	// ADR-051 D6: mid-reseat, the compositor can still list an output whose
	// EDID reads back empty. Keep the identity the previous build read on the
	// same output and rect, or the screen table publishes a removal plus an
	// EDID-less "new" screen, then the reverse once the EDID is back.
	bool prev_used[XRT_DP_REGISTRY_MAX_ENTRIES] = {false};
	for (uint32_t i = 0; i < n; i++) {
		mons[i] = list->monitors[i];
		read_ns[i] = now_ns;
		carried[i] = false;
		if (!edid_triple_zero(&mons[i])) {
			continue;
		}
		for (uint32_t j = 0; j < g_monitor_side_count; j++) {
			if (prev_used[j]) {
				continue;
			}
			const enum target_edid_carry r = target_plugin_edid_carry(
			    &g_monitor_side[j].mon, g_monitor_side[j].edid_read_ns, &mons[i], now_ns);
			if (r == TARGET_EDID_CARRY_NONE) {
				continue;
			}
			prev_used[j] = true;
			if (r == TARGET_EDID_CARRY_KEPT) {
				read_ns[i] = g_monitor_side[j].edid_read_ns;
				carried[i] = true;
				carried_count++;
				if (!g_monitor_side[j].edid_carried) {
					U_LOG_W(
					    "plugin loader: monitor '%s' at (%d,%d) %ux%u: EDID unreadable — "
					    "keeping its identity mfr=0x%04x product=0x%04x serial=0x%08x "
					    "(key '%s') for up to %llu s while it is re-read",
					    mons[i].output_name[0] != '\0' ? mons[i].output_name : mons[i].connector,
					    (int)mons[i].screen_left, (int)mons[i].screen_top, mons[i].pixel_width,
					    mons[i].pixel_height, mons[i].manufacturer_id, mons[i].product_id,
					    mons[i].serial_number, g_monitor_side[j].key,
					    (unsigned long long)(TARGET_EDID_CARRY_WINDOW_NS /
					                         (1000ull * 1000ull * 1000ull)));
				}
			} else if (g_monitor_side[j].edid_carried) {
				U_LOG_W(
				    "plugin loader: monitor '%s' at (%d,%d): EDID still unreadable after %llu s "
				    "— treating it as a display without an EDID",
				    mons[i].output_name[0] != '\0' ? mons[i].output_name : mons[i].connector,
				    (int)mons[i].screen_left, (int)mons[i].screen_top,
				    (unsigned long long)(TARGET_EDID_CARRY_WINDOW_NS / (1000ull * 1000ull * 1000ull)));
			}
			break;
		}
	}

	for (uint32_t i = 0; i < n; i++) {
		const struct os_display_edid_monitor *m = &mons[i];
		struct xrt_display_descriptor *d = &out[i];
		memset(d, 0, sizeof(*d));
		d->struct_size = (uint32_t)sizeof(*d);
		d->monitor_id =
		    monitor_id_from_edid(m->manufacturer_id, m->product_id, m->serial_number, m->screen_left,
		                         m->screen_top, m->connector[0] != '\0' ? m->connector : m->output_name);
		d->edid_manufacturer = m->manufacturer_id;
		d->edid_product = m->product_id;
		d->pixel_width = m->pixel_width;
		d->pixel_height = m->pixel_height;
		d->refresh_mhz = m->refresh_hz * 1000u;
		d->screen_left = m->screen_left;
		d->screen_top = m->screen_top;
		d->flags = m->is_primary ? 1u : 0u;
	}

	// #2250: what the last resolve learned of each monitor's vendor serial,
	// by EDID-form key, so the key does not drop back to the EDID form
	// between this enumeration and the next resolve (or while the claim that
	// carried the serial is momentarily absent, e.g. present-not-ready).
	struct
	{
		char legacy_key[TARGET_SCREEN_KEY_MAX];
		char vendor_serial[TARGET_SCREEN_KEY_SERIAL_MAX + 1];
	} prev_serial[XRT_DP_REGISTRY_MAX_ENTRIES];
	const uint32_t prev_count = g_monitor_side_count;
	for (uint32_t i = 0; i < prev_count; i++) {
		(void)snprintf(prev_serial[i].legacy_key, sizeof(prev_serial[i].legacy_key), "%s",
		               g_monitor_side[i].legacy_key);
		(void)snprintf(prev_serial[i].vendor_serial, sizeof(prev_serial[i].vendor_serial), "%s",
		               g_monitor_side[i].vendor_serial);
	}

	g_monitor_side_count = n < XRT_DP_REGISTRY_MAX_ENTRIES ? n : XRT_DP_REGISTRY_MAX_ENTRIES;
	for (uint32_t i = 0; i < g_monitor_side_count; i++) {
		g_monitor_side[i].vendor_serial[0] = '\0';
		g_monitor_side[i].monitor_id = out[i].monitor_id;
		g_monitor_side[i].cand_count = 0;
		g_monitor_side[i].cands_valid = false;
		g_monitor_side[i].mon = mons[i];
		g_monitor_side[i].desc = out[i];
		g_monitor_side[i].edid_read_ns = read_ns[i];
		g_monitor_side[i].edid_carried = carried[i];
		monitor_device_name(&mons[i], g_monitor_side[i].device_name, sizeof(g_monitor_side[i].device_name));
	}
	// Display dashboard phase 7: the stable key a per-screen preference is
	// stored under (the monitor id is per boot). First the EDID form, which
	// finds each monitor's remembered vendor serial, then the real keys.
	monitor_side_rebuild_keys();
	for (uint32_t i = 0; i < g_monitor_side_count; i++) {
		for (uint32_t j = 0; j < prev_count; j++) {
			if (prev_serial[j].vendor_serial[0] != '\0' &&
			    strcmp(prev_serial[j].legacy_key, g_monitor_side[i].legacy_key) == 0) {
				(void)snprintf(g_monitor_side[i].vendor_serial, sizeof(g_monitor_side[i].vendor_serial),
				               "%s", prev_serial[j].vendor_serial);
				break;
			}
		}
	}
	monitor_side_rebuild_keys();
	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
	}

	// A carried identity is provisional: ask for another enumeration so the
	// real EDID is read once the kernel has it (the window bounds the series).
	// Rate-limited, so a status read every second cannot keep re-arming the
	// service worker's debounce and starve the re-run.
	static uint64_t s_last_followup_ns = 0;
	if (carried_count > 0 && now_ns - s_last_followup_ns >= 2ull * 1000ull * 1000ull * 1000ull) {
		s_last_followup_ns = now_ns;
		target_display_hotplug_request_followup("EDID unreadable — identity kept, re-reading");
	}
	return n;
}

bool
target_plugin_get_monitor_key(uint64_t monitor_id, char *out_key, size_t key_cap, char *out_device, size_t device_cap)
{
	bool found = false;
	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}
	for (uint32_t i = 0; i < g_monitor_side_count; i++) {
		if (g_monitor_side[i].monitor_id != monitor_id) {
			continue;
		}
		if (out_key != NULL && key_cap > 0) {
			(void)snprintf(out_key, key_cap, "%s", g_monitor_side[i].key);
		}
		if (out_device != NULL && device_cap > 0) {
			(void)snprintf(out_device, device_cap, "%s", g_monitor_side[i].device_name);
		}
		found = true;
		break;
	}
	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
	}
	return found;
}

bool
target_plugin_get_monitor_legacy_key(uint64_t monitor_id, char *out_key, size_t key_cap)
{
	bool differs = false;
	if (out_key != NULL && key_cap > 0) {
		out_key[0] = '\0';
	}
	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}
	for (uint32_t i = 0; i < g_monitor_side_count; i++) {
		if (g_monitor_side[i].monitor_id != monitor_id) {
			continue;
		}
		differs = g_monitor_side[i].legacy_key[0] != '\0' &&
		          strcmp(g_monitor_side[i].legacy_key, g_monitor_side[i].key) != 0;
		if (differs && out_key != NULL && key_cap > 0) {
			(void)snprintf(out_key, key_cap, "%s", g_monitor_side[i].legacy_key);
		}
		break;
	}
	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
	}
	return differs;
}

bool
target_plugin_get_monitor_candidates(uint64_t monitor_id,
                                     struct target_plugin_screen_candidate *out,
                                     uint32_t max,
                                     uint32_t *out_count)
{
	bool found = false;
	if (out_count != NULL) {
		*out_count = 0;
	}
	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}
	for (uint32_t i = 0; i < g_monitor_side_count; i++) {
		if (g_monitor_side[i].monitor_id != monitor_id) {
			continue;
		}
		if (g_monitor_side[i].cands_valid) {
			uint32_t n = 0;
			for (; out != NULL && n < g_monitor_side[i].cand_count && n < max; n++) {
				out[n] = g_monitor_side[i].cands[n];
			}
			if (out_count != NULL) {
				*out_count = n;
			}
			found = true;
		}
		break;
	}
	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
	}
	return found;
}

bool
target_plugin_get_monitor_record(uint64_t monitor_id,
                                 struct xrt_display_descriptor *out_desc,
                                 struct os_display_edid_monitor *out_mon)
{
	bool found = false;
	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}
	for (uint32_t i = 0; i < g_monitor_side_count; i++) {
		if (g_monitor_side[i].monitor_id != monitor_id) {
			continue;
		}
		if (out_desc != NULL) {
			*out_desc = g_monitor_side[i].desc;
		}
		if (out_mon != NULL) {
			*out_mon = g_monitor_side[i].mon;
		}
		found = true;
		break;
	}
	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
	}
	return found;
}

#ifndef XRT_OS_WINDOWS
static const struct os_display_edid_monitor *
monitor_side_find(uint64_t monitor_id)
{
	for (uint32_t i = 0; i < g_monitor_side_count; i++) {
		if (g_monitor_side[i].monitor_id == monitor_id) {
			return &g_monitor_side[i].mon;
		}
	}
	return NULL;
}
#endif

/*!
 * Off-Windows: which descriptor is the panel described by @p hint, by the
 * same ADR-033 rules the desktop-rect resolver uses (origin, then connector
 * mode, then pixel size; ties on physical size). Pure.
 */
#ifndef XRT_OS_WINDOWS
static bool
panel_monitor_index(const struct xrt_display_descriptor *descs,
                    uint32_t n,
                    const struct os_display_edid_monitor *const *monitors,
                    const struct os_display_panel_hint *hint,
                    uint32_t *out_pick)
{
	if (n > XRT_DP_REGISTRY_MAX_ENTRIES) {
		n = XRT_DP_REGISTRY_MAX_ENTRIES;
	}

	struct os_display_desktop_info mons[XRT_DP_REGISTRY_MAX_ENTRIES];
	memset(mons, 0, sizeof(mons));
	bool origin_known[XRT_DP_REGISTRY_MAX_ENTRIES];
	for (uint32_t i = 0; i < n; i++) {
		struct os_display_desktop_info *m = &mons[i];
		m->left = descs[i].screen_left;
		m->top = descs[i].screen_top;
		m->width = descs[i].pixel_width;
		m->height = descs[i].pixel_height;
		m->width_in_caller_dpi = m->width;
		m->height_in_caller_dpi = m->height;
		m->is_primary = (descs[i].flags & 1u) != 0;
		origin_known[i] = true;
		const struct os_display_edid_monitor *side = monitors != NULL ? monitors[i] : NULL;
		if (side != NULL) {
			m->physical_width_mm = side->physical_width_mm;
			m->physical_height_mm = side->physical_height_mm;
			m->native_width = side->native_width;
			m->native_height = side->native_height;
			(void)snprintf(m->device_name, sizeof(m->device_name), "%s",
			               side->output_name[0] != '\0' ? side->output_name : side->connector);
			origin_known[i] = !side->origin_unknown;
		}
	}

	// Rule 1: the plug-in placed its panel. Trust a non-(0,0) origin that
	// falls inside a monitor whose own origin is a real desktop position.
	if (hint->screen_left != 0 || hint->screen_top != 0) {
		for (uint32_t i = 0; i < n; i++) {
			const struct os_display_desktop_info *m = &mons[i];
			if (origin_known[i] && hint->screen_left >= m->left &&
			    hint->screen_left < m->left + (int64_t)m->width && hint->screen_top >= m->top &&
			    hint->screen_top < m->top + (int64_t)m->height) {
				*out_pick = i;
				return true;
			}
		}
	}

	// Rules 2/3: connector mode, then pixel size.
	struct os_display_panel_match match = {0};
	const int32_t idx = os_display_desktop_select_by_size(mons, n, hint, &match);
	if (idx >= 0) {
		*out_pick = (uint32_t)idx;
		return true;
	}
	return false;
}
#endif

uint32_t
target_plugin_backcompat_claim_index(const struct xrt_display_descriptor *descs,
                                     uint32_t n,
                                     const struct os_display_edid_monitor *const *monitors,
                                     const struct os_display_panel_hint *panel)
{
	if (descs == NULL || n == 0) {
		return 0;
	}
	uint32_t pick = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (descs[i].flags & 1u) {
			pick = i;
			break;
		}
	}
#ifndef XRT_OS_WINDOWS
	uint32_t at_panel = 0;
	if (panel != NULL && panel->pixel_width > 0 && panel->pixel_height > 0 &&
	    panel_monitor_index(descs, n, monitors, panel, &at_panel)) {
		pick = at_panel;
	}
#else
	(void)monitors;
	(void)panel;
#endif
	return pick;
}

/*!
 * Synthesize the back-compat claim for a loaded plug-in that has no
 * `probe_displays` but whose binary `probe()` succeeded: one
 * @ref XRT_DISPLAY_CLAIM_EDID claim on the monitor
 * @ref target_plugin_backcompat_claim_index picks — the primary monitor, or,
 * off-Windows for the ACTIVE plug-in once its panel is known, that panel's
 * monitor. `supported_apis` is set to all bits — the actual factory set is
 * masked against the plug-in's non-NULL factory pointers at fill time.
 *
 * Why the panel and not the primary: a 3D panel is a SECOND monitor on
 * essentially every real deployment, so "the primary" lands the vendor's
 * claim on the laptop screen — and the active plug-in wins every monitor it
 * claims (#1521), so the registry would route the laptop screen to the vendor
 * DP and the panel to the fallback. Caller holds @ref g_refresh_mutex.
 */
static uint32_t
synth_primary_edid_claim(const struct xrt_display_descriptor *descs,
                         uint32_t n,
                         struct xrt_display_claim *out,
                         uint32_t max,
                         const struct xrt_plugin_iface *iface)
{
	if (n == 0 || max == 0) {
		return 0;
	}
	if (n > XRT_DP_REGISTRY_MAX_ENTRIES) {
		n = XRT_DP_REGISTRY_MAX_ENTRIES;
	}
	const bool is_active = iface != NULL && iface == g_active_iface;
	const uint32_t primary = target_plugin_backcompat_claim_index(descs, n, NULL, NULL);
	uint32_t pick = primary;
#ifndef XRT_OS_WINDOWS
	// A plug-in that ships `probe_displays` never reaches here, so its own
	// claims always win; only the ACTIVE plug-in's panel is known.
	if (is_active && g_active_panel_valid) {
		const struct os_display_edid_monitor *side[XRT_DP_REGISTRY_MAX_ENTRIES];
		for (uint32_t i = 0; i < n; i++) {
			side[i] = monitor_side_find(descs[i].monitor_id);
		}
		pick = target_plugin_backcompat_claim_index(descs, n, side, &g_active_panel_hint);
	}
#else
	(void)is_active;
#endif
	if (pick != primary) {
		U_LOG_I(
		    "plugin loader: '%s' has no probe_displays — back-compat claim placed on its panel "
		    "(monitor 0x%016llx, %ux%u at (%d,%d)) instead of the primary monitor",
		    iface->id != NULL ? iface->id : "?", (unsigned long long)descs[pick].monitor_id,
		    descs[pick].pixel_width, descs[pick].pixel_height, (int)descs[pick].screen_left,
		    (int)descs[pick].screen_top);
	}
	out[0].monitor_id = descs[pick].monitor_id;
	out[0].confidence = (uint32_t)XRT_DISPLAY_CLAIM_EDID;
	out[0].supported_apis = 0xFFFFFFFFu;
	out[0].serial[0] = '\0';
	return 1;
}

/*!
 * Get one source's claims for the supplied descriptors: call its
 * `probe_displays()` if present (struct_size-guarded), else synthesize the
 * back-compat primary-monitor claim.
 */
static uint32_t
query_source_claims(const struct plugin_display_source *src,
                    const struct xrt_display_descriptor *descs,
                    uint32_t n,
                    struct xrt_display_claim *out,
                    uint32_t max)
{
	const struct xrt_plugin_iface *iface = src->iface;
	if (iface == NULL) {
		return 0;
	}
	if (iface->struct_size >= offsetof(struct xrt_plugin_iface, probe_displays) + sizeof(iface->probe_displays) &&
	    iface->probe_displays != NULL) {
		return iface->probe_displays(src->inst, descs, n, out, max);
	}
	return synth_primary_edid_claim(descs, n, out, max, iface);
}

/*!
 * Copy the winning source's per-API factory pointers (masked by the claim's
 * supported_apis AND the plug-in's actually-non-NULL factory) plus identity
 * into a registry entry. Mirrors the platform gating of
 * `fill_dp_factories_from_plugin` in target_instance.c.
 */
/*!
 * #1243/#1244: is this plug-in's `struct vk_bundle` layout compatible with ours?
 *
 * The VK DP factory hands the plug-in a raw `vk_bundle *`, and that struct's
 * layout varies with BUILD CONFIG (`os_mutex` carries `#ifndef NDEBUG` fields,
 * and `vk_bundle` embeds two of them via `queues[2]` — 16 bytes, exactly two
 * function-pointer slots) as well as with Vulkan-header gates. Dispatching
 * through a skewed table calls the WRONG driver entry points; the observed
 * failure is a SIGSEGV inside the Adreno driver from `process_atlas_weave`.
 *
 * Lives here, called from EVERY site that captures `create_dp_vk` — originally
 * only the per-display-claims path below was guarded, which left
 * `target_instance.c`'s `fill_dp_factories_from_plugin()` (the path Android
 * actually takes) unprotected, so the guard never stood between an Android
 * device and the crash it was written for. One function, all call sites.
 *
 * @param iface      The negotiated plug-in interface.
 * @param plugin_id  For logging only.
 * @return true if the factory may be used.
 */
bool
xrt_plugin_vk_abi_compatible(const struct xrt_plugin_iface *iface, const char *plugin_id)
{
	if (iface == NULL) {
		return false;
	}
	const char *id = (plugin_id != NULL) ? plugin_id : "(unknown)";
#ifdef XRT_HAVE_VULKAN
	const uint32_t rt_vkb = vk_bundle_get_abi_size();
	const uint32_t rt_fto = vk_bundle_get_fn_table_offset();
	const bool have_field =
	    iface->struct_size >=
	    (uint32_t)(offsetof(struct xrt_plugin_iface, vk_bundle_fn_table_offset) + sizeof(uint32_t));
	const uint32_t pl_vkb = have_field ? iface->vk_bundle_abi_size : 0;
	const uint32_t pl_fto = have_field ? iface->vk_bundle_fn_table_offset : 0;

	if (pl_vkb == rt_vkb && pl_fto == rt_fto && pl_vkb != 0) {
		return true;
	}
	if (pl_vkb != 0) {
		U_LOG_E("plugin loader:   %s: vk_bundle ABI mismatch — plug-in compiled "
		        "sizeof=%u fn_table_offset=%u, runtime sizeof=%u fn_table_offset=%u "
		        "(build-config skew: NDEBUG/os_mutex or Vulkan-header gates, #1243). "
		        "Refusing the VK DP factory; the session will run UNWOVEN. Rebuild "
		        "the plug-in with the same build config (NDEBUG) and Vulkan headers "
		        "as this runtime.",
		        id, pl_vkb, pl_fto, rt_vkb, rt_fto);
		return false;
	}
#ifdef XRT_OS_ANDROID
	U_LOG_E("plugin loader:   %s: plug-in predates the vk_bundle ABI guard (#1243) — "
	        "cannot verify layout compatibility, and every pre-guard Android pairing "
	        "is the Debug-vs-Release crash class. Refusing the VK DP factory; the "
	        "session will run UNWOVEN. Update the plug-in.",
	        id);
	return false;
#else
	U_LOG_W("plugin loader:   %s: plug-in has no vk_bundle_abi_size (predates the "
	        "#1243 ABI guard) — layout compatibility UNVERIFIED; proceeding for "
	        "desktop back-compat (shipped Linux .debs are Release-built on both "
	        "sides). IF THIS PROCESS LATER CRASHES INSIDE A VULKAN DRIVER CALL "
	        "(e.g. vkCreateFramebuffer), THIS IS WHY: rebuild the plug-in with the "
	        "same build config (NDEBUG) as this runtime.",
	        id);
	return true;
#endif
#else
	(void)id;
	return true;
#endif
}

static void
fill_registry_entry(struct xrt_dp_registry_entry *e,
                    const struct xrt_display_descriptor *desc,
                    const struct plugin_display_source *src,
                    const struct xrt_display_claim *claim)
{
	const struct xrt_plugin_iface *iface = src->iface;
	memset(e, 0, sizeof(*e));
	e->monitor_id = desc->monitor_id;
	e->confidence = claim->confidence;
	e->screen_left = desc->screen_left;
	e->screen_top = desc->screen_top;
	e->pixel_width = desc->pixel_width;
	e->pixel_height = desc->pixel_height;
	snprintf(e->plugin_id, sizeof(e->plugin_id), "%s", iface->id != NULL ? iface->id : "");
	snprintf(e->serial, sizeof(e->serial), "%s", claim->serial);

	if ((claim->supported_apis & XRT_DP_API_BIT_VK) && iface->create_dp_vk != NULL) {
		// #1243/#1244 — shared check, see xrt_plugin_vk_abi_compatible().
		if (xrt_plugin_vk_abi_compatible(iface, e->plugin_id)) {
			e->dp_factory_vk = (void *)iface->create_dp_vk;
		}
	}
#ifdef XRT_OS_WINDOWS
	if ((claim->supported_apis & XRT_DP_API_BIT_D3D11) && iface->create_dp_d3d11 != NULL) {
		e->dp_factory_d3d11 = (void *)iface->create_dp_d3d11;
	}
	if ((claim->supported_apis & XRT_DP_API_BIT_D3D12) && iface->create_dp_d3d12 != NULL) {
		e->dp_factory_d3d12 = (void *)iface->create_dp_d3d12;
	}
#endif
	if ((claim->supported_apis & XRT_DP_API_BIT_GL) && iface->create_dp_gl != NULL) {
		e->dp_factory_gl = (void *)iface->create_dp_gl;
	}
#ifdef __APPLE__
	if ((claim->supported_apis & XRT_DP_API_BIT_METAL) && iface->create_dp_metal != NULL) {
		e->dp_factory_metal = (void *)iface->create_dp_metal;
	}
#endif
	e->owning_iface = (const void *)iface;
	e->owning_instance = src->inst;
}

#if !defined(XRT_OS_WINDOWS) && !defined(XRT_OS_ANDROID)
/*!
 * POSIX: does the active plug-in, on its own, already decide every monitor?
 * The active plug-in wins every monitor it claims, at any confidence (#1521),
 * and only three things outrank it: a different PreferredPlugin, a
 * `DXR_SCREEN_PLUGIN` pin naming a different plug-in for that monitor, and a
 * per-screen preference naming one (display dashboard phase 7). When it
 * claims every descriptor and neither applies, loading and probing every other
 * installed plug-in cannot change a single registry entry. It would only
 * dlopen them, run their vendor probes (Leia's reaches the SR service and the
 * shared panel), and keep their instances. Typical cases:
 * `XRT_PREFERRED_PLUGIN_ID=sim-display`, or sim-display active because no
 * vendor plug-in is installed.
 *
 * The pin check matters: `XRT_PREFERRED_PLUGIN_ID=sim-display
 * DXR_SCREEN_PLUGIN=HDMI-1=leia-sr` (the documented sim-session-with-a-woven-
 * DS1 recipe) needs leia-sr loaded as a claim source, or the pin finds no
 * leia-sr claim and is ignored. The decision itself is the pure
 * target_screen_active_decides_every_monitor() (unit-tested). Caller holds
 * @ref g_refresh_mutex (the monitor side table).
 */
static bool
active_plugin_decides_every_monitor(const struct xrt_display_descriptor *descs, uint32_t n)
{
	if (g_active_iface == NULL || g_active_iface->id == NULL || descs == NULL || n == 0) {
		return false;
	}
	char preferred[64] = {0};
	const bool have_preferred = target_plugin_get_preferred(preferred, sizeof(preferred));

	struct target_screen_pins pins;
	target_screen_pin_parse(getenv("DXR_SCREEN_PLUGIN"), &pins, NULL);

	const struct plugin_display_source active = {
	    .iface = g_active_iface,
	    .inst = g_active_instance,
	    .probe_order = g_active_probe_order,
	};
	struct xrt_display_claim claims[XRT_DP_REGISTRY_MAX_ENTRIES];
	const uint32_t cn = query_source_claims(&active, descs, n, claims, XRT_DP_REGISTRY_MAX_ENTRIES);

	struct target_screen_monitor mons[XRT_DP_REGISTRY_MAX_ENTRIES];
	char screen_pref[XRT_DP_REGISTRY_MAX_ENTRIES][64];
	const uint32_t mn = n < XRT_DP_REGISTRY_MAX_ENTRIES ? n : XRT_DP_REGISTRY_MAX_ENTRIES;
	for (uint32_t d = 0; d < mn; d++) {
		mons[d].monitor_id = descs[d].monitor_id;
		mons[d].output_name = "";
		mons[d].connector = "";
		mons[d].active_claims = false;
		for (uint32_t c = 0; c < cn && !mons[d].active_claims; c++) {
			mons[d].active_claims = claims[c].monitor_id == descs[d].monitor_id;
		}
		mons[d].screen_pref = NULL;
		for (uint32_t i = 0; i < g_monitor_side_count; i++) {
			if (g_monitor_side[i].monitor_id == descs[d].monitor_id) {
				mons[d].output_name = g_monitor_side[i].mon.output_name;
				mons[d].connector = g_monitor_side[i].mon.connector;
				if (u_setting_get_preferred_plugin_for_screen(g_monitor_side[i].key, screen_pref[d],
				                                              sizeof(screen_pref[d]), NULL) != NULL) {
					mons[d].screen_pref = screen_pref[d];
				}
				break;
			}
		}
	}
	return target_screen_active_decides_every_monitor(g_active_iface->id, have_preferred ? preferred : NULL, &pins,
	                                                  mons, mn);
}
#endif

/*!
 * Build the cached display-claim source set if not already collected. Caller
 * holds @ref g_refresh_mutex and has loaded the active plug-in.
 *
 * POSIX only loads the other plug-ins when they could matter: if the active
 * plug-in decides every monitor in @p descs on its own
 * (@ref active_plugin_decides_every_monitor) the set is just the active
 * plug-in, and that answer is re-checked on the next resolve.
 */
static void
ensure_display_sources(const struct xrt_display_descriptor *descs, uint32_t n)
{
#if !defined(XRT_OS_WINDOWS) && !defined(XRT_OS_ANDROID)
	if (g_display_source_count >= 0 && !g_display_sources_active_only) {
		return;
	}
	if (active_plugin_decides_every_monitor(descs, n)) {
		g_display_sources[0].iface = g_active_iface;
		g_display_sources[0].inst = g_active_instance;
		g_display_sources[0].probe_order = g_active_probe_order;
		g_display_source_count = 1;
		g_display_sources_active_only = true;
		return;
	}
	g_display_sources_active_only = false;
#else
	(void)descs;
	(void)n;
	if (g_display_source_count >= 0) {
		return;
	}
#endif
	g_display_source_count = 0;
#if !defined(XRT_OS_ANDROID)
	// Windows (registry) and macOS/Linux (manifests): every registered
	// plug-in is a claim source, so the fallback and a vendor plug-in both
	// contribute. macOS has no EDID enumerator yet, so resolution is a no-op
	// there before this is ever reached.
	g_display_source_count = collect_display_sources_platform(g_display_sources, TARGET_PLUGIN_MAX_SOURCES);
#else
	// Android: one plug-in ships inside the runtime APK (ADR-038) and the
	// EDID enumerator yields no monitors; the active plug-in is the lone
	// source for any caller that supplies its own descriptors.
	if (g_active_iface != NULL) {
		g_display_sources[0].iface = g_active_iface;
		g_display_sources[0].inst = g_active_instance;
		g_display_sources[0].probe_order = g_active_probe_order;
		g_display_source_count = 1;
	}
#endif
}

//! Case-insensitive plug-in id compare (a per-screen preference is written by hand or a UI).
static bool
screen_ids_equal(const char *a, const char *b)
{
	for (; *a != '\0' && *b != '\0'; a++, b++) {
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') {
			ca = (char)(ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb = (char)(cb - 'A' + 'a');
		}
		if (ca != cb) {
			return false;
		}
	}
	return *a == *b;
}

#if !defined(XRT_OS_ANDROID)
/*!
 * `Enabled` for a plug-in that is ALREADY a display-claim source (N3b + N3d,
 * #2049 / #2051). @ref dp_entry_usable keeps a disabled plug-in from ever
 * loading, but the source set is collected once per process and a live
 * `dp disable <id>` lands after that: the loaded plug-in must stop
 * contributing claims, so every screen it drove re-resolves to the next
 * candidate (the service recreates a secondary's segment DP; the session's
 * primary DP stays until the next session, plugin-discovery.md §4.2a).
 * Nothing is unloaded or destroyed — a live segment DP may still be made from
 * the instance — and a source with no registration row at all is kept (the
 * "removed after it loaded" rule of @ref collect_display_sources_platform).
 * Re-read on every resolve (the policy overlay is what changed); each edge is
 * logged once. Caller holds @ref g_refresh_mutex.
 */
static uint32_t g_withdrawn_mask = 0; // by source index; the set is stable per process

static uint32_t
dp_sources_apply_enabled(bool *withdrawn, uint32_t src_count)
{
	for (uint32_t s = 0; s < src_count; s++) {
		withdrawn[s] = false;
	}
	struct target_component_entry *entries = dp_entries_alloc();
	if (entries == NULL) {
		return 0;
	}
	const int n = dp_enumerate(entries, false);
	uint32_t mask = 0;
	uint32_t count = 0;
	for (uint32_t s = 0; s < src_count && s < 32; s++) {
		const struct xrt_plugin_iface *sif = g_display_sources[s].iface;
		if (sif == NULL || sif->id == NULL) {
			continue;
		}
		for (int i = 0; i < n; i++) {
			if ((entries[i].flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0 ||
			    strcmp(entries[i].id, sif->id) != 0) {
				continue;
			}
			const bool was = (g_withdrawn_mask & (1u << s)) != 0;
			if (!entries[i].enabled) {
				withdrawn[s] = true;
				mask |= 1u << s;
				count++;
				if (!was) {
					U_LOG_W(
					    "plugin loader: '%s' withdrawn from the display claims (Enabled=0, %s): "
					    "its "
					    "screens re-resolve now, the session's primary DP at the next session.",
					    sif->id, dp_enabled_source(&entries[i]));
				}
			} else if (was) {
				U_LOG_W("plugin loader: '%s' re-admitted to the display claims (Enabled=1).", sif->id);
			}
			break;
		}
	}
	g_withdrawn_mask = mask;
	free(entries);
	return count;
}
#endif /* !XRT_OS_ANDROID */

//! Monitors the last resolve found "present, not ready" (WARN only on entry). Guarded by g_refresh_mutex.
static uint64_t g_not_ready_prev[XRT_DP_REGISTRY_MAX_ENTRIES];
static uint32_t g_not_ready_prev_count = 0;

//! #2250: this process may rewrite the per-user store to re-key a legacy screen key (service, CLI).
static bool g_screen_key_rekey_allowed = false;

void
target_plugin_set_screen_key_rekey(bool allowed)
{
	g_screen_key_rekey_allowed = allowed;
}

struct screen_pref_probe
{
	char *buf;
	size_t cap;
	enum u_setting_source source;
};

static bool
screen_pref_has(const char *key, void *userdata)
{
	struct screen_pref_probe *p = (struct screen_pref_probe *)userdata;
	return u_setting_get_preferred_plugin_for_screen(key, p->buf, p->cap, &p->source) != NULL;
}

/*!
 * The per-screen preference for a monitor, by its current key, else by its
 * EDID-form @p legacy_key (#2250: a preference stored before the key moved to
 * the vendor serial keeps applying). A per-user legacy match is re-keyed in
 * the store (one WARN; the next probe matches the current key directly) — but
 * only in a process that opted in (@ref target_plugin_set_screen_key_rekey:
 * the service and the CLI; an app never rewrites the user's settings file).
 * Anywhere else, and for an env / machine match (no write access from here),
 * the legacy match applies and is WARNed once per process with the command
 * that re-stores it. Caller holds
 * @ref g_refresh_mutex (it also guards the WARN latch).
 */
static bool
screen_pref_lookup(const char *key, const char *legacy_key, char *buf, size_t cap, enum u_setting_source *out_source)
{
	struct screen_pref_probe probe = {buf, cap, U_SETTING_SOURCE_DEFAULT};
	const enum target_screen_key_match m = target_screen_key_pref_match(key, legacy_key, screen_pref_has, &probe);
	*out_source = probe.source;
	if (m == TARGET_SCREEN_KEY_MATCH_NONE) {
		buf[0] = '\0';
		*out_source = U_SETTING_SOURCE_DEFAULT;
		return false;
	}
	if (m == TARGET_SCREEN_KEY_MATCH_CURRENT) {
		return true;
	}
	if (probe.source == U_SETTING_SOURCE_USER && g_screen_key_rekey_allowed &&
	    u_setting_user_set_preferred_plugin_for_screen(key, buf) &&
	    u_setting_user_set_preferred_plugin_for_screen(legacy_key, NULL)) {
		U_LOG_W(
		    "plugin loader: per-screen preference '%s' -> '%s' re-keyed to '%s' (per-user store): the screen "
		    "is now keyed by its vendor serial, which survives a cable or port change (#2250)",
		    legacy_key, buf, key);
		return true;
	}
	static char s_warned[XRT_DP_REGISTRY_MAX_ENTRIES][TARGET_SCREEN_KEY_MAX];
	static uint32_t s_warned_count = 0;
	for (uint32_t i = 0; i < s_warned_count; i++) {
		if (strcmp(s_warned[i], legacy_key) == 0) {
			return true;
		}
	}
	if (s_warned_count < XRT_DP_REGISTRY_MAX_ENTRIES) {
		(void)snprintf(s_warned[s_warned_count++], TARGET_SCREEN_KEY_MAX, "%s", legacy_key);
	}
	const char *hint =
	    " — the service or 'displayxr-cli dp use <id> --screen <key>' re-stores it under the new key";
	if (probe.source == U_SETTING_SOURCE_MACHINE) {
		hint = " — re-store it with 'displayxr-cli dp use <id> --screen <key> --machine' (admin)";
	} else if (probe.source == U_SETTING_SOURCE_ENV) {
		hint = " — update " U_SETTING_PER_SCREEN_ENV;
	} else if (g_screen_key_rekey_allowed) {
		hint = " — re-keying the per-user store failed";
	}
	U_LOG_W(
	    "plugin loader: per-screen preference '%s' -> '%s' (%s) matched by the screen's legacy EDID key; its "
	    "key is now '%s' (vendor serial, #2250)%s",
	    legacy_key, buf, u_setting_source_str(probe.source), key, hint);
	return true;
}

void
target_plugin_resolve_displays(const struct xrt_display_descriptor *descriptors,
                               uint32_t descriptor_count,
                               struct xrt_dp_factory_registry *out_registry)
{
	if (out_registry == NULL) {
		return;
	}
	memset(out_registry, 0, sizeof(*out_registry));
	if (descriptors == NULL || descriptor_count == 0) {
		return;
	}

	// Ensure the active plug-in is loaded (and the refresh mutex initialized)
	// before we lock — get_active() runs the one-shot discovery on first call.
	(void)target_plugin_get_active();

	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}

	uint32_t dn = descriptor_count;
	if (dn > XRT_DP_REGISTRY_MAX_ENTRIES) {
		dn = XRT_DP_REGISTRY_MAX_ENTRIES;
	}

	ensure_display_sources(descriptors, dn);

	// N3b live: a source the policy disabled since it loaded contributes
	// nothing below. POSIX's active-only shortcut set then holds no claim
	// source at all, so ask every other plug-in once (the full set stays
	// cached from here on — ensure_display_sources returns early on it).
	bool src_withdrawn[TARGET_PLUGIN_MAX_SOURCES] = {false};
#if !defined(XRT_OS_ANDROID)
	if (dp_sources_apply_enabled(src_withdrawn, g_display_source_count > 0 ? (uint32_t)g_display_source_count : 0) >
	        0 &&
	    g_display_sources_active_only) {
		g_display_sources_active_only = false;
		g_display_source_count = collect_display_sources_platform(g_display_sources, TARGET_PLUGIN_MAX_SOURCES);
		(void)dp_sources_apply_enabled(src_withdrawn,
		                               g_display_source_count > 0 ? (uint32_t)g_display_source_count : 0);
	}
#endif

	// Query every source once for the full descriptor set.
	static struct xrt_display_claim src_claims[TARGET_PLUGIN_MAX_SOURCES][XRT_DP_REGISTRY_MAX_ENTRIES];
	uint32_t src_claim_count[TARGET_PLUGIN_MAX_SOURCES] = {0};
	// The same claims flattened for target_screen_collect_candidates.
	static uint64_t src_mon[TARGET_PLUGIN_MAX_SOURCES][XRT_DP_REGISTRY_MAX_ENTRIES];
	static uint32_t src_conf[TARGET_PLUGIN_MAX_SOURCES][XRT_DP_REGISTRY_MAX_ENTRIES];
	struct target_screen_source_claims src_view[TARGET_PLUGIN_MAX_SOURCES];
	const uint32_t src_count = g_display_source_count > 0 ? (uint32_t)g_display_source_count : 0;
	for (uint32_t s = 0; s < src_count; s++) {
		src_claim_count[s] = src_withdrawn[s] ? 0u
		                                      : query_source_claims(&g_display_sources[s], descriptors, dn,
		                                                            src_claims[s], XRT_DP_REGISTRY_MAX_ENTRIES);
		for (uint32_t c = 0; c < src_claim_count[s]; c++) {
			src_mon[s][c] = src_claims[s][c].monitor_id;
			src_conf[s][c] = (uint32_t)src_claims[s][c].confidence;
		}
		const struct xrt_plugin_iface *sif = g_display_sources[s].iface;
		src_view[s].plugin_id = (sif != NULL && sif->id != NULL) ? sif->id : "";
		src_view[s].is_active = g_active_iface != NULL && sif == g_active_iface;
		src_view[s].monitor_ids = src_mon[s];
		src_view[s].confidences = src_conf[s];
		src_view[s].count = src_claim_count[s];
	}

	// #2250: a monitor with no EDID serial is keyed by a claim's vendor
	// serial, which names the panel whatever cable it is on (its EDID product
	// code can differ per input). Picked from every claim, not the winner:
	// the key decides the per-screen preference, which decides the winner.
	// No qualifying claim this time keeps the remembered serial.
	for (uint32_t i = 0; i < g_monitor_side_count; i++) {
		uint32_t conf[TARGET_PLUGIN_MAX_SOURCES];
		char ser_buf[TARGET_PLUGIN_MAX_SOURCES][sizeof(src_claims[0][0].serial)];
		const char *ser[TARGET_PLUGIN_MAX_SOURCES];
		uint32_t k = 0;
		for (uint32_t s = 0; s < src_count && k < TARGET_PLUGIN_MAX_SOURCES; s++) {
			for (uint32_t c = 0; c < src_claim_count[s]; c++) {
				if (src_claims[s][c].monitor_id != g_monitor_side[i].monitor_id) {
					continue;
				}
				conf[k] = (uint32_t)src_claims[s][c].confidence;
				(void)snprintf(ser_buf[k], sizeof(ser_buf[k]), "%.*s", (int)sizeof(ser_buf[k]) - 1,
				               src_claims[s][c].serial);
				ser[k] = ser_buf[k];
				k++;
				break;
			}
		}
		const int sp = target_screen_key_serial_pick(conf, ser, k);
		if (sp >= 0) {
			(void)target_screen_key_sanitize_serial(ser[sp], g_monitor_side[i].vendor_serial,
			                                        sizeof(g_monitor_side[i].vendor_serial));
		}
	}
	monitor_side_rebuild_keys();

	// #791: an explicit PreferredPlugin override (`displayxr-cli dp use <id>`,
	// or XRT_PREFERRED_PLUGIN_ID off-Windows) must force the WEAVING DP too, not
	// just the active head-device plug-in. Without this, the per-monitor winner
	// below is purely EDID confidence / ProbeOrder, so on real vendor hardware
	// the vendor DP wins the registry even when the user pinned sim_display for
	// testing — and the compositor then weaves with the vendor DP (exactly how
	// the #776 "byte-identical sim-vs-Leia" run happened, and why sim N>2 weaving
	// could not be visually validated on a vendor-hardware box). When the
	// preferred source claims a monitor, it wins that monitor outright. sim's
	// probe_displays claims every monitor at FALLBACK confidence, so `dp use
	// sim-display` forces sim across the board; a pinned vendor that does NOT
	// claim a given monitor still falls through to confidence for it.
	char preferred_id[64] = {0};
	bool have_preferred = target_plugin_get_preferred(preferred_id, sizeof(preferred_id));

	/*
	 * Multi-screen M4 (#793 phase 3): DXR_SCREEN_PLUGIN pins ONE monitor to a
	 * plug-in, outranking the global rules below for that monitor only. Read
	 * per resolve (it is a test/bring-up knob, and resolve is rare).
	 */
	struct target_screen_pins pins;
	uint32_t pins_malformed = 0;
	target_screen_pin_parse(getenv("DXR_SCREEN_PLUGIN"), &pins, &pins_malformed);
	bool pin_used[TARGET_SCREEN_PIN_MAX] = {false};
	if (pins_malformed > 0) {
		U_LOG_W(
		    "plugin loader: DXR_SCREEN_PLUGIN has %u malformed entr%s (expected "
		    "<output|connector|monitor-id-hex>=<plugin-id>[,...]) — ignored",
		    pins_malformed, pins_malformed == 1 ? "y" : "ies");
	}

	// Monitors claimed "present, not ready" in this resolve (log edges).
	uint64_t nr_now[XRT_DP_REGISTRY_MAX_ENTRIES];
	uint32_t nr_count = 0;

	/*
	 * The winner per monitor (target_screen_pick, unit-tested): a per-screen
	 * pin, then PreferredPlugin (#791), then the active plug-in (#1521), then
	 * highest confidence with ties to the lower ProbeOrder (sources are in
	 * ascending ProbeOrder).
	 */
	for (uint32_t d = 0; d < dn; d++) {
		const struct xrt_display_descriptor *desc = &descriptors[d];
		const struct plugin_display_source *best_src = NULL;
		const struct xrt_display_claim *best_claim = NULL;

		// Every source's claim on this monitor, in source order.
		struct target_screen_candidate cands[TARGET_PLUGIN_MAX_SOURCES];
		const struct plugin_display_source *cand_src[TARGET_PLUGIN_MAX_SOURCES];
		const struct xrt_display_claim *cand_claim[TARGET_PLUGIN_MAX_SOURCES];
		uint32_t cand_s[TARGET_PLUGIN_MAX_SOURCES];
		uint32_t cand_c[TARGET_PLUGIN_MAX_SOURCES];
		const uint32_t nc = target_screen_collect_candidates(src_view, src_count, desc->monitor_id, cands,
		                                                     cand_s, cand_c, TARGET_PLUGIN_MAX_SOURCES);
		for (uint32_t i = 0; i < nc; i++) {
			cand_src[i] = &g_display_sources[cand_s[i]];
			cand_claim[i] = &src_claims[cand_s[i]][cand_c[i]];
		}

		// The dashboard's per-screen selector lists exactly these: the
		// plug-ins that can drive this monitor. Complete only when every
		// loadable plug-in was asked (not the POSIX active-only shortcut).
		for (uint32_t i = 0; i < g_monitor_side_count; i++) {
			if (g_monitor_side[i].monitor_id != desc->monitor_id) {
				continue;
			}
			for (uint32_t k = 0; k < nc; k++) {
				(void)snprintf(g_monitor_side[i].cands[k].plugin_id,
				               sizeof(g_monitor_side[i].cands[k].plugin_id), "%s", cands[k].plugin_id);
				g_monitor_side[i].cands[k].confidence = cands[k].confidence;
			}
			g_monitor_side[i].cand_count = nc;
			g_monitor_side[i].cands_valid = !g_display_sources_active_only;
			break;
		}

		// This monitor's names, for the pin match (the side table is
		// guarded by g_refresh_mutex, which this function holds).
		const char *out_name = "";
		const char *conn_name = "";
		for (uint32_t i = 0; i < g_monitor_side_count; i++) {
			if (g_monitor_side[i].monitor_id == desc->monitor_id) {
				out_name = g_monitor_side[i].mon.output_name;
				conn_name = g_monitor_side[i].mon.connector;
				break;
			}
		}
		// Display dashboard phase 7: the per-screen preference, by the
		// monitor's stable key (env > per-user file > machine).
		const char *screen_key = "";
		const char *legacy_key = "";
		for (uint32_t i = 0; i < g_monitor_side_count; i++) {
			if (g_monitor_side[i].monitor_id == desc->monitor_id) {
				screen_key = g_monitor_side[i].key;
				legacy_key = g_monitor_side[i].legacy_key;
				break;
			}
		}
		char screen_pref[64] = {0};
		enum u_setting_source pref_source = U_SETTING_SOURCE_DEFAULT;
		const bool have_screen_pref =
		    screen_pref_lookup(screen_key, legacy_key, screen_pref, sizeof(screen_pref), &pref_source);

		const int pin_idx = target_screen_pin_find(&pins, desc->monitor_id, out_name, conn_name);
		const char *pin_plugin = pin_idx >= 0 ? pins.pin[pin_idx].plugin_id : NULL;
		if (pin_idx >= 0) {
			pin_used[pin_idx] = true;
		}

		enum target_screen_pick_reason reason = TARGET_SCREEN_PICK_NONE;
		bool pin_unclaimed = false;
		bool pref_unclaimed = false;
		const int pick = target_screen_pick_ex(cands, nc, pin_plugin, have_screen_pref ? screen_pref : NULL,
		                                       have_preferred ? preferred_id : NULL, &reason, &pin_unclaimed,
		                                       &pref_unclaimed);
		if (pref_unclaimed) {
			// The named plug-in is not loaded, or it has no claim on this monitor.
			bool loaded = false;
			for (int s = 0; s < g_display_source_count && !loaded; s++) {
				const struct xrt_plugin_iface *sif = g_display_sources[s].iface;
				loaded = sif != NULL && sif->id != NULL && screen_ids_equal(sif->id, screen_pref);
			}
			U_LOG_W("plugin loader: per-screen preference '%s' -> '%s' ignored: %s", screen_key,
			        screen_pref,
			        loaded ? "that plug-in has no claim on this screen"
			               : "that plug-in is not loaded (not registered, or it failed to load)");
		}
		if (pin_unclaimed) {
			U_LOG_W(
			    "plugin loader: DXR_SCREEN_PLUGIN pins monitor 0x%016llx ('%s'/'%s') to '%s', which has no "
			    "claim on it — the pin is ignored for this monitor",
			    (unsigned long long)desc->monitor_id, out_name, conn_name, pin_plugin);
		}
		if (pick >= 0) {
			best_src = cand_src[pick];
			best_claim = cand_claim[pick];
		}

		// What confidence alone would pick, for the override logs.
		int conf_pick = -1;
		for (uint32_t i = 0; i < nc; i++) {
			if (conf_pick < 0 || cands[i].confidence > cands[conf_pick].confidence) {
				conf_pick = (int)i;
			}
		}

		if (reason == TARGET_SCREEN_PICK_SCREEN_PREF) {
			// Once per resolve (rare: instance create, re-probe), and it is the
			// one line that explains a screen woven by a non-default plug-in.
			bool over_active = false;
			for (uint32_t i = 0; i < nc && !cands[pick].is_active; i++) {
				over_active |= cands[i].is_active;
			}
			U_LOG_W(
			    "plugin loader: monitor 0x%016llx (screen '%s') -> '%s' by a per-screen preference (%s; "
			    "confidence=%u)%s",
			    (unsigned long long)desc->monitor_id, screen_key, cands[pick].plugin_id,
			    u_setting_source_str(pref_source), cands[pick].confidence,
			    over_active ? " — the active plug-in's claim on it yields to the explicit preference" : "");
		} else if (reason == TARGET_SCREEN_PICK_PIN) {
			U_LOG_I(
			    "plugin loader: monitor 0x%016llx ('%s'/'%s') → '%s' by DXR_SCREEN_PLUGIN (confidence=%u; "
			    "outranks PreferredPlugin and the active plug-in for this monitor)",
			    (unsigned long long)desc->monitor_id, out_name, conn_name, cands[pick].plugin_id,
			    cands[pick].confidence);
		} else if (reason == TARGET_SCREEN_PICK_PREFERRED) {
			U_LOG_W(
			    "plugin loader: monitor 0x%016llx → PreferredPlugin override '%s' "
			    "(forced over EDID confidence)",
			    (unsigned long long)desc->monitor_id, preferred_id);
		} else if (reason == TARGET_SCREEN_PICK_BETTER_RANKED) {
			const char *active_name = "?";
			for (uint32_t i = 0; i < nc; i++) {
				if (cands[i].is_active) {
					active_name = cands[i].plugin_id;
				}
			}
			U_LOG_W(
			    "plugin loader: monitor 0x%016llx → '%s' (confidence=%u, ProbeOrder=%u, ranked before "
			    "the active plug-in) — the active plug-in '%s' claims it only as a fallback: a panel "
			    "that came up after instance create (display hotplug, ADR-047 Amendment 8)",
			    (unsigned long long)desc->monitor_id, cands[pick].plugin_id, cands[pick].confidence,
			    cand_src[pick]->probe_order, active_name);
		} else if (reason == TARGET_SCREEN_PICK_ACTIVE && conf_pick >= 0 && conf_pick != pick) {
			// One-off per monitor, and only when the override changes the outcome.
			U_LOG_W(
			    "plugin loader: monitor 0x%016llx → ACTIVE plug-in '%s' "
			    "(confidence=%u, ProbeOrder=%u) forced over '%s' (confidence=%u) — "
			    "the weaving DP follows the active plug-in so registry-routed APIs "
			    "(GL, service D3D11) match the scalar-routed ones (#1521)",
			    (unsigned long long)desc->monitor_id, cands[pick].plugin_id, cands[pick].confidence,
			    cand_src[pick]->probe_order, cands[conf_pick].plugin_id, cands[conf_pick].confidence);
		}
		if (best_claim == NULL) {
			continue; // no plug-in claimed this monitor
		}
		struct xrt_dp_registry_entry *e = &out_registry->entries[out_registry->entry_count++];
		fill_registry_entry(e, desc, best_src, best_claim);
		for (uint32_t i = 0; i < g_monitor_side_count; i++) {
			if (g_monitor_side[i].monitor_id == desc->monitor_id) {
				e->platform_id = g_monitor_side[i].mon.platform_id;
				break;
			}
		}
		// Display hotplug (ADR-047 Amendment 8): "present, not ready" — the
		// plug-in owns the panel, but no display processor may touch it yet:
		// no factory, so every compositor shows the screen flat 2D.
		// DXR_TEST_SCREEN_NOT_READY=<output|connector|id-hex> forces it (test).
		{
			struct target_screen_pins nr;
			char spec[96];
			const char *t = getenv("DXR_TEST_SCREEN_NOT_READY");
			(void)snprintf(spec, sizeof(spec), "%s=x", t != NULL ? t : "");
			const bool forced_nr = t != NULL && t[0] != '\0' &&
			                       target_screen_pin_parse(spec, &nr, NULL) == 1 &&
			                       target_screen_pin_find(&nr, desc->monitor_id, out_name, conn_name) == 0;
			if (best_claim->confidence == (uint32_t)XRT_DISPLAY_CLAIM_PRESENT_NOT_READY || forced_nr) {
				e->not_ready = true;
				e->dp_factory_vk = NULL;
				e->dp_factory_gl = NULL;
				e->dp_factory_d3d11 = NULL;
				e->dp_factory_d3d12 = NULL;
				e->dp_factory_metal = NULL;
				// WARN when a monitor enters the state; repeats (follow-up sweeps) at INFO.
				bool was_nr = false;
				for (uint32_t k = 0; k < g_not_ready_prev_count && !was_nr; k++) {
					was_nr = g_not_ready_prev[k] == desc->monitor_id;
				}
				if (nr_count < XRT_DP_REGISTRY_MAX_ENTRIES) {
					nr_now[nr_count++] = desc->monitor_id;
				}
				if (was_nr) {
					U_LOG_I(
					    "plugin loader: monitor 0x%016llx → '%s' still PRESENT, NOT READY (flat "
					    "2D)",
					    (unsigned long long)desc->monitor_id, e->plugin_id);
				} else {
					U_LOG_W(
					    "plugin loader: monitor 0x%016llx → '%s' PRESENT, NOT READY%s — no display "
					    "processor, the screen shows flat 2D until the plug-in claims it for real",
					    (unsigned long long)desc->monitor_id, e->plugin_id,
					    forced_nr ? " (DXR_TEST_SCREEN_NOT_READY)" : "");
				}
			}
		}
		if (have_screen_pref) {
			(void)snprintf(e->preferred_plugin, sizeof(e->preferred_plugin), "%s", screen_pref);
			e->preferred_source = (uint32_t)pref_source;
		}
		if (reason == TARGET_SCREEN_PICK_SCREEN_PREF) {
			e->forced = true;
			e->forced_source = (uint32_t)pref_source;
		}
		U_LOG_I("plugin loader: monitor 0x%016llx → plug-in '%s' (confidence=%u)",
		        (unsigned long long)e->monitor_id, e->plugin_id, e->confidence);
		if (reason == TARGET_SCREEN_PICK_PIN && e->dp_factory_vk == NULL && e->dp_factory_gl == NULL &&
		    e->dp_factory_d3d11 == NULL && e->dp_factory_d3d12 == NULL && e->dp_factory_metal == NULL) {
			U_LOG_W(
			    "plugin loader: DXR_SCREEN_PLUGIN pinned monitor 0x%016llx to '%s', but it offers no "
			    "display-processor factory for it — that screen will not be woven",
			    (unsigned long long)e->monitor_id, e->plugin_id);
		}
	}

	for (uint32_t i = 0; i < nr_count; i++) {
		g_not_ready_prev[i] = nr_now[i];
	}
	g_not_ready_prev_count = nr_count;

	for (uint32_t i = 0; i < pins.count; i++) {
		if (!pin_used[i]) {
			U_LOG_W(
			    "plugin loader: DXR_SCREEN_PLUGIN entry '%s=%s' names no known monitor (match an output "
			    "or connector name, e.g. HDMI-1 / HDMI-A-1, or a monitor id in hex)",
			    pins.pin[i].match, pins.pin[i].plugin_id);
		}
	}

	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
	}
}

void
target_plugin_release_claim_sources(void)
{
#if !defined(XRT_OS_WINDOWS) && !defined(XRT_OS_ANDROID)
	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}
	for (int s = 0; s < g_display_source_count; s++) {
		struct plugin_display_source *src = &g_display_sources[s];
		// The active plug-in's instance belongs to discovery and outlives
		// any one xrt_instance; only the claim-only instances are ours.
		// destroy() pairs with the probe() that made the instance, even when
		// probe() handed back NULL (a plug-in with process-global state).
		if (src->iface == NULL || src->iface == g_active_iface) {
			continue;
		}
		if (src->iface->destroy != NULL) {
			U_LOG_I("plugin loader: releasing display-claim source id=%s",
			        src->iface->id != NULL ? src->iface->id : "?");
			src->iface->destroy(src->inst);
		}
		src->inst = NULL;
	}
	// The next resolve re-collects (and re-probes) from scratch. The dlopen
	// handles stay loaded, as everywhere else in this loader: a re-collection
	// just takes another reference.
	g_display_source_count = -1;
	g_display_sources_active_only = false;
	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
	}
#endif
}

uint32_t
target_plugin_rescan_claim_sources(void)
{
#if !defined(XRT_OS_ANDROID)
	(void)target_plugin_get_active();
	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}
	uint32_t added = 0;
	if (g_display_source_count >= 0) {
		// A user-pinned active plug-in (PreferredPlugin == active) wins every
		// monitor it claims (rule 2), and the shortcut set means it claimed
		// them all: loading the others could change nothing, and #1874 keeps
		// them (and the vendor service they reach) out of the process. Any
		// other set loads the rest now — a better-ranked plug-in may have
		// found a hotplugged panel (rule 3's carve-out).
		char preferred[64] = {0};
		const bool pinned_active = target_plugin_get_preferred(preferred, sizeof(preferred)) &&
		                           g_active_iface != NULL && g_active_iface->id != NULL &&
		                           strcmp(preferred, g_active_iface->id) == 0;
		if (!(g_display_sources_active_only && pinned_active)) {
			const int before = g_display_source_count;
			g_display_source_count = rescan_display_sources_platform(
			    g_display_sources, g_display_source_count, TARGET_PLUGIN_MAX_SOURCES);
			added = (uint32_t)(g_display_source_count - before);
			// Every loadable plug-in has now been asked: the set is the full one.
			g_display_sources_active_only = false;
		}
	}
	// g_display_source_count < 0: never collected; the next resolve collects
	// (and so probes) every plug-in from scratch.
	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
	}
	return added;
#else
	// Android: one plug-in, no claim-only sources. (Windows rescans like
	// POSIX since #2305: rescan_display_sources_platform walks the same
	// registry entries discovery does, so a vendor plug-in that declined at
	// instance create — no panel then — is asked again on a hotplug.)
	return 0;
#endif
}
