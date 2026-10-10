// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  POSIX MCP capability marker (see u_mcp_capability.h).
 * @ingroup aux_util
 */

#include "u_mcp_capability.h"

#include "xrt/xrt_config_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef XRT_OS_WINDOWS
#include <fcntl.h>
#include <unistd.h>
#endif

int
u_mcp_capability_marker_paths(char out[U_MCP_CAPABILITY_MAX_PATHS][U_MCP_CAPABILITY_PATH_MAX])
{
	int n = 0;
#if defined(XRT_OS_MACOS)
	snprintf(out[n++], U_MCP_CAPABILITY_PATH_MAX, "%s",
	         "/Library/Application Support/DisplayXR/Capabilities/MCP/Enabled");
#elif defined(XRT_OS_LINUX_DESKTOP)
	// Per-user first (no root needed to opt in), then system-wide. Same
	// layout the DisplayXR Shell's Linux port writes.
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg != NULL && xdg[0] == '/') {
		snprintf(out[n++], U_MCP_CAPABILITY_PATH_MAX, "%s/displayxr/capabilities/mcp/Enabled", xdg);
	} else if (home != NULL && home[0] != '\0') {
		snprintf(out[n++], U_MCP_CAPABILITY_PATH_MAX, "%s/.config/displayxr/capabilities/mcp/Enabled", home);
	}
	snprintf(out[n++], U_MCP_CAPABILITY_PATH_MAX, "%s", "/etc/displayxr/capabilities/mcp/Enabled");
#else
	(void)out;
#endif
	return n;
}

int
u_mcp_capability_read_marker(const char *path)
{
#ifdef XRT_OS_WINDOWS
	(void)path;
	return -1;
#else
	if (path == NULL || path[0] == '\0') {
		return -1;
	}
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	char b = 0;
	ssize_t r = read(fd, &b, 1);
	close(fd);
	return (r == 1 && b == '1') ? 1 : 0;
#endif
}

bool
u_mcp_capability_marker_enabled(void)
{
	char paths[U_MCP_CAPABILITY_MAX_PATHS][U_MCP_CAPABILITY_PATH_MAX];
	int n = u_mcp_capability_marker_paths(paths);
	for (int i = 0; i < n; i++) {
		int v = u_mcp_capability_read_marker(paths[i]);
		if (v >= 0) {
			return v == 1;
		}
	}
	return false;
}
