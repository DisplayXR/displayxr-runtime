// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Dev-path guard for plug-in DLL discovery (#952).
 * @ingroup aux_util
 */

#include "target_plugin_path_guard.h"

#include "util/u_logging.h"

#ifdef XRT_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
#include <stdbool.h>

// Lowercase a wide path in place (ASCII fold is enough for path matching).
static void
lower_inplace(wchar_t *s)
{
	for (; *s; ++s) {
		if (*s >= L'A' && *s <= L'Z') {
			*s = (wchar_t)(*s - L'A' + L'a');
		}
		if (*s == L'/') {
			*s = L'\\';
		}
	}
}

static bool
starts_with(const wchar_t *hay, const wchar_t *pre)
{
	size_t n = wcslen(pre);
	return n > 0 && wcsncmp(hay, pre, n) == 0;
}

// Directory of the module this loader code lives in — the service exe or
// DisplayXRClient.dll, both in the runtime install dir. Cached.
static const wchar_t *
runtime_dir_lower(void)
{
	static wchar_t dir[MAX_PATH] = {0};
	static bool done = false;
	if (done) {
		return dir[0] ? dir : NULL;
	}
	done = true;

	HMODULE self = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)(void *)&runtime_dir_lower, &self)) {
		return NULL;
	}
	DWORD len = GetModuleFileNameW(self, dir, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		dir[0] = 0;
		return NULL;
	}
	wchar_t *slash = wcsrchr(dir, L'\\');
	if (slash == NULL) {
		dir[0] = 0;
		return NULL;
	}
	*slash = L'\0';
	lower_inplace(dir);
	return dir[0] ? dir : NULL;
}

static bool
under_env_dir(const wchar_t *path_lower, const wchar_t *env_name)
{
	wchar_t buf[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(env_name, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return false;
	}
	lower_inplace(buf);
	return starts_with(path_lower, buf);
}

static bool
override_set(void)
{
	wchar_t buf[8];
	DWORD n = GetEnvironmentVariableW(L"DXR_ALLOW_DEV_PLUGIN_PATHS", buf, 8);
	// Any value other than empty / "0" counts as set.
	return n > 0 && !(n == 1 && buf[0] == L'0');
}

enum target_plugin_path_verdict
target_plugin_path_check(const void *binary_path_w, const char *id, const char *kind)
{
	if (binary_path_w == NULL) {
		return TARGET_PLUGIN_PATH_TRUSTED;
	}
	const char *k = kind ? kind : "plugin";
	const char *pid = id ? id : "?";

	wchar_t lower[MAX_PATH];
	wcsncpy_s(lower, MAX_PATH, (const wchar_t *)binary_path_w, _TRUNCATE);
	lower_inplace(lower);

	// Trusted: under the runtime install dir or a Program Files root (vendor
	// plug-ins install there, e.g. C:\Program Files\DisplayXR\Plugins\LeiaSR).
	const wchar_t *rt = runtime_dir_lower();
	if ((rt != NULL && starts_with(lower, rt)) || under_env_dir(lower, L"ProgramFiles") ||
	    under_env_dir(lower, L"ProgramFiles(x86)") || under_env_dir(lower, L"ProgramW6432")) {
		return TARGET_PLUGIN_PATH_TRUSTED;
	}

	// Footgun: a live build tree. These are the paths #943 warns against —
	// a concurrent rebuild tears the mapped image under the running service.
	// `_package/` (the dev *deploy* dir push-runtime-pf.sh copies to) is NOT
	// in this list — that is the intended local-iteration location.
	static const wchar_t *const build_markers[] = {
	    L"\\.claude\\worktrees\\", L"\\build\\",        L"\\cmake-build", L"\\out\\build\\", L"\\_deps\\",
	    L"\\x64\\debug\\",         L"\\x64\\release\\",
	};
	bool is_build_tree = false;
	for (size_t i = 0; i < sizeof(build_markers) / sizeof(build_markers[0]); ++i) {
		if (wcsstr(lower, build_markers[i]) != NULL) {
			is_build_tree = true;
			break;
		}
	}

	if (is_build_tree) {
		if (override_set()) {
			U_LOG_W(
			    "%s loader: %s: loading from a BUILD-TREE path under DXR_ALLOW_DEV_PLUGIN_PATHS "
			    "(%ls) — a concurrent rebuild can tear this image under the live service (#943).",
			    k, pid, (const wchar_t *)binary_path_w);
			return TARGET_PLUGIN_PATH_DEV;
		}
		U_LOG_E(
		    "%s loader: %s: REFUSING a build-tree DLL path (%ls) — registering a worktree/build-output "
		    "DLL under a live service is the #943 footgun. Copy-then-register (push-runtime-pf.sh) into "
		    "the runtime dir, or set DXR_ALLOW_DEV_PLUGIN_PATHS=1 to override.",
		    k, pid, (const wchar_t *)binary_path_w);
		return TARGET_PLUGIN_PATH_REFUSED;
	}

	// Everything else (e.g. _package/): allowed, one-shot warning.
	U_LOG_W(
	    "%s loader: %s: loading from a non-install path (%ls) — fine for local dev, but released "
	    "runtimes discover plug-ins from the install dir. (#952)",
	    k, pid, (const wchar_t *)binary_path_w);
	return TARGET_PLUGIN_PATH_DEV;
}

#else /* !XRT_OS_WINDOWS */

enum target_plugin_path_verdict
target_plugin_path_check(const void *binary_path_w, const char *id, const char *kind)
{
	(void)binary_path_w;
	(void)id;
	(void)kind;
	return TARGET_PLUGIN_PATH_TRUSTED;
}

#endif
