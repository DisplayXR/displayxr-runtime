// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MCP shell-scope tools registered in-process inside displayxr-service.
 * @ingroup ipc_server
 *
 * These tools back Phase B of the MCP v0.2 spec: list_windows,
 * get/set_window_pose, set_focus, apply_layout_preset, save/load_workspace.
 * They run on the MCP server thread started by u_mcp_server_maybe_start_named()
 * and read/write shell state via the same IPC paths the shell uses.
 *
 * @see docs/roadmap/mcp-phase-b-plan.md
 */

#pragma once

#include "server/ipc_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Register the Phase B shell tools against the global MCP server.
 *
 * Must be called after @c u_mcp_server_maybe_start_named("service") so the
 * server-thread picks the tools up via the shared registry. Stashes @p s
 * in a file-static global so tool handlers can look up per-client state
 * without routing through a userdata ABI change.
 *
 * Safe to call when MCP is disabled — the registry is harmless when the
 * server thread was never spawned.
 */
void
ipc_mcp_tools_register(struct ipc_server *s);

#ifdef __cplusplus
}
#endif
