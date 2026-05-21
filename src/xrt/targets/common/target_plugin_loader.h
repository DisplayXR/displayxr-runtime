// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-side loader for vendor plug-in DLLs.
 *
 * Implements the consumer side of the ABI defined in
 * `xrt/xrt_plugin.h`. At its first call, the loader tries to
 * `LoadLibraryExW` the sim_display plug-in DLL from
 * `<runtime DLL dir>/plugins/DisplayXR-SimDisplay.dll`, resolves
 * `xrtPluginNegotiate`, runs the version handshake, and caches the
 * returned @ref xrt_plugin_iface. Subsequent calls return the cached
 * iface (or NULL if the load failed).
 *
 * The plug-in DLL handle is intentionally leaked for the process
 * lifetime — the DP factories returned by the iface are stored in
 * `xrt_system_compositor_info` and must remain callable until session
 * teardown. Proper shutdown becomes meaningful when registry-driven
 * discovery + per-instance lifecycle land in the next step.
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
 * Returns the @ref xrt_plugin_iface for the sim_display plug-in DLL, or
 * NULL if the plug-in is unavailable. NULL is the signal for callers to
 * fall back to the statically-linked `sim_display_*` symbols.
 *
 * Reasons the loader returns NULL:
 *   - Non-Windows platform (v1 limitation; plug-in path is Windows-only).
 *   - Plug-in DLL not present at `<runtime>/plugins/DisplayXR-SimDisplay.dll`
 *     (typical for developer builds that don't `install`).
 *   - `xrtPluginNegotiate` missing or returns a non-`XRT_SUCCESS`
 *     result (e.g. ABI version mismatch).
 *
 * Thread-safety: not safe under concurrent first-call. v1 assumes
 * single-threaded instance creation.
 */
const struct xrt_plugin_iface *
target_plugin_get_sim_display(void);

#ifdef __cplusplus
}
#endif
