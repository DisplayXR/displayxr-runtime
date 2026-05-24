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


#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
