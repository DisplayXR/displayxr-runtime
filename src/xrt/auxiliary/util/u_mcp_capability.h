// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  POSIX MCP capability marker: where it lives and how it is read.
 *
 * The MCP capability gate (installer-first, `DISPLAYXR_MCP` env override; see
 * oxr_mcp_capability_enabled) is a one-byte file whose first byte is `'1'`:
 *
 *   macOS:          `/Library/Application Support/DisplayXR/Capabilities/MCP/Enabled`
 *   desktop Linux:  `$XDG_CONFIG_HOME/displayxr/capabilities/mcp/Enabled`
 *                   (default `~/.config/displayxr/capabilities/mcp/Enabled`),
 *                   then `/etc/displayxr/capabilities/mcp/Enabled`
 *
 * The first marker that EXISTS decides (a per-user `0` disables even when the
 * system marker says `1`). Windows uses the registry and never calls this.
 * Spec: docs/specs/extensions/XR_DXR_mcp_tools.md § Capability gate.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Maximum number of marker paths u_mcp_capability_marker_paths returns.
#define U_MCP_CAPABILITY_MAX_PATHS 2

//! Path buffer size per marker.
#define U_MCP_CAPABILITY_PATH_MAX 1024

/*!
 * Fill @p out with this platform's marker paths, in priority order. Reads
 * `XDG_CONFIG_HOME` / `HOME` on Linux. Returns the number written (0 on
 * platforms without a file marker).
 */
int
u_mcp_capability_marker_paths(char out[U_MCP_CAPABILITY_MAX_PATHS][U_MCP_CAPABILITY_PATH_MAX]);

/*!
 * Read one marker. Returns 1 (exists, first byte `'1'`), 0 (exists, anything
 * else) or -1 (absent / unreadable).
 */
int
u_mcp_capability_read_marker(const char *path);

/*!
 * The POSIX capability check: the first existing marker decides.
 */
bool
u_mcp_capability_marker_enabled(void);

#ifdef __cplusplus
}
#endif
