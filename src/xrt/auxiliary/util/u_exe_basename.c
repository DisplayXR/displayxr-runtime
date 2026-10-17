// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Executable basenames as per-app keys (ADR-034 Amendment 5, #2181).
 * @ingroup aux_util
 */

#include "util/u_exe_basename.h"

#include "xrt/xrt_config_os.h"

#include <stdint.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(XRT_OS_MACOS)
#include <mach-o/dyld.h>
#include <limits.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

bool
u_exe_basename_valid(const char *name, size_t max_size)
{
	if (name == NULL || name[0] == '\0') {
		return false;
	}
	const size_t len = strlen(name);
	if (len + 1 > max_size) {
		return false;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		const unsigned char c = (unsigned char)name[i];
		if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') {
			return false;
		}
	}
	return true;
}

bool
u_exe_basename_from_path(const char *path, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	if (path == NULL) {
		return false;
	}
	const char *base = path;
	for (const char *p = path; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	if (!u_exe_basename_valid(base, cap)) {
		return false;
	}
	memcpy(out, base, strlen(base) + 1);
	return true;
}

bool
u_exe_basename_self(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	char path[PATH_MAX];
#ifdef XRT_OS_WINDOWS
	wchar_t wide[MAX_PATH * 4];
	const DWORD n = GetModuleFileNameW(NULL, wide, (DWORD)(sizeof(wide) / sizeof(wide[0])));
	if (n == 0 || n >= (DWORD)(sizeof(wide) / sizeof(wide[0]))) {
		return false;
	}
	if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, (int)sizeof(path), NULL, NULL) <= 0) {
		return false;
	}
#elif defined(XRT_OS_MACOS)
	uint32_t size = (uint32_t)sizeof(path);
	if (_NSGetExecutablePath(path, &size) != 0) {
		return false;
	}
#else
	const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (n <= 0 || (size_t)n >= sizeof(path) - 1) {
		return false;
	}
	path[n] = '\0';
#endif
	return u_exe_basename_from_path(path, out, cap);
}
