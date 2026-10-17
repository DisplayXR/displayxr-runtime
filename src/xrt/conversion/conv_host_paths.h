// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The directories a conversion host hands a provider through
 *         `xrt_conversion_plugin_host_iface::get_path` (#2194).
 *
 * Shared by both hosts so a provider gets the same answer in-service and in a
 * satellite (the satellite is a separate process started with the service's
 * environment, so it resolves the same root):
 *
 *  - XRT_CONV_PATH_SCRATCH: one writable directory per provider id, created
 *    on first request (owner-only, 0700 on POSIX). A cache: the runtime or the
 *    user may delete it at any time and the provider rebuilds what it kept.
 *      macOS:   ~/Library/Caches/DisplayXR/Conversion/<id>
 *      Linux:   $XDG_CACHE_HOME/displayxr/conversion/<id> (default ~/.cache)
 *      Windows: %LOCALAPPDATA%\DisplayXR\Conversion\<id>
 *    `DXR_CONV_SCRATCH_ROOT` (environment of the hosting process) replaces
 *    the root, so the directory is `<root>/<id>`: tests and dev boxes only.
 *  - XRT_CONV_PATH_MODEL_STORE: the process model store's root (read-only to
 *    providers; conv_model_store.h).
 *  - XRT_CONV_PATH_LOGS: the directory of the host's log file (informational).
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_config_os.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Which platform rule conv_scratch_root_from() applies.
enum conv_scratch_os
{
	CONV_SCRATCH_OS_WINDOWS = 0,
	CONV_SCRATCH_OS_MACOS = 1,
	CONV_SCRATCH_OS_LINUX = 2,
};

/*! Every input of the scratch root, injectable for tests (NULL or "" = unset). */
struct conv_scratch_root_inputs
{
	enum conv_scratch_os os;
	const char *env_override;   //!< DXR_CONV_SCRATCH_ROOT
	const char *home;           //!< $HOME (POSIX)
	const char *xdg_cache_home; //!< $XDG_CACHE_HOME (Linux)
	const char *local_app_data; //!< %LOCALAPPDATA% (Windows)
};

/*!
 * The scratch root (the directory that holds one directory per provider) from
 * @p in. False when nothing usable is set (no HOME / LOCALAPPDATA).
 */
bool
conv_scratch_root_from(const struct conv_scratch_root_inputs *in, char *out, size_t out_size);

//! conv_scratch_root_from() with this process's platform and environment.
bool
conv_scratch_root_resolve(char *out, size_t out_size);

/*!
 * Is @p id usable as one path component? Non-empty, at most 63 bytes, only
 * [A-Za-z0-9._-], and not "." or "..": a provider id never names a path.
 */
bool
conv_scratch_id_valid(const char *id);

/*!
 * The scratch directory of provider @p id, created (with any missing parent)
 * when absent: owner-only (0700) on POSIX, the profile's inherited ACL on
 * Windows. False when the id is invalid, no root resolves, the path does not
 * fit, or the directory cannot be created.
 */
bool
conv_scratch_dir(const char *id, char *out, size_t out_size);

/*!
 * `get_path` for provider @p id, as both hosts answer it. @p log_file = the
 * host's current log file ("" / NULL: XRT_CONV_PATH_LOGS is unanswered).
 * Writes "" and returns false for an unknown kind or any failure, never a
 * truncated path.
 */
bool
conv_host_get_path(const char *id, uint32_t kind, const char *log_file, char *buf, uint32_t buf_size);

#ifdef __cplusplus
}
#endif
