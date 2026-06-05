// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Sanitized pre-load of plug-in dependency DLLs whose unwind data
 *         crashes host-engine module tracers (issue #434).
 *
 * Some game engines register loader DLL-notification callbacks that parse
 * every loaded module's x64 unwind tables. Unreal Engine 5.x's
 * `FBacktracer::AddModule` (WindowsCallstackTrace.cpp, active in every
 * non-Shipping Win64 build) underflows its output cursor when a module's
 * FIRST version-1 unwind entry has UNW_FLAG_CHAININFO — an access violation
 * inside the host's callback, mid-`LoadLibrary`, the moment the runtime
 * loads a vendor plug-in whose dependency has that shape (e.g. Leia SR's
 * DimencoWeaving.dll). The host crashes during `xrCreateInstance` through
 * no fault of its own code or ours.
 *
 * The workaround composes two facts:
 *  1. UE's backtracer intentionally skips any non-exe module whose full
 *     path contains "ThirdParty" (and not "Binaries") — its own
 *     compatibility valve for third-party binaries (case-sensitive).
 *  2. The Windows loader binds imports by basename to already-resident
 *     modules — the same property @ref preload_runtime_core_dll exploits
 *     for issue #328.
 *
 * So: walk the plug-in's import graph BEFORE `LoadLibrary`ing it,
 * shape-check each non-system dependency, stage a byte-copy of any
 * vulnerable one under `%LOCALAPPDATA%\DisplayXR\ThirdParty\...` and
 * pre-load it from there. The whole subsequent import graph binds to the
 * resident, filter-skipped module and the vulnerable parse never runs.
 *
 * Fully vendor-agnostic: detection is by unwind shape, not by name, so it
 * covers any present or future vendor dependency and retires itself
 * automatically once a fixed DLL ships (ADR-019: no vendor identifiers).
 *
 * @ingroup target_common
 */

#pragma once

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_WINDOWS

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Scan @p plugin_binary_path's import graph (static + delay imports,
 * recursively) and sanitize-preload any dependency whose unwind data would
 * crash a host-engine module tracer. Best-effort: any failure logs one line
 * and falls through to the normal load — never worse than the status quo.
 *
 * Call once per plug-in, immediately before `LoadLibraryExW` of the binary.
 */
void
target_plugin_sanitized_preload(const wchar_t *plugin_binary_path);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_WINDOWS
