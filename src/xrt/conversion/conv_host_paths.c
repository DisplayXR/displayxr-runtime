// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The directories a conversion host hands a provider (see conv_host_paths.h).
 * @ingroup aux_conversion
 */

#include "conv_host_paths.h"
#include "conv_model_store.h"

#include "xrt/xrt_conversion_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define PATHS_MAX 512

static bool
set(const char *s)
{
	return s != NULL && s[0] != '\0';
}

//! snprintf that reports truncation as failure.
static bool
fmt_fits(char *out, size_t out_size, int n)
{
	return out != NULL && n >= 0 && (size_t)n < out_size;
}

bool
conv_scratch_root_from(const struct conv_scratch_root_inputs *in, char *out, size_t out_size)
{
	if (out == NULL || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	if (in == NULL) {
		return false;
	}
	int n = -1;
	if (set(in->env_override)) {
		n = snprintf(out, out_size, "%s", in->env_override);
	} else if (in->os == CONV_SCRATCH_OS_WINDOWS) {
		if (set(in->local_app_data)) {
			n = snprintf(out, out_size, "%s\\DisplayXR\\Conversion", in->local_app_data);
		}
	} else if (in->os == CONV_SCRATCH_OS_MACOS) {
		if (set(in->home)) {
			n = snprintf(out, out_size, "%s/Library/Caches/DisplayXR/Conversion", in->home);
		}
	} else {
		// XDG: a relative $XDG_CACHE_HOME is invalid and must be ignored.
		if (set(in->xdg_cache_home) && in->xdg_cache_home[0] == '/') {
			n = snprintf(out, out_size, "%s/displayxr/conversion", in->xdg_cache_home);
		} else if (set(in->home)) {
			n = snprintf(out, out_size, "%s/.cache/displayxr/conversion", in->home);
		}
	}
	if (!fmt_fits(out, out_size, n)) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool
conv_scratch_root_resolve(char *out, size_t out_size)
{
	struct conv_scratch_root_inputs in;
	memset(&in, 0, sizeof(in));
	in.env_override = getenv("DXR_CONV_SCRATCH_ROOT");
#ifdef XRT_OS_WINDOWS
	in.os = CONV_SCRATCH_OS_WINDOWS;
	char lad[PATHS_MAX] = "";
	wchar_t w[PATHS_MAX];
	const DWORD wn = GetEnvironmentVariableW(L"LOCALAPPDATA", w, PATHS_MAX);
	if (wn > 0 && wn < PATHS_MAX && WideCharToMultiByte(CP_UTF8, 0, w, -1, lad, (int)sizeof(lad), NULL, NULL) > 0) {
		in.local_app_data = lad;
	}
#else
#ifdef XRT_OS_MACOS
	in.os = CONV_SCRATCH_OS_MACOS;
#else
	in.os = CONV_SCRATCH_OS_LINUX;
	in.xdg_cache_home = getenv("XDG_CACHE_HOME");
#endif
	in.home = getenv("HOME");
	if (!set(in.home)) {
		// A service started without HOME (a bare launchd / systemd unit).
		const struct passwd *pw = getpwuid(getuid());
		in.home = pw != NULL ? pw->pw_dir : NULL;
	}
#endif
	return conv_scratch_root_from(&in, out, out_size);
}

bool
conv_scratch_id_valid(const char *id)
{
	if (!set(id) || strcmp(id, ".") == 0 || strcmp(id, "..") == 0) {
		return false;
	}
	size_t n = 0;
	for (const char *c = id; *c != '\0'; c++, n++) {
		const char ch = *c;
		const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
		                ch == '.' || ch == '_' || ch == '-';
		if (!ok || n >= 63) {
			return false;
		}
	}
	return true;
}

static bool
is_dir(const char *path)
{
#ifdef XRT_OS_WINDOWS
	wchar_t w[PATHS_MAX];
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, PATHS_MAX) <= 0) {
		return false;
	}
	const DWORD a = GetFileAttributesW(w);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool
mkdir_one(const char *path)
{
#ifdef XRT_OS_WINDOWS
	wchar_t w[PATHS_MAX];
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, PATHS_MAX) <= 0) {
		return false;
	}
	return CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
	// Owner-only: what a provider caches is nobody else's business. An
	// existing directory keeps its mode (we never chmod what we did not make).
	return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

//! mkdir -p: every missing component of @p path, then @p path itself.
static bool
mkdir_p(const char *path)
{
	char tmp[PATHS_MAX];
	if (!fmt_fits(tmp, sizeof(tmp), snprintf(tmp, sizeof(tmp), "%s", path))) {
		return false;
	}
	const size_t n = strlen(tmp);
	for (size_t i = 1; i < n; i++) {
		if (tmp[i] != '/' && tmp[i] != '\\') {
			continue;
		}
		if (i == 2 && tmp[1] == ':') {
			continue; // "C:\"
		}
		const char c = tmp[i];
		tmp[i] = '\0';
		if (!is_dir(tmp)) {
			(void)mkdir_one(tmp);
		}
		tmp[i] = c;
	}
	return mkdir_one(tmp) && is_dir(tmp);
}

bool
conv_scratch_dir(const char *id, char *out, size_t out_size)
{
	if (out == NULL || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!conv_scratch_id_valid(id)) {
		return false;
	}
	char root[PATHS_MAX];
	if (!conv_scratch_root_resolve(root, sizeof(root))) {
		return false;
	}
	char dir[PATHS_MAX];
	const size_t rl = strlen(root);
	const bool has_sep = rl > 0 && (root[rl - 1] == '/' || root[rl - 1] == '\\');
#ifdef XRT_OS_WINDOWS
	const char *sep = has_sep ? "" : "\\";
#else
	const char *sep = has_sep ? "" : "/";
#endif
	if (!fmt_fits(dir, sizeof(dir), snprintf(dir, sizeof(dir), "%s%s%s", root, sep, id))) {
		return false;
	}
	if (!mkdir_p(dir)) {
		return false;
	}
	if (!fmt_fits(out, out_size, snprintf(out, out_size, "%s", dir))) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool
conv_host_get_path(const char *id, uint32_t kind, const char *log_file, char *buf, uint32_t buf_size)
{
	if (buf == NULL || buf_size == 0) {
		return false;
	}
	buf[0] = '\0';
	char path[PATHS_MAX];
	path[0] = '\0';
	bool ok = false;
	switch (kind) {
	case XRT_CONV_PATH_SCRATCH: ok = conv_scratch_dir(id, path, sizeof(path)); break;
	case XRT_CONV_PATH_MODEL_STORE: {
		// The root the process store resolved (it may not exist yet: the store
		// creates it with the first blob), else what it would resolve.
		const struct conv_model_store *s = conv_store_host_get();
		const char *root = s != NULL ? conv_model_store_root(s) : NULL;
		if (set(root)) {
			ok = fmt_fits(path, sizeof(path), snprintf(path, sizeof(path), "%s", root));
		} else {
			ok = conv_store_root_resolve(path, sizeof(path), NULL, 0);
		}
		break;
	}
	case XRT_CONV_PATH_LOGS: {
		if (!set(log_file) || !fmt_fits(path, sizeof(path), snprintf(path, sizeof(path), "%s", log_file))) {
			break;
		}
		char *last = NULL;
		for (char *c = path; *c != '\0'; c++) {
			if (*c == '/' || *c == '\\') {
				last = c;
			}
		}
		if (last != NULL && last != path) {
			*last = '\0';
			ok = true;
		}
		break;
	}
	default: break;
	}
	if (!ok || !set(path) || strlen(path) >= buf_size) {
		return false;
	}
	memcpy(buf, path, strlen(path) + 1);
	return true;
}
