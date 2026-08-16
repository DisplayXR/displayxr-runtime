// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Dev-path guard for plug-in DLL discovery (#952).
 *
 * Both the display-processor loader and the input-provider loader hand a
 * registry-supplied `Binary` path straight to `LoadLibraryExW`. Registering a
 * DLL that lives in a live *build tree* (a git worktree, a `build/` output dir)
 * under a running service is the exact footgun that staged #943: a concurrent
 * rebuild tore the mapped image and something in it called `exit()`, taking the
 * whole service down. This guard classifies the path so the loaders can refuse
 * the footgun (unless a dev override is set) while still allowing the ordinary
 * dev-deploy location (`_package/`) with a one-shot warning.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_config_os.h"

#ifdef __cplusplus
extern "C" {
#endif

enum target_plugin_path_verdict
{
	//! Under the runtime install dir or Program Files — load silently.
	TARGET_PLUGIN_PATH_TRUSTED = 0,
	//! Not an install path but not a live build tree either (e.g. `_package/`).
	//! Load, but WARN once — a dev iterating locally.
	TARGET_PLUGIN_PATH_DEV,
	//! A git worktree / `build/` output dir. This is the #943 footgun; the
	//! loader should SKIP it unless DXR_ALLOW_DEV_PLUGIN_PATHS is set.
	TARGET_PLUGIN_PATH_REFUSED,
};

/*!
 * Classify a plug-in DLL path. Windows-only in substance; on POSIX (manifest
 * discovery already roots paths) it always returns TRUSTED.
 *
 * @param binary_path_w  wide DLL path from the registry (Windows) — may be NULL
 *                       on POSIX callers, which get TRUSTED.
 * @param id             plug-in id, for log lines.
 * @param kind           "plugin" / "input plugin", for log lines.
 *
 * The verdict already accounts for the DXR_ALLOW_DEV_PLUGIN_PATHS override:
 * when it is set, a would-be REFUSED path is downgraded to DEV. The function
 * emits the one-shot WARN/refusal log line itself, so callers only branch on
 * the verdict.
 */
enum target_plugin_path_verdict
target_plugin_path_check(const void *binary_path_w, const char *id, const char *kind);

#ifdef __cplusplus
}
#endif
