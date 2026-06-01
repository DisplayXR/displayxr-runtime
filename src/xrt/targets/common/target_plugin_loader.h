// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-side loader for vendor plug-in DLLs.
 *
 * Implements the consumer side of the ABI defined in
 * `xrt/xrt_plugin.h`. On first call, the loader enumerates the
 * platform's discovery root — `HKLM\Software\DisplayXR\DisplayProcessors\*`
 * on Windows, JSON manifests under
 * `~/Library/Application Support/DisplayXR/DisplayProcessors/` on macOS,
 * `${XDG_DATA_HOME:-~/.local/share}/DisplayXR/DisplayProcessors/` plus
 * system roots on Linux, or sibling `libdxrp<NNN>_<id>.so` files in the
 * runtime APK's own lib dir on Android — sorts the registered plug-ins
 * by their `ProbeOrder` value (ascending; missing = 100), and tries
 * each in turn: load the DLL/dylib/`.so` → resolve `xrtPluginNegotiate`
 * → `iface->probe()`. The first plug-in whose probe returns
 * `XRT_SUCCESS` wins and is cached for the process's lifetime;
 * subsequent entries are not attempted. The discovery contract lives
 * at `docs/specs/runtime/plugin-discovery.md`.
 *
 * The loader is intentionally per-process and one-shot. Multi-vendor
 * heterogeneous setups (multiple active plug-ins concurrently) are a
 * v2 problem per the plan's non-goals (§3).
 *
 * The plug-in DLL handle is intentionally leaked for the process
 * lifetime — the iface holds the DP factories that get stored in
 * `xrt_system_compositor_info` and must remain callable until session
 * teardown.
 *
 * Issue #256 — vendor plug-in re-architecture.
 *
 * @ingroup target_common
 */

#pragma once

#include "xrt/xrt_plugin.h"

#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Vendor-neutral descriptor for one registered plug-in, as discovered
 * from the platform's discovery root WITHOUT loading the DLL/dylib/`.so`.
 *
 * Used by diagnostic front-ends — `displayxr-cli dp list` and the Control
 * Panel — to enumerate the choices the loader would consider, plus their
 * `ProbeOrder`, without paying the cost (or the ADR-019 exposure) of
 * actually loading a vendor plug-in. All fields are UTF-8.
 */
struct target_plugin_desc
{
	char id[64];           /* discovery id (Windows subkey / manifest <id>) */
	char display_name[128];
	char vendor[64];
	char version[64];
	char binary_path[512];
	uint32_t probe_order; /* lower wins; missing defaults to 100 */
};

/*!
 * Enumerate registered plug-ins from the platform's discovery root
 * WITHOUT loading them. Writes up to @p max descriptors to @p out and
 * returns the count (0 if the discovery root is absent / empty). The
 * order is unspecified — callers that care sort by `probe_order`.
 *
 * This re-reads the discovery root every call (it is not cached like the
 * active-plug-in path); cheap enough for an interactive `dp list`.
 */
int
target_plugin_enumerate(struct target_plugin_desc *out, int max);

/*!
 * Read the `PreferredPlugin` override — the plug-in id the loader tries
 * before the `ProbeOrder` sort. Writes the id (NUL-terminated) to @p out
 * and returns true iff a non-empty override is configured; otherwise
 * writes an empty string and returns false.
 *
 * Windows: `REG_SZ` `PreferredPlugin` at the discovery root key
 * `HKLM\Software\DisplayXR\DisplayProcessors`. POSIX: the
 * `XRT_PREFERRED_PLUGIN_ID` env var, else a `preferred` file in the
 * per-user manifest dir.
 */
bool
target_plugin_get_preferred(char *out, size_t cap);

/*!
 * Set the `PreferredPlugin` override to @p id (a plug-in discovery id).
 * Takes effect for processes that begin discovery AFTER the write — the
 * loader is one-shot per process, so a running service must be restarted.
 *
 * Windows writes HKLM and therefore requires administrator rights;
 * returns @ref XRT_ERROR_NOT_AUTHORIZED on access-denied,
 * @ref XRT_ERROR_IPC_FAILURE on other write failures, and
 * @ref XRT_SUCCESS on success.
 */
xrt_result_t
target_plugin_set_preferred(const char *id);

/*!
 * Clear the `PreferredPlugin` override (restore normal `ProbeOrder`
 * discovery). Idempotent — clearing an unset override is @ref XRT_SUCCESS.
 * Same elevation / restart caveats as @ref target_plugin_set_preferred.
 */
xrt_result_t
target_plugin_clear_preferred(void);

/*!
 * Returns the @ref xrt_plugin_iface for the active plug-in (whichever
 * registered plug-in's probe succeeded first per `ProbeOrder`), or
 * NULL if none claimed the system. NULL is the signal for callers to
 * fall back to the statically-linked driver symbols.
 *
 * Reasons the loader returns NULL:
 *   - Discovery root is absent or empty (no Windows registry key, no
 *     JSON manifests in any POSIX root — typical for developer builds
 *     that haven't run an installer or set `XRT_PLUGIN_SEARCH_PATH`).
 *   - Every registered plug-in failed to load, declined at
 *     `xrtPluginNegotiate`, or returned non-`XRT_SUCCESS` from
 *     `iface->probe()`.
 *
 * Thread-safety: not safe under concurrent first-call. The runtime
 * resolves this from `xrCreateInstance`, which is single-threaded
 * per the OpenXR spec.
 */
const struct xrt_plugin_iface *
target_plugin_get_active(void);

/*!
 * Returns the @ref xrt_plugin_instance handle returned by the active
 * plug-in's `iface->probe()`, or NULL if no plug-in is active or the
 * plug-in's probe yielded a NULL instance (the v1 sim_display and
 * leia plug-ins both do — they store driver state in file-scope
 * statics inside the plug-in DLL). Pair with
 * `target_plugin_get_active()` when calling iface vtable methods
 * that take the instance handle.
 */
struct xrt_plugin_instance *
target_plugin_get_active_instance(void);

/*!
 * Re-enumerate the platform's discovery root and, if a STRICTLY-BETTER
 * (lower ProbeOrder) plug-in than the currently active one is now
 * registered (or no plug-in was active before), swap it in and return
 * its iface. Otherwise returns the unchanged current iface.
 *
 * Concretely: addresses issue #342 — the service starts mid-install
 * with only `sim-display` registered (ProbeOrder 200) and bakes its
 * factory pointers into `xrt_system_compositor_info`; the vendor
 * plug-in (e.g. `leia-sr` at ProbeOrder 50) registers a moment later.
 * Service-mode compositors invoke
 * `xrt_system_compositor_info::refresh_display_processors` at
 * per-client compositor create, which calls this and then re-derives
 * the factory pointers, so the first app launch picks up the better
 * plug-in without a service restart.
 *
 * Thread-safety: mutex-guarded internally. Idempotent — repeated calls
 * with no new registration are cheap (re-enumerate + skip every entry
 * whose ProbeOrder is not strictly better than the active one). The
 * previously-active plug-in's DLL is intentionally LEAKED on swap,
 * consistent with the load path: the runtime never destroys a vtable
 * the compositor might still hold pointers into.
 */
const struct xrt_plugin_iface *
target_plugin_refresh_active(void);

/* Forward declarations — full defs in xrt/xrt_compositor.h and
 * os/os_display_edid.h; the .c includes those, the header stays lean. */
struct xrt_dp_factory_registry;
struct os_display_edid_list;

/*!
 * Map an enumerated EDID monitor list (`os_display_edid_enumerate`) into the
 * vendor-neutral @ref xrt_display_descriptor array handed to
 * `probe_displays()`. Assigns each monitor a stable-for-this-boot
 * `monitor_id` (hashed from EDID manufacturer/product + screen position),
 * converts `refresh_hz`→`refresh_mhz`, and maps `is_primary`→`flags` bit 0.
 * Writes up to @p max descriptors and returns the count.
 *
 * Issue #69 / ADR-015.
 */
uint32_t
target_plugin_build_descriptors(const struct os_display_edid_list *list,
                                struct xrt_display_descriptor *out,
                                uint32_t max);

/*!
 * Build the per-monitor DP factory registry (issue #69 / ADR-015). Loads
 * every registered plug-in (reusing the already-active one), asks each for
 * its `probe_displays()` claims — or, for a plug-in without `probe_displays`
 * whose binary `probe()` succeeded, synthesizes a single
 * @ref XRT_DISPLAY_CLAIM_EDID claim on the primary monitor (single-display
 * back-compat) — then resolves per monitor (highest confidence wins; ties by
 * ascending ProbeOrder) into @p out_registry. A monitor no plug-in claims
 * gets no entry.
 *
 * Vendor-blind: the registry stores only `void *` factory pointers, the
 * winning `iface->id` string, and the claim serial — no vendor symbols enter
 * the runtime link line (ADR-019). The loaded plug-in source set is cached
 * for the process lifetime (rebuilt when @ref target_plugin_refresh_active
 * swaps in a better plug-in). Mutex-guarded like the refresh path.
 *
 * Off-Windows the EDID enumerator yields no monitors, so a 0-length
 * descriptor list resolves to an empty registry (`entry_count == 0`),
 * signalling callers to use the scalar `dp_factory_*` fields.
 */
void
target_plugin_resolve_displays(const struct xrt_display_descriptor *descriptors,
                               uint32_t descriptor_count,
                               struct xrt_dp_factory_registry *out_registry);

#ifdef __cplusplus
}
#endif
