// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-side loader for vendor plug-in DLLs.
 *
 * Implements the consumer side of the ABI defined in
 * `xrt/xrt_plugin.h`. On first call, the loader enumerates
 * `HKLM\Software\DisplayXR\DisplayProcessors\*` (Windows) — the
 * registry schema documented in
 * `docs/roadmap/vendor-plugin-architecture.md` §4.1 — sorts the
 * registered plug-ins by their `ProbeOrder` value (ascending; missing
 * = 100), and tries each in turn: `LoadLibraryExW` → `GetProcAddress`
 * → `xrtPluginNegotiate` → `iface->probe()`. The first plug-in whose
 * probe returns `XRT_SUCCESS` wins and is cached for the process's
 * lifetime; subsequent registry entries are not attempted.
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
 *   - Non-Windows platform (v1 limitation; the manifest-driven Linux
 *     and macOS discovery lands with the cross-platform port).
 *   - `HKLM\Software\DisplayXR\DisplayProcessors` absent or empty
 *     (typical for developer builds that haven't run the installer or
 *     written the registry entry by hand).
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

#ifdef __cplusplus
}
#endif
