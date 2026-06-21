// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of @ref u_capability_enabled.
 * @ingroup aux_util
 */

#include "xrt/xrt_config_os.h"

#include "util/u_capability.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(XRT_OS_WINDOWS) && !defined(XRT_ENV_MINGW)
#include "xrt/xrt_windows.h"
#elif defined(XRT_OS_LINUX) || defined(XRT_OS_MACOS)
#include <fcntl.h>
#include <unistd.h>
#endif

/*
 * Returns true and writes *out_value when @p env_var names a set, non-empty
 * environment variable; false (leaving *out_value untouched) otherwise.
 */
static bool
env_override(const char *env_var, bool *out_value)
{
	if (env_var == NULL) {
		return false;
	}
	const char *e = getenv(env_var);
	if (e == NULL || e[0] == '\0') {
		return false;
	}
	if (e[0] == '0' || strcmp(e, "false") == 0 || strcmp(e, "off") == 0 || strcmp(e, "no") == 0) {
		*out_value = false;
	} else {
		*out_value = true;
	}
	return true;
}

/*
 * Returns true and writes *out_value when the machine capability marker for
 * @p cap_name exists; false otherwise. The marker is registry-backed on Windows
 * and a file on macOS/Linux; on the MinGW dev-check build (Windows target without
 * the Win32 registry path) there is no marker support and this always returns false.
 */
static bool
registry_marker(const char *cap_name, bool *out_value)
{
	if (cap_name == NULL) {
		return false;
	}
#if defined(XRT_OS_WINDOWS) && !defined(XRT_ENV_MINGW)
	char subkey[256];
	int n = snprintf(subkey, sizeof(subkey), "Software\\DisplayXR\\Capabilities\\%s", cap_name);
	if (n <= 0 || (size_t)n >= sizeof(subkey)) {
		return false;
	}
	HKEY key = NULL;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
		return false;
	}
	DWORD value = 0;
	DWORD value_size = sizeof(value);
	DWORD value_type = 0;
	LSTATUS rc = RegQueryValueExA(key, "Enabled", NULL, &value_type, (LPBYTE)&value, &value_size);
	RegCloseKey(key);
	if (rc != ERROR_SUCCESS || value_type != REG_DWORD) {
		return false;
	}
	*out_value = (value == 1);
	return true;
#elif defined(XRT_OS_LINUX) || defined(XRT_OS_MACOS)
	char path[256];
	int n = snprintf(path, sizeof(path),
	                 "/Library/Application Support/DisplayXR/Capabilities/%s/Enabled", cap_name);
	if (n <= 0 || (size_t)n >= sizeof(path)) {
		return false;
	}
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return false;
	}
	char b = 0;
	ssize_t r = read(fd, &b, 1);
	close(fd);
	if (r != 1) {
		return false;
	}
	*out_value = (b == '1');
	return true;
#else
	(void)out_value;
	return false;
#endif
}

bool
u_capability_enabled(const char *env_var, const char *cap_name, bool default_value)
{
	bool value = false;
	if (env_override(env_var, &value)) {
		return value;
	}
	if (registry_marker(cap_name, &value)) {
		return value;
	}
	return default_value;
}
