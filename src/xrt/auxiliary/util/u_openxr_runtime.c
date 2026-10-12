// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The OpenXR loader's POSIX active-runtime chain (read-only).
 * @ingroup aux_util
 */

#include "util/u_openxr_runtime.h"

#include "xrt/xrt_config_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef XRT_OS_WINDOWS
#include <sys/stat.h>
#endif


static bool
is_set(const char *s)
{
	return s != NULL && s[0] != '\0';
}

//! Copy @p path (length @p n) into @p out when it fits.
static bool
take(const char *path, int n, char *out, size_t out_size)
{
	if (n <= 0 || (size_t)n >= out_size) {
		return false;
	}
	memcpy(out, path, (size_t)n + 1);
	return true;
}

//! Try `<dir>/openxr/1/active_runtime.<arch>.json`, then `.../active_runtime.json`.
static bool
try_dir(const char *dir,
        size_t dir_len,
        const char *arch,
        u_openxr_path_exists_fn exists,
        void *user,
        char *out,
        size_t out_size)
{
	if (dir_len == 0) {
		return false;
	}
	char path[4096];
	if (is_set(arch)) {
		const int n =
		    snprintf(path, sizeof(path), "%.*s/openxr/1/active_runtime.%s.json", (int)dir_len, dir, arch);
		if (n > 0 && (size_t)n < sizeof(path) && exists(path, user)) {
			return take(path, n, out, out_size);
		}
	}
	const int n = snprintf(path, sizeof(path), "%.*s/openxr/1/active_runtime.json", (int)dir_len, dir);
	if (n > 0 && (size_t)n < sizeof(path) && exists(path, user)) {
		return take(path, n, out, out_size);
	}
	return false;
}

bool
u_openxr_active_runtime_resolve(const struct u_openxr_runtime_env *env,
                                u_openxr_path_exists_fn exists,
                                void *user,
                                char *out,
                                size_t out_size)
{
	if (env == NULL || exists == NULL || out == NULL || out_size == 0) {
		return false;
	}
	out[0] = '\0';

	// 1. The override: the loader uses it as-is.
	if (is_set(env->xr_runtime_json)) {
		const int n = snprintf(out, out_size, "%s", env->xr_runtime_json);
		if (n > 0 && (size_t)n < out_size) {
			return true;
		}
		out[0] = '\0';
		return false;
	}

	// 2. Per-user config.
	char home_cfg[4096];
	const char *cfg = env->xdg_config_home;
	if (!is_set(cfg) && is_set(env->home)) {
		(void)snprintf(home_cfg, sizeof(home_cfg), "%s/.config", env->home);
		cfg = home_cfg;
	}
	if (is_set(cfg) && try_dir(cfg, strlen(cfg), env->arch, exists, user, out, out_size)) {
		return true;
	}

	// 3. System config dirs, colon-separated.
	const char *dirs = is_set(env->xdg_config_dirs) ? env->xdg_config_dirs : "/etc/xdg";
	for (const char *p = dirs;;) {
		const char *colon = strchr(p, ':');
		const size_t len = colon != NULL ? (size_t)(colon - p) : strlen(p);
		if (try_dir(p, len, env->arch, exists, user, out, out_size)) {
			return true;
		}
		if (colon == NULL) {
			break;
		}
		p = colon + 1;
	}

	// 4./5. SYSCONFDIR, then the data dirs.
	static const char *const fixed[] = {"/etc", "/usr/local/share", "/usr/share"};
	for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
		if (try_dir(fixed[i], strlen(fixed[i]), env->arch, exists, user, out, out_size)) {
			return true;
		}
	}
	out[0] = '\0';
	return false;
}

const char *
u_openxr_arch_abi(void)
{
#if defined(__x86_64__) || defined(_M_X64)
	return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
	return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
	return "i686";
#elif defined(__arm__)
	return "armv7a-vfp";
#else
	return NULL;
#endif
}

#ifndef XRT_OS_WINDOWS
static bool
file_exists(const char *path, void *user)
{
	(void)user;
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
#endif

bool
u_openxr_active_runtime_find(char *out, size_t out_size)
{
#ifdef XRT_OS_WINDOWS
	(void)out;
	(void)out_size;
	return false;
#else
	const struct u_openxr_runtime_env env = {
	    .xr_runtime_json = getenv("XR_RUNTIME_JSON"),
	    .xdg_config_home = getenv("XDG_CONFIG_HOME"),
	    .home = getenv("HOME"),
	    .xdg_config_dirs = getenv("XDG_CONFIG_DIRS"),
	    .arch = u_openxr_arch_abi(),
	};
	char found[4096];
	if (!u_openxr_active_runtime_resolve(&env, file_exists, NULL, found, sizeof(found))) {
		return false;
	}
	// Report the manifest the link really names. A missing XR_RUNTIME_JSON
	// stays verbatim: that is what the loader will try.
	char *real = realpath(found, NULL);
	const int n = snprintf(out, out_size, "%s", real != NULL ? real : found);
	free(real);
	return n > 0 && (size_t)n < out_size;
#endif
}
