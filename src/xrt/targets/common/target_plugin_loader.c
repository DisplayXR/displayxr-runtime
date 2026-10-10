// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-side registry-driven loader for vendor plug-in DLLs.
 *
 * Enumerates `HKLM\Software\DisplayXR\DisplayProcessors\*`, sorts by
 * `ProbeOrder`, then `LoadLibraryExW` → `GetProcAddress` → negotiate
 * → probe each in turn. First plug-in whose probe returns
 * `XRT_SUCCESS` wins and is cached. The plug-in's chosen instance
 * handle (returned by `probe()`) is kept alongside the iface so the
 * runtime can hand it back to subsequent vtable calls.
 *
 * v1 hosts at most one active plug-in per process; later sequencing
 * steps may relax that for multi-display heterogeneous setups, per
 * `docs/roadmap/vendor-plugin-architecture.md` §3 non-goals.
 *
 * Issue #256.
 *
 * @ingroup target_common
 */

#include "target_plugin_loader.h"
#include "target_screen_pin.h"
#include "xrt/xrt_config_have.h"

#ifdef XRT_HAVE_VULKAN
/* #1243 — Vulkan-free prototypes shared with the defining TU (vk_helpers.c),
 * so the declarations cannot silently diverge from the definitions. */
#include "vk/vk_abi_fingerprint.h"
#endif
#include "target_plugin_preload_sanitize.h"
#include "target_plugin_path_guard.h"

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_config_os.h"

#include "os/os_threading.h"
#include "os/os_display_edid.h"
#include "os/os_display_desktop.h"
#include "util/u_logging.h"
#include "util/u_setting.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
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
	status_lock();
	int i = status_find_or_add_locked(id != NULL ? id : "?");
	if (i >= 0) {
		struct target_plugin_status *s = &g_status[i];
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
 * Platform-specific loader.
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
 * Maximum number of plug-ins we enumerate from the registry. 16 is
 * generous — the design admits at most a handful in practice (vendor
 * panels + the sim_display fallback).
 */
#define MAX_PLUGIN_ENTRIES 16

/*!
 * Registry-derived per-plug-in metadata. Filled in by the enumeration
 * pass, sorted, then iterated by the probe pass.
 */
struct plugin_entry
{
	wchar_t binary_path[MAX_PATH];
	char id[64];           /* subkey name (UTF-8) */
	char display_name[128];
	char vendor[64];
	char version[64];
	uint32_t probe_order; /* defaults to 100 when REG value is missing */
};


/*
 *
 * Helpers.
 *
 */

/*
 * (Earlier revisions of this loader called AddDllDirectory(runtime dir)
 * before LoadLibraryExW with LOAD_LIBRARY_SEARCH_USER_DIRS, but that flag
 * is mutually exclusive with LOAD_WITH_ALTERED_SEARCH_PATH per MSDN —
 * combining them returns ERROR_INVALID_PARAMETER for plug-ins that have
 * transitive deps the loader had to actually walk. We rely on
 * LOAD_WITH_ALTERED_SEARCH_PATH alone now: it puts the plug-in DLL's
 * own directory first in the search, and the legacy strategy continues
 * to PATH, so SR DLLs at `C:\Program Files\LeiaSR\Platform\bin` and
 * cjson.dll / vulkan-1.dll alongside the runtime resolve through their
 * existing PATH entries.)
 */

/*!
 * UTF-16 wchar_t string → UTF-8 char buffer. Returns true on success.
 */
static bool
wide_to_utf8(const wchar_t *src, char *dst, int dst_size)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, NULL, NULL);
	if (n <= 0) {
		if (dst_size > 0) {
			dst[0] = '\0';
		}
		return false;
	}
	return true;
}

/*!
 * Read an optional REG_SZ value into the supplied UTF-8 buffer. On
 * miss, leaves dst as an empty string.
 */
static void
read_optional_string(HKEY key, const wchar_t *name, char *dst, int dst_size)
{
	wchar_t wbuf[256];
	DWORD wbuf_bytes = sizeof(wbuf);
	if (RegGetValueW(key, NULL, name, RRF_RT_REG_SZ, NULL, wbuf, &wbuf_bytes) == ERROR_SUCCESS) {
		wide_to_utf8(wbuf, dst, dst_size);
	} else {
		if (dst_size > 0) {
			dst[0] = '\0';
		}
	}
}

/*!
 * Enumerate plug-in subkeys under HKLM\Software\DisplayXR\DisplayProcessors.
 *
 * Returns the number of entries populated (0 if the root key is
 * absent / unreadable). Entries with no Binary value are skipped —
 * they don't satisfy the spec's "required" rule.
 */
static int
enumerate_registry(struct plugin_entry *entries, int max)
{
	HKEY root;
	LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
	                           L"Software\\DisplayXR\\DisplayProcessors",
	                           0, KEY_READ, &root);
	if (rc != ERROR_SUCCESS) {
		U_LOG_I("plugin loader: registry root HKLM\\Software\\DisplayXR\\DisplayProcessors absent (rc=%ld) "
		        "— no plug-ins to try.",
		        rc);
		return 0;
	}

	int count = 0;
	DWORD index = 0;
	for (;;) {
		if (count >= max) {
			U_LOG_W("plugin loader: more than %d registered plug-ins — truncating.", max);
			break;
		}

		wchar_t subkey_name[256];
		DWORD subkey_name_len = (DWORD)(sizeof(subkey_name) / sizeof(wchar_t));
		LSTATUS er = RegEnumKeyExW(root, index, subkey_name, &subkey_name_len, NULL, NULL, NULL, NULL);
		index++;
		if (er == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (er != ERROR_SUCCESS) {
			U_LOG_W("plugin loader: RegEnumKeyEx failed (rc=%ld) at index %lu — stopping enumeration.",
			        er, (unsigned long)index);
			break;
		}

		HKEY sub;
		if (RegOpenKeyExW(root, subkey_name, 0, KEY_READ, &sub) != ERROR_SUCCESS) {
			continue;
		}

		struct plugin_entry *e = &entries[count];
		memset(e, 0, sizeof(*e));

		/* Binary is required. */
		DWORD binary_bytes = (DWORD)sizeof(e->binary_path);
		if (RegGetValueW(sub, NULL, L"Binary", RRF_RT_REG_SZ, NULL, e->binary_path, &binary_bytes) !=
		    ERROR_SUCCESS) {
			U_LOG_W("plugin loader: subkey '%ls' missing required 'Binary' value — skipping.",
			        subkey_name);
			RegCloseKey(sub);
			continue;
		}

		wide_to_utf8(subkey_name, e->id, (int)sizeof(e->id));

		read_optional_string(sub, L"DisplayName", e->display_name, (int)sizeof(e->display_name));
		read_optional_string(sub, L"Vendor", e->vendor, (int)sizeof(e->vendor));
		read_optional_string(sub, L"Version", e->version, (int)sizeof(e->version));

		DWORD order = 100;
		DWORD order_bytes = sizeof(order);
		if (RegGetValueW(sub, NULL, L"ProbeOrder", RRF_RT_REG_DWORD, NULL, &order, &order_bytes) ==
		    ERROR_SUCCESS) {
			e->probe_order = (uint32_t)order;
		} else {
			e->probe_order = 100;
		}

		count++;
		RegCloseKey(sub);
	}

	RegCloseKey(root);
	return count;
}

/*!
 * ProbeOrder ascending, ties broken by id (#1466) — same shape, same reason as
 * the input loader's comparator: qsort is not stable, so two plug-ins
 * registered at one ProbeOrder would otherwise be ranked by the sort
 * implementation. Which one is probed first decides which DP the runtime ends
 * up on, so it must not vary between runs.
 */
static int
compare_by_probe_order(const void *a, const void *b)
{
	const struct plugin_entry *ea = (const struct plugin_entry *)a;
	const struct plugin_entry *eb = (const struct plugin_entry *)b;
	if (ea->probe_order < eb->probe_order) {
		return -1;
	}
	if (ea->probe_order > eb->probe_order) {
		return 1;
	}
	return strcmp(ea->id, eb->id);
}

/*!
 * Try one registered plug-in. Returns the iface on success and writes
 * the probed instance handle to *out_inst; returns NULL (and closes
 * the DLL) on any failure. Caller never sees the HMODULE — successful
 * loads intentionally leak it for the process lifetime so the iface's
 * function pointers remain callable.
 */
/*!
 * Registry↔DLL version-skew tripwire (#461). Installers replace the plug-in
 * DLL and rewrite the registry `Version` in one transaction — EXCEPT when a
 * running process (typically displayxr-service) has the DLL mapped: NSIS
 * silent installs skip locked files with exit 0, leaving an old DLL on disk
 * under a new registry version (the v0.14.0 bundle incident). Compare the
 * registry semver against the DLL's embedded VERSIONINFO and WARN loudly on
 * mismatch. Quietly does nothing when the DLL ships no version resource
 * (sim-display today) or the registry value isn't a semver.
 */
static void
check_registry_dll_version_skew(const struct plugin_entry *e)
{
	unsigned reg_major = 0, reg_minor = 0, reg_patch = 0;
	const char *v = e->version;
	if (*v == 'v') {
		v++;
	}
	if (sscanf(v, "%u.%u.%u", &reg_major, &reg_minor, &reg_patch) != 3) {
		return; /* empty / non-semver registry Version — nothing to compare */
	}

	DWORD ignored = 0;
	DWORD size = GetFileVersionInfoSizeW(e->binary_path, &ignored);
	if (size == 0) {
		return; /* DLL ships no VERSIONINFO resource — nothing to compare */
	}
	void *buf = malloc(size);
	if (buf == NULL) {
		return;
	}
	VS_FIXEDFILEINFO *ffi = NULL;
	UINT ffi_len = 0;
	if (GetFileVersionInfoW(e->binary_path, 0, size, buf) &&
	    VerQueryValueW(buf, L"\\", (LPVOID *)&ffi, &ffi_len) && ffi != NULL &&
	    ffi_len >= sizeof(*ffi)) {
		unsigned dll_major = HIWORD(ffi->dwFileVersionMS);
		unsigned dll_minor = LOWORD(ffi->dwFileVersionMS);
		unsigned dll_patch = HIWORD(ffi->dwFileVersionLS);
		if (dll_major != reg_major || dll_minor != reg_minor || dll_patch != reg_patch) {
			U_LOG_W("plugin loader:   %s: registry Version '%s' != DLL file version "
			        "%u.%u.%u (%ls) — an installer likely failed to replace a locked "
			        "DLL; reinstall the plug-in with displayxr-service stopped (#461).",
			        e->id, e->version, dll_major, dll_minor, dll_patch, e->binary_path);
		}
	}
	free(buf);
}

/*!
 * Load + negotiate + ABI-check + probe one registered plug-in. Returns the
 * iface (writing the instance to *out_inst and the negotiated ABI version to
 * *out_version) on success, NULL (and closes the DLL) on any failure. No
 * "active plug-in" logging — callers add their own role-specific line, so
 * this can serve both the single-winner discovery and the load-all display
 * probe (#69). Successful loads intentionally leak the HMODULE for the
 * process lifetime so the iface's function pointers remain callable.
 */
static const struct xrt_plugin_iface *
load_and_probe_one(const struct plugin_entry *e,
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

	// ADR-045: an orphan registration (Binary deleted, key left behind) is a
	// fact to skip, not an error to repeat — one WARN per process.
	if (GetFileAttributesW(e->binary_path) == INVALID_FILE_ATTRIBUTES) {
		DWORD gle = GetLastError();
		if (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND) {
			bool changed = status_finish(e->id, TARGET_PLUGIN_RESULT_BINARY_MISSING, (uint32_t)gle, NULL);
			PLUGIN_LOG_OUTCOME(changed,
			                   "plugin loader:   %s: registered Binary '%ls' does not exist — skipping "
			                   "(orphan registration).",
			                   e->id, e->binary_path);
			snprintf(g_last_reject_reason, sizeof(g_last_reject_reason),
			         "registered Binary does not exist (orphan registration)");
			return NULL;
		}
	}

	// #952: refuse a build-tree/worktree DLL path (the #943 footgun) unless
	// DXR_ALLOW_DEV_PLUGIN_PATHS is set; warn on other non-install paths.
	if (target_plugin_path_check(e->binary_path, e->id, "plugin") == TARGET_PLUGIN_PATH_REFUSED) {
		status_finish(e->id, TARGET_PLUGIN_RESULT_PATH_REFUSED, 0, "dev/build-tree path refused (#952)");
		return NULL;
	}

	// Pre-load any dependency whose unwind data would crash host-engine
	// module tracers during this LoadLibrary (issue #434) — loaded from a
	// path those tracers' filters skip, so the buggy parse never runs.
	target_plugin_sanitized_preload(e->binary_path);

	// One-shot breadcrumb: a host-side crash during the load below (DLL
	// notification callbacks run host code) leaves this as the last line
	// in the per-app log, naming the in-flight binary (issue #434). WARN on
	// the first attempt per process; re-probes (ADR-045) log it at INFO.
	PLUGIN_LOG_OUTCOME(first_attempt, "plugin loader:   %s: loading plug-in binary %ls", e->id, e->binary_path);

	// #461: warn if the registry-declared version doesn't match the DLL on
	// disk (an installer skipped a locked file). Diagnostic only — the ABI
	// negotiation below remains the actual compatibility gate.
	if (first_attempt) {
		check_registry_dll_version_skew(e);
	}

	HMODULE dll = LoadLibraryExW(e->binary_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (dll == NULL) {
		DWORD gle = GetLastError();
		// The binary exists (checked above), so ERROR_MOD_NOT_FOUND means a
		// library IT imports is missing — typically the vendor platform the
		// plug-in should resolve lazily (ADR-045 rule 1).
		const bool dep = gle == ERROR_MOD_NOT_FOUND;
		bool changed = status_finish(
		    e->id, dep ? TARGET_PLUGIN_RESULT_DEPENDENCY_MISSING : TARGET_PLUGIN_RESULT_LOAD_FAILED,
		    (uint32_t)gle, NULL);
		PLUGIN_LOG_OUTCOME(changed, "plugin loader:   %s: LoadLibrary(%ls) failed (err=%lu)%s.", e->id,
		                   e->binary_path, gle,
		                   dep ? " — a library the plug-in imports is missing (vendor platform not installed, "
		                         "or not resolvable from this process)"
		                       : "");
		snprintf(g_last_reject_reason, sizeof(g_last_reject_reason), "%s (err=%lu)",
		         dep ? "dependency missing: a library the plug-in imports was not found" : "LoadLibrary failed",
		         gle);
		return NULL;
	}

	xrt_plugin_negotiate_fn_t negotiate =
	    (xrt_plugin_negotiate_fn_t)(void *)GetProcAddress(dll, XRT_PLUGIN_ENTRYPOINT_NAME);
	if (negotiate == NULL) {
		U_LOG_W("plugin loader:   %s: missing entry point '%s' — skipping.", e->id,
		        XRT_PLUGIN_ENTRYPOINT_NAME);
		status_finish(e->id, TARGET_PLUGIN_RESULT_NO_ENTRY_POINT, 0, NULL);
		FreeLibrary(dll);
		return NULL;
	}

	struct xrt_plugin_host_iface host = {0};
	host.struct_size = (uint32_t)sizeof(struct xrt_plugin_host_iface);
	host.host_api_version = XRT_PLUGIN_API_VERSION_CURRENT;
#if defined(XRT_OS_ANDROID)
	// Android plug-ins need the host's JavaVM/activity for vendor-SDK init
	// (their own statically-linked aux_android copy is never populated).
	host.get_android_vm = plugin_host_get_android_vm;
	// Activity when in-process; Service Context when out-of-process (CNSDK only
	// needs an android.content.Context to bind the on-device tracking service).
	host.get_android_activity = plugin_host_get_android_activity;
	// #1037: a Context whose classloader is the runtime APK's, so an
	// in-process vendor plug-in can resolve vendor Java glue the app does
	// not ship. Class loading only — see xrt_plugin.h.
	host.get_android_class_host_context = plugin_host_get_android_class_host_context;
	host.android_package_is_visible = plugin_host_android_package_is_visible;
#endif

	struct xrt_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = negotiate(XRT_PLUGIN_API_VERSION_CURRENT, &host, &iface, &plugin_version);
	if (xret != XRT_SUCCESS || iface == NULL) {
		U_LOG_W("plugin loader:   %s: negotiate returned %d (iface=%p) — skipping.", e->id,
		        (int)xret, (void *)iface);
		status_finish(e->id, TARGET_PLUGIN_RESULT_NEGOTIATE_FAILED, 0, NULL);
		FreeLibrary(dll);
		return NULL;
	}

	/* ADR-020 rule 3: reject a major-version mismatch before touching the
	 * vtable. A plug-in built against a different ABI major lays its vtable
	 * out at offsets the runtime doesn't agree on — calling through it is
	 * exactly the corruption this guards against. Skip it (the caller falls
	 * back to the next plug-in / sim_display); never dispatch. */
	if (plugin_version != XRT_PLUGIN_API_VERSION_CURRENT) {
		U_LOG_E("plugin loader:   %s: ABI major mismatch — plugin_api=%u, runtime expects %u; "
		        "the plug-in must be rebuilt against this runtime's headers — skipping (ADR-020 rule 3).",
		        e->id, plugin_version, (unsigned)XRT_PLUGIN_API_VERSION_CURRENT);
		/* #1212: carry the reason to the self-test, so a rejected vendor
		 * plug-in reads as an ABI mismatch rather than a bare failure. */
		snprintf(g_last_reject_reason, sizeof(g_last_reject_reason),
		         "ABI mismatch: plug-in reports v%u, runtime expects v%u (rebuild it)", plugin_version,
		         (unsigned)XRT_PLUGIN_API_VERSION_CURRENT);
		status_finish(e->id, TARGET_PLUGIN_RESULT_ABI_MISMATCH, 0, g_last_reject_reason);
		FreeLibrary(dll);
		return NULL;
	}

	// ADR-045: ask BEFORE probe(), so a plug-in about to decline can say why.
	status_record_platform_state(e->id, iface);

	if (iface->probe != NULL) {
		xret = iface->probe(out_inst);
		if (xret == XRT_ERROR_PROBER_NOT_SUPPORTED) {
			U_LOG_I("plugin loader:   %s: probe declined (no matching device).", e->id);
			status_finish(e->id, TARGET_PLUGIN_RESULT_DECLINED, 0, NULL);
			/* #1212: a plug-in that LOADED and then said "not my
			 * hardware" is behaving correctly on a box without that
			 * panel. Only a failed LOAD is a misconfiguration, so
			 * mark this so the vendor_dp self-test does not fail a
			 * dev box that merely has a vendor plug-in registered. */
			g_last_reject_declined = true;
			FreeLibrary(dll);
			return NULL;
		}
		if (xret != XRT_SUCCESS) {
			bool changed = status_finish(e->id, TARGET_PLUGIN_RESULT_PROBE_FAILED, (uint32_t)xret, NULL);
			PLUGIN_LOG_OUTCOME(changed, "plugin loader:   %s: probe returned %d — skipping.", e->id,
			                   (int)xret);
			FreeLibrary(dll);
			return NULL;
		}
	}

	if (out_version != NULL) {
		*out_version = plugin_version;
	}
	status_finish(e->id, TARGET_PLUGIN_RESULT_CLAIMED, 0, NULL);
	return iface;
}

/*!
 * Try one registered plug-in as the single active winner. Thin wrapper over
 * @ref load_and_probe_one that adds the canonical "active plug-in:" log line
 * (parsed by diagnostics). Returns the iface on success, NULL on any failure.
 */
static const struct xrt_plugin_iface *
try_load_one(const struct plugin_entry *e, struct xrt_plugin_instance **out_inst)
{
	uint32_t plugin_version = 0;
	const struct xrt_plugin_iface *iface = load_and_probe_one(e, out_inst, &plugin_version);
	if (iface == NULL) {
		return NULL;
	}

	U_LOG_W(
	    "plugin loader: active plug-in: id=%s name='%s' vendor='%s' version='%s' "
	    "plugin_api=%u probe_order=%u path=%ls",
	    iface->id ? iface->id : e->id, iface->display_name ? iface->display_name : e->display_name,
	    iface->vendor ? iface->vendor : e->vendor, e->version, plugin_version, e->probe_order,
	    e->binary_path);
	status_set_active(e->id);

	return iface;
}

/*!
 * Pin the runtime core DLL (DisplayXRClient.dll) into the process by
 * absolute path before any plug-in is loaded. Runs once.
 *
 * Vendor plug-in DLLs statically import DisplayXRClient.dll, which ships
 * in the runtime install dir. That dir is intentionally NOT on PATH
 * (issue #104), and the per-plug-in LOAD_WITH_ALTERED_SEARCH_PATH below
 * searches only the plug-in's own dir + System32 + cwd + PATH — none of
 * which hold DisplayXRClient.dll except cwd when it happens to equal the
 * runtime dir. So at logon (Run-key launches with cwd=System32) and when
 * the workspace controller spawns the service, that import fails with
 * ERROR_MOD_NOT_FOUND (126) and every plug-in load aborts → no display
 * processor → the service exits (issue #328).
 *
 * Pre-loading DisplayXRClient.dll by absolute path here pins it into the
 * process by base name; the loader then satisfies each plug-in's import
 * from the already-resident module with no disk search, independent of
 * cwd/PATH. The per-plug-in load deliberately keeps
 * LOAD_WITH_ALTERED_SEARCH_PATH so vendor side DLLs (e.g. the SR platform
 * at C:\Program Files\LeiaSR\Platform\bin) still resolve via PATH.
 *
 * No process-wide search-order mutation (cf. target_dll_init.c) — this is
 * the surgical option and leaves PATH resolution intact for vendors.
 */
static void
preload_runtime_core_dll(void)
{
	static int done = 0;
	if (done) {
		return;
	}
	done = 1;

	// Directory of whatever module this loader code lives in — the
	// service exe or DisplayXRClient.dll, both of which sit in the
	// runtime install dir. UNCHANGED_REFCOUNT: we only want its path.
	HMODULE self = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)(void *)&preload_runtime_core_dll, &self)) {
		U_LOG_W("plugin loader: GetModuleHandleEx(self) failed (err=%lu) — cannot locate runtime dir to "
		        "pre-load DisplayXRClient.dll.",
		        GetLastError());
		return;
	}

	wchar_t modpath[MAX_PATH];
	DWORD len = GetModuleFileNameW(self, modpath, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		U_LOG_W("plugin loader: GetModuleFileName(self) failed/truncated — skipping DisplayXRClient.dll "
		        "pre-load.");
		return;
	}

	// Strip the module filename to leave the runtime dir.
	wchar_t *slash = wcsrchr(modpath, L'\\');
	if (slash == NULL) {
		return;
	}
	*slash = L'\0';

	wchar_t corepath[MAX_PATH];
	if (swprintf_s(corepath, MAX_PATH, L"%s\\DisplayXRClient.dll", modpath) <= 0) {
		return;
	}

	// If it is already resident (in-process app target, where the host
	// loaded it via the OpenXR loader) this just bumps the refcount and
	// the plug-in import binds to it. Otherwise it loads now so the
	// import resolves. LOAD_WITH_ALTERED_SEARCH_PATH so the core DLL's
	// own siblings (cjson.dll, pthreadVC3.dll) resolve from the runtime
	// dir rather than the host exe dir.
	if (LoadLibraryExW(corepath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) == NULL) {
		U_LOG_W("plugin loader: pre-load of %ls failed (err=%lu); plug-in loads may fail when cwd is not "
		        "the runtime dir.",
		        corepath, GetLastError());
	} else {
		U_LOG_I("plugin loader: pinned runtime core DLL for plug-in import resolution: %ls", corepath);
	}
}

void
target_plugin_preload_runtime_core_dll(void)
{
	/* Shared with the input-provider loader: input-provider DLLs import
	 * DisplayXRClient.dll exactly like DP plug-ins do, so the #328
	 * cwd-independent pinning must run before either loader's first
	 * LoadLibrary. Idempotent (one-shot inside). */
	preload_runtime_core_dll();
}

static const struct xrt_plugin_iface *
discover_active_plugin(struct xrt_plugin_instance **out_inst, uint32_t max_probe_order)
{
	*out_inst = NULL;

	// Ensure DisplayXRClient.dll is resident before any plug-in import
	// of it has to resolve (issue #328).
	preload_runtime_core_dll();

	struct plugin_entry entries[MAX_PLUGIN_ENTRIES];
	int n = enumerate_registry(entries, MAX_PLUGIN_ENTRIES);
	if (n == 0) {
		return NULL;
	}

	qsort(entries, (size_t)n, sizeof(entries[0]), compare_by_probe_order);

	// DXR_PLUGIN_EXCLUSIVE (#1545): warn once if the pin matches nothing.
	{
		const char *ids[MAX_PLUGIN_ENTRIES];
		for (int i = 0; i < n; i++) {
			ids[i] = entries[i].id;
		}
		plugin_warn_exclusive_miss(ids, n);
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
		/* #1545: skipped BEFORE LoadLibrary, and deliberately without
		 * plugin_note_reject — an excluded plug-in is not a failure. */
		if (plugin_id_excluded(entries[i].id)) {
			U_LOG_I("plugin loader:   [%d/%d] %s skipped (DXR_PLUGIN_EXCLUSIVE).", i + 1, n, entries[i].id);
			continue;
		}
		U_LOG_I("plugin loader:   [%d/%d] %s (ProbeOrder=%u, %ls)", i + 1, n, entries[i].id,
		        entries[i].probe_order, entries[i].binary_path);
		const struct xrt_plugin_iface *iface = try_load_one(&entries[i], out_inst);
		if (iface != NULL) {
			g_active_probe_order = entries[i].probe_order;
			return iface;
		}
		/* #1212: attempted in ascending ProbeOrder and failed, so this is
		 * by construction a better-ranked plug-in than whatever wins
		 * below. The self-test's vendor_dp check keys off this. */
		plugin_note_reject(entries[i].id, entries[i].probe_order);
	}

	// A refresh (max_probe_order bounded by the current winner) legitimately
	// attempts nothing when nothing better is registered — that is not a
	// fallback, so do not shout (monkey-test F2: 48 false alarms per soak).
	if (max_probe_order == 0xFFFFFFFFu) {
		U_LOG_W("plugin loader: no registered plug-in claimed the system — falling back to static drivers.");
	}
	return NULL;
}

/*!
 * Load EVERY registered plug-in and return them as display-claim sources for
 * the per-monitor registry (#69 / ADR-015). Unlike @ref discover_active_plugin
 * (which stops at the first winner), this consults all of them so the
 * fallback (sim_display) and a vendor plug-in can both contribute claims. The
 * already-active plug-in is reused — not loaded twice — and the remainder are
 * loaded via @ref load_and_probe_one (DLL handles leaked, consistent with the
 * single-winner path). Plug-ins whose `probe()` declines contribute nothing.
 * Returns the source count, sorted ascending by ProbeOrder (so a confidence
 * tie resolves to the lower-ProbeOrder plug-in).
 *
 * Caller must hold @ref g_refresh_mutex and have already loaded the active
 * plug-in (so `g_active_*` are set for the reuse path).
 */
static int
collect_display_sources_platform(struct plugin_display_source *out, int max)
{
	struct plugin_entry entries[MAX_PLUGIN_ENTRIES];
	int n = enumerate_registry(entries, MAX_PLUGIN_ENTRIES);
	if (n == 0) {
		return 0;
	}
	qsort(entries, (size_t)n, sizeof(entries[0]), compare_by_probe_order);

	const char *active_id = (g_active_iface != NULL && g_active_iface->id != NULL) ? g_active_iface->id : NULL;

	int count = 0;
	for (int i = 0; i < n && count < max; i++) {
		// #1545: THE load site this variable exists for. Claim
		// collection consults every registered plug-in, so without this
		// an exclusive lane would still LoadLibrary the vendor DLL (and
		// its GL ICD) even though it never becomes the active DP.
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
	return count;
}

/*
 *
 * Public enumeration + PreferredPlugin override (Windows).
 *
 */

int
target_plugin_enumerate(struct target_plugin_desc *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}

	struct plugin_entry entries[MAX_PLUGIN_ENTRIES];
	int n = enumerate_registry(entries, MAX_PLUGIN_ENTRIES);
	if (n > max) {
		n = max;
	}

	for (int i = 0; i < n; i++) {
		struct target_plugin_desc *d = &out[i];
		memset(d, 0, sizeof(*d));
		snprintf(d->id, sizeof(d->id), "%s", entries[i].id);
		snprintf(d->display_name, sizeof(d->display_name), "%s", entries[i].display_name);
		snprintf(d->vendor, sizeof(d->vendor), "%s", entries[i].vendor);
		snprintf(d->version, sizeof(d->version), "%s", entries[i].version);
		wide_to_utf8(entries[i].binary_path, d->binary_path, (int)sizeof(d->binary_path));
		d->probe_order = entries[i].probe_order;
	}

	return n;
}

bool
target_plugin_get_preferred(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';

	wchar_t wbuf[64];
	DWORD wbuf_bytes = sizeof(wbuf);
	LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\DisplayProcessors", L"PreferredPlugin",
	                          RRF_RT_REG_SZ, NULL, wbuf, &wbuf_bytes);
	if (rc != ERROR_SUCCESS) {
		return false;
	}
	wide_to_utf8(wbuf, out, (int)cap);
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
 * Same upper bound as the other platforms — admits a vendor plug-in plus
 * the sim_display fallback with plenty of headroom.
 */
#define MAX_PLUGIN_ENTRIES 16

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

/*
 * Discovery contract: see docs/specs/runtime/plugin-discovery.md §3.2.
 *
 * Android v1 uses **convention-driven** discovery rather than the JSON
 * manifest scheme that POSIX (macOS / Linux) and the registry scheme
 * that Windows use. Reason: Android's package installer extracts only
 * `.so` files from an APK's `jniLibs/<abi>/` into the on-disk
 * `lib/<abi>/`. Any `.json` shipped in `jniLibs/` stays trapped inside
 * `base.apk` and would require AAssetManager + a JNIEnv to read — JNI
 * plumbing the loader has no access to at xrCreateInstance time. The
 * iface returned by `xrtPluginNegotiate` already carries id /
 * display_name / vendor; the only load-bearing manifest field is
 * ProbeOrder, which we encode in the filename.
 *
 * Filename convention: `libdxrp<NNN>_<id>.so` where `<NNN>` is the
 * three-digit zero-padded ProbeOrder and `<id>` matches the
 * iface->id the plug-in returns at negotiate. Examples:
 *   libdxrp050_leia_cnsdk.so   (vendor)
 *   libdxrp200_sim_display.so  (fallback)
 *
 * Lexicographic sort on filename gives probe-order ascending for free,
 * same trick the POSIX `050-leia-sr.json` filename convention uses.
 *
 * Discovery root (priority order):
 *   1. $XRT_PLUGIN_SEARCH_PATH — dev override, single dir (no colon
 *      splitting — Android emulator iteration is the only use case)
 *   2. dirname(dladdr(&get_runtime_lib_dir)) — the runtime `.so`'s
 *      own lib dir, which is `/data/app/<runtime-pkg>-<hash>/lib/<abi>/`.
 *      Plug-ins shipped in the runtime APK's `jniLibs/<abi>/` land here.
 *
 * Multi-APK discovery (separate vendor-APKs each shipping plug-ins) is
 * a v2 problem requiring PackageManager queries via JNI — out of scope
 * for v1.
 */

struct plugin_entry
{
	char binary_path[PATH_MAX];  /* absolute path to the .so */
	char filename[NAME_MAX + 1]; /* basename, used for stable sort */
	char id[64];                 /* parsed from filename: chars between `_` and `.so` */
	uint32_t probe_order;        /* parsed from filename: 3 digits after `libdxrp` */
};

static int
compare_by_filename(const void *a, const void *b)
{
	return strcmp(((const struct plugin_entry *)a)->filename,
	              ((const struct plugin_entry *)b)->filename);
}

/*!
 * Parse `libdxrp<NNN>_<id>.so` into probe_order and id. Returns true
 * on a well-formed filename, false otherwise (caller skips it).
 */
static bool
parse_plugin_filename(const char *name, uint32_t *out_order, char *out_id, size_t id_size)
{
	const char *prefix = "libdxrp";
	const size_t prefix_len = 7; /* strlen("libdxrp") */
	size_t n = strlen(name);

	/* Min plausible: "libdxrpNNN_X.so" = 7 + 3 + 1 + 1 + 3 = 15 chars. */
	if (n < 15 || strncmp(name, prefix, prefix_len) != 0) {
		return false;
	}
	if (strcmp(name + n - 3, ".so") != 0) {
		return false;
	}
	if (!(name[prefix_len + 0] >= '0' && name[prefix_len + 0] <= '9' &&
	      name[prefix_len + 1] >= '0' && name[prefix_len + 1] <= '9' &&
	      name[prefix_len + 2] >= '0' && name[prefix_len + 2] <= '9')) {
		return false;
	}
	if (name[prefix_len + 3] != '_') {
		return false;
	}

	*out_order = (uint32_t)((name[prefix_len + 0] - '0') * 100 +
	                        (name[prefix_len + 1] - '0') * 10 +
	                        (name[prefix_len + 2] - '0'));

	size_t id_start = prefix_len + 4;
	size_t id_len = n - 3 - id_start;
	if (id_len == 0 || id_len >= id_size) {
		return false;
	}
	memcpy(out_id, name + id_start, id_len);
	out_id[id_len] = '\0';
	return true;
}

static int
enumerate_dir(const char *root, struct plugin_entry *entries, int start, int max)
{
	DIR *d = opendir(root);
	if (d == NULL) {
		return start;
	}

	int count = start;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (count >= max) {
			U_LOG_W("plugin loader: more than %d entries — truncating.", max);
			break;
		}
		struct plugin_entry e;
		memset(&e, 0, sizeof(e));
		if (!parse_plugin_filename(de->d_name, &e.probe_order, e.id, sizeof(e.id))) {
			continue;
		}
		snprintf(e.binary_path, sizeof(e.binary_path), "%s/%s", root, de->d_name);
		snprintf(e.filename, sizeof(e.filename), "%s", de->d_name);
		entries[count++] = e;
	}
	closedir(d);
	return count;
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
 *     the normal discovery + probe flow.
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
		/* Skip plug-in candidates — they go through try_load_one. */
		if (strncmp(name, "libdxrp", 7) == 0) {
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

static const struct xrt_plugin_iface *
try_load_one(const struct plugin_entry *e, struct xrt_plugin_instance **out_inst)
{
	*out_inst = NULL;
	g_last_reject_reason[0] = '\0';
	g_last_reject_declined = false;
	(void)status_begin(e->id, NULL, NULL, e->probe_order);

	/* RTLD_LOCAL keeps the plug-in's symbols private; aux symbols
	 * resolve via the runtime .so already in the namespace. */
	void *handle = dlopen(e->binary_path, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		const char *dl_err = dlerror();
		struct stat st;
		const bool missing = stat(e->binary_path, &st) != 0;
		bool changed = status_finish(
		    e->id, missing ? TARGET_PLUGIN_RESULT_BINARY_MISSING : TARGET_PLUGIN_RESULT_LOAD_FAILED, 0, dl_err);
		PLUGIN_LOG_OUTCOME(changed, "plugin loader:   %s: dlopen(%s) failed: %s.", e->id, e->binary_path,
		                   dl_err != NULL ? dl_err : "?");
		snprintf(g_last_reject_reason, sizeof(g_last_reject_reason), "%s",
		         missing ? "registered binary does not exist (orphan registration)" : "dlopen failed");
		return NULL;
	}

	dlerror();
	xrt_plugin_negotiate_fn_t negotiate =
	    (xrt_plugin_negotiate_fn_t)dlsym(handle, XRT_PLUGIN_ENTRYPOINT_NAME);
	const char *err = dlerror();
	if (negotiate == NULL || err != NULL) {
		U_LOG_W("plugin loader:   %s: missing entry point '%s' (%s) — skipping.", e->id,
		        XRT_PLUGIN_ENTRYPOINT_NAME, err ? err : "null");
		status_finish(e->id, TARGET_PLUGIN_RESULT_NO_ENTRY_POINT, 0, NULL);
		dlclose(handle);
		return NULL;
	}

	struct xrt_plugin_host_iface host = {0};
	host.struct_size = (uint32_t)sizeof(struct xrt_plugin_host_iface);
	host.host_api_version = XRT_PLUGIN_API_VERSION_CURRENT;
#if defined(XRT_OS_ANDROID)
	// Android plug-ins need the host's JavaVM/activity for vendor-SDK init
	// (their own statically-linked aux_android copy is never populated).
	host.get_android_vm = plugin_host_get_android_vm;
	// Activity when in-process; Service Context when out-of-process (CNSDK only
	// needs an android.content.Context to bind the on-device tracking service).
	host.get_android_activity = plugin_host_get_android_activity;
	// #1037: a Context whose classloader is the runtime APK's, so an
	// in-process vendor plug-in can resolve vendor Java glue the app does
	// not ship. Class loading only — see xrt_plugin.h.
	host.get_android_class_host_context = plugin_host_get_android_class_host_context;
	host.android_package_is_visible = plugin_host_android_package_is_visible;
#endif

	struct xrt_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = negotiate(XRT_PLUGIN_API_VERSION_CURRENT, &host, &iface, &plugin_version);
	if (xret != XRT_SUCCESS || iface == NULL) {
		U_LOG_W("plugin loader:   %s: negotiate returned %d (iface=%p) — skipping.", e->id, (int)xret,
		        (void *)iface);
		status_finish(e->id, TARGET_PLUGIN_RESULT_NEGOTIATE_FAILED, 0, NULL);
		dlclose(handle);
		return NULL;
	}

	/* ADR-020 rule 3: reject a major-version mismatch before touching the
	 * vtable. A plug-in built against a different ABI major lays its vtable
	 * out at offsets the runtime doesn't agree on — calling through it is
	 * exactly the corruption this guards against. Skip it (the caller falls
	 * back to the next plug-in / sim_display); never dispatch. */
	if (plugin_version != XRT_PLUGIN_API_VERSION_CURRENT) {
		U_LOG_E("plugin loader:   %s: ABI major mismatch — plugin_api=%u, runtime expects %u; "
		        "the plug-in must be rebuilt against this runtime's headers — skipping (ADR-020 rule 3).",
		        e->id, plugin_version, (unsigned)XRT_PLUGIN_API_VERSION_CURRENT);
		/* #1212: carry the reason to the self-test, so a rejected vendor
		 * plug-in reads as an ABI mismatch rather than a bare failure. */
		snprintf(g_last_reject_reason, sizeof(g_last_reject_reason),
		         "ABI mismatch: plug-in reports v%u, runtime expects v%u (rebuild it)", plugin_version,
		         (unsigned)XRT_PLUGIN_API_VERSION_CURRENT);
		status_finish(e->id, TARGET_PLUGIN_RESULT_ABI_MISMATCH, 0, g_last_reject_reason);
		dlclose(handle);
		return NULL;
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
			dlclose(handle);
			return NULL;
		}
		if (xret != XRT_SUCCESS) {
			bool changed = status_finish(e->id, TARGET_PLUGIN_RESULT_PROBE_FAILED, (uint32_t)xret, NULL);
			PLUGIN_LOG_OUTCOME(changed, "plugin loader:   %s: probe returned %d — skipping.", e->id,
			                   (int)xret);
			dlclose(handle);
			return NULL;
		}
	}

	U_LOG_W(
	    "plugin loader: active plug-in: id=%s name='%s' vendor='%s' version='%s' "
	    "plugin_api=%u probe_order=%u path=%s",
	    iface->id ? iface->id : e->id, iface->display_name ? iface->display_name : "",
	    iface->vendor ? iface->vendor : "", iface->version ? iface->version : "", plugin_version, e->probe_order,
	    e->binary_path);

	status_set_active(e->id);

	/* dlopen handle intentionally leaked: the iface's function pointers
	 * remain reachable into the .so for the process's lifetime. */
	return iface;
}

static const struct xrt_plugin_iface *
discover_active_plugin(struct xrt_plugin_instance **out_inst, uint32_t max_probe_order)
{
	*out_inst = NULL;

	char root[PATH_MAX] = {0};
	const char *override = getenv("XRT_PLUGIN_SEARCH_PATH");
	if (override != NULL && *override != '\0') {
		snprintf(root, sizeof(root), "%s", override);
	} else if (!get_runtime_lib_dir(root, sizeof(root))) {
		U_LOG_W("plugin loader: dladdr could not locate runtime lib dir — no plug-ins to try.");
		return NULL;
	}

	struct stat st;
	if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
		U_LOG_I("plugin loader: discovery root '%s' absent — no plug-ins to try.", root);
		return NULL;
	}

	struct plugin_entry entries[MAX_PLUGIN_ENTRIES];
	int n = enumerate_dir(root, entries, 0, MAX_PLUGIN_ENTRIES);
	if (n == 0) {
		U_LOG_I("plugin loader: '%s' searched, no libdxrp*.so files found.", root);
		return NULL;
	}

	qsort(entries, (size_t)n, sizeof(entries[0]), compare_by_filename);

	/* Bring sibling vendor `.so` files into the current namespace's
	 * loaded-soinfo table before any plug-in dlopen so DT_NEEDED can
	 * resolve them. See preload_runtime_lib_dir's docstring. */
	preload_runtime_lib_dir(root);

	/* #1037: the class-host Context handed to plug-ins is created for this
	 * package — see plugin_host_get_android_class_host_context(). */
	remember_runtime_package_from_lib_dir(root);

	{
		const char *ids[MAX_PLUGIN_ENTRIES];
		for (int i = 0; i < n; i++) {
			ids[i] = entries[i].id;
		}
		plugin_warn_exclusive_miss(ids, n);
	}

	U_LOG_I("plugin loader: %d registered plug-in(s) in %s; attempting in filename order.", n, root);
	for (int i = 0; i < n; i++) {
		// Refresh path (#342): only attempt strictly-better candidates.
		// First-call path passes UINT32_MAX so no entry is skipped.
		if (entries[i].probe_order >= max_probe_order) {
			continue;
		}
		/* #1545: skipped before dlopen, and not counted as a reject. */
		if (plugin_id_excluded(entries[i].id)) {
			U_LOG_I("plugin loader:   [%d/%d] %s skipped (DXR_PLUGIN_EXCLUSIVE).", i + 1, n, entries[i].id);
			continue;
		}
		U_LOG_I("plugin loader:   [%d/%d] %s (ProbeOrder=%u, %s)", i + 1, n, entries[i].id,
		        entries[i].probe_order, entries[i].binary_path);
		const struct xrt_plugin_iface *iface = try_load_one(&entries[i], out_inst);
		if (iface != NULL) {
			g_active_probe_order = entries[i].probe_order;
			return iface;
		}
		/* #1212: attempted in ascending ProbeOrder and failed, so this is
		 * by construction a better-ranked plug-in than whatever wins
		 * below. The self-test's vendor_dp check keys off this. */
		plugin_note_reject(entries[i].id, entries[i].probe_order);
	}

	// A refresh (max_probe_order bounded by the current winner) legitimately
	// attempts nothing when nothing better is registered — that is not a
	// fallback, so do not shout (monkey-test F2: 48 false alarms per soak).
	if (max_probe_order == 0xFFFFFFFFu) {
		U_LOG_W("plugin loader: no registered plug-in claimed the system — falling back to static drivers.");
	}
	return NULL;
}

/*
 *
 * Public enumeration + PreferredPlugin override (Android — minimal).
 *
 * Android discovery is convention-driven (filename ProbeOrder) and the
 * diagnostic CLI / Control Panel don't ship there in v1, so enumeration
 * and the writable override are stubs; the read honors the env override
 * so a dev/emulator can still pin a plug-in.
 *
 */

int
target_plugin_enumerate(struct target_plugin_desc *out, int max)
{
	(void)out;
	(void)max;
	return 0;
}

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

#else /* !XRT_OS_WINDOWS && !XRT_OS_ANDROID — macOS / Linux */

#include "util/u_json.h"

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

/*
 * Same upper bound as the Windows path — admits a vendor plug-in plus
 * the sim_display fallback with plenty of headroom.
 */
#define MAX_PLUGIN_ENTRIES 16

/*
 * Discovery contract: see docs/specs/runtime/plugin-discovery.md §3.
 *
 * Each plug-in publishes a JSON manifest with the filename convention
 * `<probe_order>-<id>.json` (three-digit zero-padded ProbeOrder prefix
 * gives lexicographic ordering — `050-leia-sr.json` runs before
 * `200-sim-display.json` without parsing the JSON to sort).
 *
 * Roots searched, in priority order:
 *   1. $XRT_PLUGIN_SEARCH_PATH — dev override, single colon-separated list
 *   2. macOS: ~/Library/Application Support/DisplayXR/DisplayProcessors/
 *      Linux: $XDG_DATA_HOME/DisplayXR/DisplayProcessors/
 *             (defaults to ~/.local/share/DisplayXR/DisplayProcessors/)
 *   3. macOS: /Library/Application Support/DisplayXR/DisplayProcessors/
 *      Linux: /usr/local/share/displayxr/DisplayProcessors/
 *             /usr/share/displayxr/DisplayProcessors/
 *
 * Per-user shadows system entries via lexicographic sort tie-break on
 * the absolute path (later inserts win — the loop inserts in priority
 * order, so per-user comes first and a duplicate `<id>` from a system
 * root is dropped at insert time).
 */

struct plugin_entry
{
	char manifest_path[PATH_MAX]; /* absolute path to the JSON */
	char manifest_file[PATH_MAX]; /* basename, used for stable sort */
	char binary_path[PATH_MAX];   /* absolute path to the dylib/.so */
	char id[64];                  /* manifest "plugin.id" */
	char display_name[128];
	char vendor[64];
	char version[64];
	uint32_t probe_order;
};

static int
compare_by_filename(const void *a, const void *b)
{
	const struct plugin_entry *pa = (const struct plugin_entry *)a;
	const struct plugin_entry *pb = (const struct plugin_entry *)b;
	return strcmp(pa->manifest_file, pb->manifest_file);
}

static void
copy_optional_str(const cJSON *root, const char *field, char *dst, size_t dst_size)
{
	if (dst_size == 0) {
		return;
	}
	dst[0] = '\0';
	const cJSON *node = u_json_get(root, field);
	if (node == NULL) {
		return;
	}
	(void)u_json_get_string_into_array(node, dst, dst_size);
}

static bool
parse_manifest(const char *path, struct plugin_entry *e)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		U_LOG_W("plugin loader: %s: fopen failed (errno=%d).", path, errno);
		return false;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long len = ftell(f);
	if (len <= 0 || len > 64 * 1024) {
		U_LOG_W("plugin loader: %s: implausible manifest size %ld.", path, len);
		fclose(f);
		return false;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}
	char *buf = (char *)malloc((size_t)len + 1);
	if (buf == NULL) {
		fclose(f);
		return false;
	}
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	if (got != (size_t)len) {
		free(buf);
		return false;
	}
	buf[len] = '\0';

	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (root == NULL) {
		U_LOG_W("plugin loader: %s: JSON parse failed.", path);
		return false;
	}

	char fmt[16] = {0};
	const cJSON *fmt_node = u_json_get(root, "file_format_version");
	if (fmt_node != NULL) {
		(void)u_json_get_string_into_array(fmt_node, fmt, sizeof(fmt));
	}
	if (strcmp(fmt, "1.0") != 0) {
		U_LOG_W("plugin loader: %s: unsupported file_format_version='%s'.", path, fmt);
		cJSON_Delete(root);
		return false;
	}

	const cJSON *plugin = u_json_get(root, "plugin");
	if (plugin == NULL) {
		U_LOG_W("plugin loader: %s: missing 'plugin' object.", path);
		cJSON_Delete(root);
		return false;
	}

	copy_optional_str(plugin, "id", e->id, sizeof(e->id));
	copy_optional_str(plugin, "display_name", e->display_name, sizeof(e->display_name));
	copy_optional_str(plugin, "vendor", e->vendor, sizeof(e->vendor));
	copy_optional_str(plugin, "version", e->version, sizeof(e->version));
	copy_optional_str(plugin, "binary_path", e->binary_path, sizeof(e->binary_path));

	int probe_order = 100;
	const cJSON *po = u_json_get(plugin, "probe_order");
	if (po != NULL) {
		(void)u_json_get_int(po, &probe_order);
	}
	e->probe_order = (uint32_t)probe_order;

	cJSON_Delete(root);

	if (e->id[0] == '\0' || e->binary_path[0] == '\0') {
		U_LOG_W("plugin loader: %s: required fields 'plugin.id' or 'plugin.binary_path' missing.", path);
		return false;
	}
	return true;
}

static bool
already_have_id(const struct plugin_entry *entries, int n, const char *id)
{
	for (int i = 0; i < n; i++) {
		if (strcmp(entries[i].id, id) == 0) {
			return true;
		}
	}
	return false;
}

static int
enumerate_dir(const char *root, struct plugin_entry *entries, int start, int max)
{
	DIR *d = opendir(root);
	if (d == NULL) {
		return start;
	}

	int count = start;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (count >= max) {
			U_LOG_W("plugin loader: more than %d entries — truncating.", max);
			break;
		}
		const char *name = de->d_name;
		size_t n = strlen(name);
		if (n < 6 || strcmp(name + n - 5, ".json") != 0) {
			continue;
		}
		/* Input-provider manifests share the discovery roots but belong
		 * to the second plug-in type (ADR-034) — the input loader owns
		 * them (target_input_plugin_loader.c). Skipping here keeps the
		 * DP path from load-attempting a DLL that exports
		 * xrtInputPluginNegotiate instead of xrtPluginNegotiate. */
		{
			static const char ip_suffix[] = "-input-provider.json";
			const size_t ip_len = sizeof(ip_suffix) - 1;
			if (n >= ip_len && strcmp(name + n - ip_len, ip_suffix) == 0) {
				continue;
			}
		}

		struct plugin_entry e;
		memset(&e, 0, sizeof(e));
		snprintf(e.manifest_path, sizeof(e.manifest_path), "%s/%s", root, name);
		snprintf(e.manifest_file, sizeof(e.manifest_file), "%s", name);

		if (!parse_manifest(e.manifest_path, &e)) {
			continue;
		}
		/* Per-user shadows system: roots are walked in priority order,
		 * so the first occurrence of an `<id>` wins. */
		if (already_have_id(entries, count, e.id)) {
			U_LOG_I("plugin loader:   %s: shadowed by earlier manifest for id='%s'.", e.manifest_path,
			        e.id);
			continue;
		}
		entries[count++] = e;
	}
	closedir(d);
	return count;
}

static void
append_roots(char roots[][PATH_MAX], int max_roots, int *n_roots, const char *path)
{
	if (path == NULL || *path == '\0' || *n_roots >= max_roots) {
		return;
	}
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return;
	}
	snprintf(roots[(*n_roots)++], PATH_MAX, "%s", path);
}

/*!
 * Assemble the platform's plug-in discovery roots into @p roots (each a
 * PATH_MAX buffer), in priority order, and return the count. Factored out
 * so both discover_active_plugin() and target_plugin_enumerate() search
 * exactly the same set — and shared (non-static) with the input-provider
 * loader, which scans the SAME roots for `*-input-provider.json`
 * manifests (ADR-034 / input-provider-discovery.md §3). See
 * docs/specs/runtime/plugin-discovery.md §3.
 */
int
target_plugin_build_discovery_roots(char roots[][PATH_MAX], int max_roots)
{
	int n_roots = 0;

	const char *override = getenv("XRT_PLUGIN_SEARCH_PATH");
	if (override != NULL && *override != '\0') {
		/* Colon-separated list, like PATH. */
		const char *p = override;
		while (*p && n_roots < max_roots) {
			const char *colon = strchr(p, ':');
			size_t len = colon ? (size_t)(colon - p) : strlen(p);
			if (len > 0 && len < PATH_MAX) {
				char buf[PATH_MAX];
				memcpy(buf, p, len);
				buf[len] = '\0';
				append_roots(roots, max_roots, &n_roots, buf);
			}
			if (!colon) {
				break;
			}
			p = colon + 1;
		}
	}

	const char *home = getenv("HOME");
	char user_root[PATH_MAX];
#ifdef __APPLE__
	if (home != NULL && *home != '\0') {
		snprintf(user_root, sizeof(user_root),
		         "%s/Library/Application Support/DisplayXR/DisplayProcessors", home);
		append_roots(roots, max_roots, &n_roots, user_root);
	}
	/* System-wide root for `.pkg`-style installs that run as root and
	 * write to /Library/Application Support/ rather than ~. Per-user
	 * entries above shadow system-wide entries via the already_have_id
	 * dedup inside enumerate_dir. Issue #274. */
	append_roots(roots, max_roots, &n_roots, "/Library/Application Support/DisplayXR/DisplayProcessors");
#else
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg != NULL && *xdg != '\0') {
		snprintf(user_root, sizeof(user_root), "%s/DisplayXR/DisplayProcessors", xdg);
		append_roots(roots, max_roots, &n_roots, user_root);
	} else if (home != NULL && *home != '\0') {
		snprintf(user_root, sizeof(user_root), "%s/.local/share/DisplayXR/DisplayProcessors", home);
		append_roots(roots, max_roots, &n_roots, user_root);
	}
	/* Built-in default packaged plug-in dir — the Linux `.deb` drops the
	 * sim-display .so + its `200-sim-display.json` manifest here, so an
	 * installed box needs NO XRT_PLUGIN_SEARCH_PATH (#781). The env override
	 * above is appended first, so a dev build still wins when it is set; when
	 * it is unset this is the discovery root that makes an installed runtime
	 * self-sufficient. This is the POSIX analogue of the Windows
	 * DisplayProcessors registry root. */
	append_roots(roots, max_roots, &n_roots, "/usr/lib/displayxr/plugins");
	append_roots(roots, max_roots, &n_roots, "/usr/local/share/displayxr/DisplayProcessors");
	append_roots(roots, max_roots, &n_roots, "/usr/share/displayxr/DisplayProcessors");
#endif

	return n_roots;
}

/*!
 * Per-user manifest dir — the writable root for the `preferred` override
 * file. Returns false if HOME (and XDG) are unset.
 */
static bool
user_manifest_dir(char *out, size_t cap)
{
	const char *home = getenv("HOME");
#ifdef __APPLE__
	if (home == NULL || *home == '\0') {
		return false;
	}
	snprintf(out, cap, "%s/Library/Application Support/DisplayXR/DisplayProcessors", home);
	return true;
#else
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg != NULL && *xdg != '\0') {
		snprintf(out, cap, "%s/DisplayXR/DisplayProcessors", xdg);
		return true;
	}
	if (home != NULL && *home != '\0') {
		snprintf(out, cap, "%s/.local/share/DisplayXR/DisplayProcessors", home);
		return true;
	}
	return false;
#endif
}

/*!
 * Load + negotiate + ABI-check + probe one manifest plug-in. The POSIX twin of
 * the Windows @ref load_and_probe_one: no "active plug-in" line and no
 * status_set_active, so it serves both the single-winner discovery
 * (@ref try_load_one) and the load-all display-claim collection (#69,
 * multi-screen M0). Successful loads leak the dlopen handle so the iface's
 * function pointers stay callable for the process lifetime.
 */
static const struct xrt_plugin_iface *
load_and_probe_one(const struct plugin_entry *e, struct xrt_plugin_instance **out_inst, uint32_t *out_version)
{
	*out_inst = NULL;
	if (out_version != NULL) {
		*out_version = 0;
	}
	g_last_reject_reason[0] = '\0';
	g_last_reject_declined = false;
	(void)status_begin(e->id, e->display_name, e->version, e->probe_order);

	/* RTLD_LOCAL keeps the plug-in's symbols private; aux symbols
	 * resolve via the dependent runtime dylib that ld already linked
	 * into our process. */
	void *handle = dlopen(e->binary_path, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		const char *dl_err = dlerror();
		struct stat st;
		const bool missing = stat(e->binary_path, &st) != 0;
		bool changed = status_finish(
		    e->id, missing ? TARGET_PLUGIN_RESULT_BINARY_MISSING : TARGET_PLUGIN_RESULT_LOAD_FAILED, 0, dl_err);
		PLUGIN_LOG_OUTCOME(changed, "plugin loader:   %s: dlopen(%s) failed: %s.", e->id, e->binary_path,
		                   dl_err != NULL ? dl_err : "?");
		snprintf(g_last_reject_reason, sizeof(g_last_reject_reason), "%s",
		         missing ? "registered binary does not exist (orphan registration)" : "dlopen failed");
		return NULL;
	}

	dlerror();
	xrt_plugin_negotiate_fn_t negotiate =
	    (xrt_plugin_negotiate_fn_t)dlsym(handle, XRT_PLUGIN_ENTRYPOINT_NAME);
	const char *err = dlerror();
	if (negotiate == NULL || err != NULL) {
		U_LOG_W("plugin loader:   %s: missing entry point '%s' (%s) — skipping.", e->id,
		        XRT_PLUGIN_ENTRYPOINT_NAME, err ? err : "null");
		status_finish(e->id, TARGET_PLUGIN_RESULT_NO_ENTRY_POINT, 0, NULL);
		dlclose(handle);
		return NULL;
	}

	struct xrt_plugin_host_iface host = {0};
	host.struct_size = (uint32_t)sizeof(struct xrt_plugin_host_iface);
	host.host_api_version = XRT_PLUGIN_API_VERSION_CURRENT;
#if defined(XRT_OS_ANDROID)
	// Android plug-ins need the host's JavaVM/activity for vendor-SDK init
	// (their own statically-linked aux_android copy is never populated).
	host.get_android_vm = plugin_host_get_android_vm;
	// Activity when in-process; Service Context when out-of-process (CNSDK only
	// needs an android.content.Context to bind the on-device tracking service).
	host.get_android_activity = plugin_host_get_android_activity;
	// #1037: a Context whose classloader is the runtime APK's, so an
	// in-process vendor plug-in can resolve vendor Java glue the app does
	// not ship. Class loading only — see xrt_plugin.h.
	host.get_android_class_host_context = plugin_host_get_android_class_host_context;
	host.android_package_is_visible = plugin_host_android_package_is_visible;
#endif

	struct xrt_plugin_iface *iface = NULL;
	uint32_t plugin_version = 0;
	xrt_result_t xret = negotiate(XRT_PLUGIN_API_VERSION_CURRENT, &host, &iface, &plugin_version);
	if (xret != XRT_SUCCESS || iface == NULL) {
		U_LOG_W("plugin loader:   %s: negotiate returned %d (iface=%p) — skipping.", e->id, (int)xret,
		        (void *)iface);
		status_finish(e->id, TARGET_PLUGIN_RESULT_NEGOTIATE_FAILED, 0, NULL);
		dlclose(handle);
		return NULL;
	}

	/* ADR-020 rule 3: reject a major-version mismatch before touching the
	 * vtable. A plug-in built against a different ABI major lays its vtable
	 * out at offsets the runtime doesn't agree on — calling through it is
	 * exactly the corruption this guards against. Skip it (the caller falls
	 * back to the next plug-in / sim_display); never dispatch. */
	if (plugin_version != XRT_PLUGIN_API_VERSION_CURRENT) {
		U_LOG_E("plugin loader:   %s: ABI major mismatch — plugin_api=%u, runtime expects %u; "
		        "the plug-in must be rebuilt against this runtime's headers — skipping (ADR-020 rule 3).",
		        e->id, plugin_version, (unsigned)XRT_PLUGIN_API_VERSION_CURRENT);
		/* #1212: carry the reason to the self-test, so a rejected vendor
		 * plug-in reads as an ABI mismatch rather than a bare failure. */
		snprintf(g_last_reject_reason, sizeof(g_last_reject_reason),
		         "ABI mismatch: plug-in reports v%u, runtime expects v%u (rebuild it)", plugin_version,
		         (unsigned)XRT_PLUGIN_API_VERSION_CURRENT);
		status_finish(e->id, TARGET_PLUGIN_RESULT_ABI_MISMATCH, 0, g_last_reject_reason);
		dlclose(handle);
		return NULL;
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
			dlclose(handle);
			return NULL;
		}
		if (xret != XRT_SUCCESS) {
			bool changed = status_finish(e->id, TARGET_PLUGIN_RESULT_PROBE_FAILED, (uint32_t)xret, NULL);
			PLUGIN_LOG_OUTCOME(changed, "plugin loader:   %s: probe returned %d — skipping.", e->id,
			                   (int)xret);
			dlclose(handle);
			return NULL;
		}
	}

	if (out_version != NULL) {
		*out_version = plugin_version;
	}
	status_finish(e->id, TARGET_PLUGIN_RESULT_CLAIMED, 0, NULL);

	/* dlopen handle intentionally leaked: the iface's function pointers
	 * remain reachable into the dylib for the process's lifetime. */
	return iface;
}

/*!
 * Try one manifest plug-in as the single active winner: @ref
 * load_and_probe_one plus the canonical "active plug-in:" line (parsed by
 * diagnostics) and the ACTIVE status.
 */
static const struct xrt_plugin_iface *
try_load_one(const struct plugin_entry *e, struct xrt_plugin_instance **out_inst)
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

static const struct xrt_plugin_iface *
discover_active_plugin(struct xrt_plugin_instance **out_inst, uint32_t max_probe_order)
{
	*out_inst = NULL;

	char roots[8][PATH_MAX];
	int n_roots = target_plugin_build_discovery_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])));

	if (n_roots == 0) {
		U_LOG_I("plugin loader: no discovery roots present — no plug-ins to try.");
		return NULL;
	}

	struct plugin_entry entries[MAX_PLUGIN_ENTRIES];
	int n = 0;
	for (int r = 0; r < n_roots; r++) {
		n = enumerate_dir(roots[r], entries, n, MAX_PLUGIN_ENTRIES);
	}
	if (n == 0) {
		U_LOG_I("plugin loader: %d root(s) searched, no JSON manifests found.", n_roots);
		return NULL;
	}

	qsort(entries, (size_t)n, sizeof(entries[0]), compare_by_filename);

	// DXR_PLUGIN_EXCLUSIVE (#1545): warn once if the pin matches nothing.
	{
		const char *ids[MAX_PLUGIN_ENTRIES];
		for (int i = 0; i < n; i++) {
			ids[i] = entries[i].id;
		}
		plugin_warn_exclusive_miss(ids, n);
	}

	// PreferredPlugin override (#378): try the user-pinned plug-in before
	// the filename/ProbeOrder order. A stale or failed preference falls
	// through to the normal order, so it can never brick discovery.
	char preferred[64];
	if (target_plugin_get_preferred(preferred, sizeof(preferred))) {
		for (int i = 0; i < n; i++) {
			if (strcmp(entries[i].id, preferred) != 0) {
				continue;
			}
			// An exclusive pin outranks the user preference (#1545).
			if (plugin_id_excluded(entries[i].id)) {
				break;
			}
			if (entries[i].probe_order >= max_probe_order) {
				break; // refresh path: not strictly-better
			}
			U_LOG_W("plugin loader: PreferredPlugin override — attempting id='%s' first.", preferred);
			const struct xrt_plugin_iface *iface = try_load_one(&entries[i], out_inst);
			if (iface != NULL) {
				g_active_probe_order = entries[i].probe_order;
				return iface;
			}
			U_LOG_W("plugin loader: preferred plug-in '%s' failed — falling back to ProbeOrder.",
			        preferred);
			break;
		}
	}

	U_LOG_I("plugin loader: %d registered plug-in(s); attempting in filename order.", n);
	for (int i = 0; i < n; i++) {
		// Refresh path (#342): only attempt strictly-better candidates.
		// First-call path passes UINT32_MAX so no entry is skipped.
		if (entries[i].probe_order >= max_probe_order) {
			continue;
		}
		/* #1545: skipped before dlopen, and not counted as a reject. */
		if (plugin_id_excluded(entries[i].id)) {
			U_LOG_I("plugin loader:   [%d/%d] %s skipped (DXR_PLUGIN_EXCLUSIVE).", i + 1, n, entries[i].id);
			continue;
		}
		U_LOG_I("plugin loader:   [%d/%d] %s (ProbeOrder=%u, %s)", i + 1, n, entries[i].id,
		        entries[i].probe_order, entries[i].binary_path);
		const struct xrt_plugin_iface *iface = try_load_one(&entries[i], out_inst);
		if (iface != NULL) {
			g_active_probe_order = entries[i].probe_order;
			return iface;
		}
		/* #1212: attempted in ascending ProbeOrder and failed, so this is
		 * by construction a better-ranked plug-in than whatever wins
		 * below. The self-test's vendor_dp check keys off this. */
		plugin_note_reject(entries[i].id, entries[i].probe_order);
	}

	// A refresh (max_probe_order bounded by the current winner) legitimately
	// attempts nothing when nothing better is registered — that is not a
	// fallback, so do not shout (monkey-test F2: 48 false alarms per soak).
	if (max_probe_order == 0xFFFFFFFFu) {
		U_LOG_W("plugin loader: no registered plug-in claimed the system — falling back to static drivers.");
	}
	return NULL;
}

/*!
 * Load EVERY manifest plug-in and return them as display-claim sources for the
 * per-monitor registry — the POSIX twin of the Windows function of the same
 * name (#69 / ADR-015, multi-screen M0). Same manifest roots and order as
 * @ref discover_active_plugin; `DXR_PLUGIN_EXCLUSIVE` keeps every other
 * plug-in out of the process exactly as it does for discovery. The active
 * plug-in is reused, never loaded twice. The others are claim sources only:
 * nothing creates a device from them, and they never become active here.
 * Returns the source count in ascending ProbeOrder.
 *
 * Caller holds @ref g_refresh_mutex and has loaded the active plug-in.
 */
static int
collect_display_sources_platform(struct plugin_display_source *out, int max)
{
	char roots[8][PATH_MAX];
	int n_roots = target_plugin_build_discovery_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])));
	struct plugin_entry entries[MAX_PLUGIN_ENTRIES];
	int n = 0;
	for (int r = 0; r < n_roots; r++) {
		n = enumerate_dir(roots[r], entries, n, MAX_PLUGIN_ENTRIES);
	}
	qsort(entries, (size_t)n, sizeof(entries[0]), compare_by_filename);

	const char *active_id = (g_active_iface != NULL && g_active_iface->id != NULL) ? g_active_iface->id : NULL;
	bool active_seen = false;

	int count = 0;
	for (int i = 0; i < n && count < max; i++) {
		if (plugin_id_excluded(entries[i].id)) {
			continue;
		}
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
			continue; // declined / failed -> contributes no claims
		}
		U_LOG_I("plugin loader: display-claim source id=%s (ProbeOrder=%u)", entries[i].id,
		        entries[i].probe_order);
		out[count].iface = iface;
		out[count].inst = inst;
		out[count].probe_order = entries[i].probe_order;
		count++;
	}

	// An active plug-in that no manifest names any more (removed after it
	// loaded) is still the one devices come from: keep it as a source.
	if (!active_seen && g_active_iface != NULL && count < max) {
		out[count].iface = g_active_iface;
		out[count].inst = g_active_instance;
		out[count].probe_order = g_active_probe_order;
		count++;
	}
	return count;
}

/*
 *
 * Public enumeration + PreferredPlugin override (POSIX: macOS / Linux).
 *
 */

int
target_plugin_enumerate(struct target_plugin_desc *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}

	char roots[8][PATH_MAX];
	int n_roots = target_plugin_build_discovery_roots(roots, (int)(sizeof(roots) / sizeof(roots[0])));
	if (n_roots == 0) {
		return 0;
	}

	struct plugin_entry entries[MAX_PLUGIN_ENTRIES];
	int n = 0;
	for (int r = 0; r < n_roots; r++) {
		n = enumerate_dir(roots[r], entries, n, MAX_PLUGIN_ENTRIES);
	}
	if (n > max) {
		n = max;
	}

	for (int i = 0; i < n; i++) {
		struct target_plugin_desc *d = &out[i];
		memset(d, 0, sizeof(*d));
		snprintf(d->id, sizeof(d->id), "%s", entries[i].id);
		snprintf(d->display_name, sizeof(d->display_name), "%s", entries[i].display_name);
		snprintf(d->vendor, sizeof(d->vendor), "%s", entries[i].vendor);
		snprintf(d->version, sizeof(d->version), "%s", entries[i].version);
		snprintf(d->binary_path, sizeof(d->binary_path), "%s", entries[i].binary_path);
		d->probe_order = entries[i].probe_order;
	}

	return n;
}

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
	if (!user_manifest_dir(dir, sizeof(dir))) {
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
	if (!user_manifest_dir(dir, sizeof(dir))) {
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
	if (!user_manifest_dir(dir, sizeof(dir))) {
		return XRT_SUCCESS; /* nowhere it could live → nothing to clear */
	}
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/preferred", dir);
	if (remove(path) != 0 && errno != ENOENT) {
		return errno == EACCES ? XRT_ERROR_NOT_AUTHORIZED : XRT_ERROR_IPC_FAILURE;
	}
	return XRT_SUCCESS;
}

#endif /* platform-specific loader */


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
	//! Stable screen key (target_screen_keys_build), the per-screen preference's identity.
	char key[TARGET_SCREEN_KEY_MAX];
	//! OS device name the key was qualified with (Windows GDI name, else output / connector).
	char device_name[64];
} g_monitor_side[XRT_DP_REGISTRY_MAX_ENTRIES];
static uint32_t g_monitor_side_count = 0;

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
	for (uint32_t i = 0; i < n; i++) {
		const struct os_display_edid_monitor *m = &list->monitors[i];
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

	if (g_refresh_mutex_initialized) {
		os_mutex_lock(&g_refresh_mutex);
	}
	g_monitor_side_count = n < XRT_DP_REGISTRY_MAX_ENTRIES ? n : XRT_DP_REGISTRY_MAX_ENTRIES;
	struct target_screen_key_input key_in[XRT_DP_REGISTRY_MAX_ENTRIES];
	for (uint32_t i = 0; i < g_monitor_side_count; i++) {
		g_monitor_side[i].monitor_id = out[i].monitor_id;
		g_monitor_side[i].mon = list->monitors[i];
		g_monitor_side[i].desc = out[i];
		monitor_device_name(&list->monitors[i], g_monitor_side[i].device_name,
		                    sizeof(g_monitor_side[i].device_name));
		key_in[i].manufacturer_id = list->monitors[i].manufacturer_id;
		key_in[i].product_id = list->monitors[i].product_id;
		key_in[i].serial = list->monitors[i].serial_number;
		key_in[i].device_name = g_monitor_side[i].device_name;
	}
	// Display dashboard phase 7: the stable key a per-screen preference is
	// stored under (the monitor id is per boot).
	char keys[XRT_DP_REGISTRY_MAX_ENTRIES][TARGET_SCREEN_KEY_MAX];
	target_screen_keys_build(key_in, g_monitor_side_count, keys);
	for (uint32_t i = 0; i < g_monitor_side_count; i++) {
		(void)snprintf(g_monitor_side[i].key, sizeof(g_monitor_side[i].key), "%s", keys[i]);
	}
	if (g_refresh_mutex_initialized) {
		os_mutex_unlock(&g_refresh_mutex);
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

	// Query every source once for the full descriptor set.
	static struct xrt_display_claim src_claims[TARGET_PLUGIN_MAX_SOURCES][XRT_DP_REGISTRY_MAX_ENTRIES];
	uint32_t src_claim_count[TARGET_PLUGIN_MAX_SOURCES] = {0};
	for (int s = 0; s < g_display_source_count; s++) {
		src_claim_count[s] =
		    query_source_claims(&g_display_sources[s], descriptors, dn, src_claims[s], XRT_DP_REGISTRY_MAX_ENTRIES);
	}

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
		uint32_t nc = 0;
		for (int s = 0; s < g_display_source_count && nc < TARGET_PLUGIN_MAX_SOURCES; s++) {
			for (uint32_t c = 0; c < src_claim_count[s]; c++) {
				if (src_claims[s][c].monitor_id != desc->monitor_id) {
					continue;
				}
				const struct xrt_plugin_iface *sif = g_display_sources[s].iface;
				cands[nc].plugin_id = (sif != NULL && sif->id != NULL) ? sif->id : "";
				cands[nc].confidence = (uint32_t)src_claims[s][c].confidence;
				cands[nc].is_active = g_active_iface != NULL && sif == g_active_iface;
				cand_src[nc] = &g_display_sources[s];
				cand_claim[nc] = &src_claims[s][c];
				nc++;
				break; // one claim per source per monitor
			}
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
		for (uint32_t i = 0; i < g_monitor_side_count; i++) {
			if (g_monitor_side[i].monitor_id == desc->monitor_id) {
				screen_key = g_monitor_side[i].key;
				break;
			}
		}
		char screen_pref[64] = {0};
		enum u_setting_source pref_source = U_SETTING_SOURCE_DEFAULT;
		const bool have_screen_pref =
		    screen_key[0] != '\0' && u_setting_get_preferred_plugin_for_screen(
		                                 screen_key, screen_pref, sizeof(screen_pref), &pref_source) != NULL;

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
