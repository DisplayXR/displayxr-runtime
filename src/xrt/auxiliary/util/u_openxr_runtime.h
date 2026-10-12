// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Which OpenXR runtime manifest the Khronos loader resolves on POSIX
 *         (Linux, macOS): read-only, for diagnostics (ADR-051 snapshot).
 *
 * The loader's chain, first hit wins:
 *  1. `XR_RUNTIME_JSON` (used as-is, whether or not the file exists);
 *  2. `$XDG_CONFIG_HOME` (default `$HOME/.config`) + `/openxr/1/`;
 *  3. each dir of `$XDG_CONFIG_DIRS` (default `/etc/xdg`) + `/openxr/1/`;
 *  4. `/etc/openxr/1/` (the loader's SYSCONFDIR);
 *  5. `/usr/local/share/openxr/1/`, `/usr/share/openxr/1/`.
 * In each directory the arch-suffixed `active_runtime.<arch>.json` is
 * preferred over the plain `active_runtime.json`.
 *
 * The resolver is pure (environment values and a "file exists" callback are
 * injected), so it is unit-testable; @ref u_openxr_active_runtime_find is the
 * live wrapper.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The environment the loader's POSIX chain reads; NULL or "" = unset.
struct u_openxr_runtime_env
{
	const char *xr_runtime_json;
	const char *xdg_config_home;
	const char *home;
	const char *xdg_config_dirs;
	//! The loader's arch ABI name ("x86_64", "aarch64", ...); NULL/"" = plain name only.
	const char *arch;
};

//! Does @p path exist (as a file the loader would open)?
typedef bool (*u_openxr_path_exists_fn)(const char *path, void *user);

/*!
 * Resolve the active runtime manifest path the loader would use.
 *
 * @return true with @p out filled (no symlink following here); false when no
 *         candidate exists (or @p out is too small).
 */
bool
u_openxr_active_runtime_resolve(const struct u_openxr_runtime_env *env,
                                u_openxr_path_exists_fn exists,
                                void *user,
                                char *out,
                                size_t out_size);

//! The loader's arch ABI name for this build ("x86_64", ...), NULL when unknown.
const char *
u_openxr_arch_abi(void);

/*!
 * Live: the process environment + the filesystem, then `realpath` so a
 * symlinked `active_runtime.json` reports the manifest it really names.
 * Always false on Windows (the loader reads the registry there).
 */
bool
u_openxr_active_runtime_find(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
