// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Executable basenames as per-app keys (ADR-034 Amendment 5, #2181).
 *
 * The per-app input-provider pin (`InputProviders\PerApp\<exe-basename>`) and
 * the per-title camera profile (#1381, `app-profiles/<exe-basename>.json`)
 * share one keyspace: the actual executable's file name with its extension
 * (`SampleGame.exe`), never `XrApplicationInfo::applicationName` and never a
 * directory. In-process code keys on its own executable
 * (@ref u_exe_basename_self); the IPC service keys on the PEER process,
 * resolved from the OS-derived pid (`ipc_server_peer_exe_path`).
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Is @p name usable as a per-app key: non-empty, shorter than @p max_size
 * bytes, no path separator ('/' or '\\'), no control character, and not "."
 * or "..". Pure. A NULL @p name is not valid.
 */
bool
u_exe_basename_valid(const char *name, size_t max_size);

/*!
 * The basename of @p path (everything after the last '/' or '\\') into
 * @p out, when it is a valid key (@ref u_exe_basename_valid with @p cap).
 * Pure. Returns false — and writes "" — for a NULL / empty path, a path
 * ending in a separator, or a basename that does not fit: a truncated key
 * would name another app's record, so it is refused rather than cut.
 */
bool
u_exe_basename_from_path(const char *path, char *out, size_t cap);

/*!
 * This process's own executable basename (UTF-8). Windows:
 * `GetModuleFileNameW(NULL)`; macOS: `_NSGetExecutablePath`; Linux /
 * Android: `/proc/self/exe`. False (out = "") when it cannot be read.
 */
bool
u_exe_basename_self(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
