// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Opt-in in-process MCP server for DisplayXR (Phase A).
 * @ingroup aux_util
 *
 * Enabled when the @c DISPLAYXR_MCP environment variable is set at
 * xrInstanceCreate time. Spawns a detached thread that binds a per-PID
 * unix socket and speaks MCP-style JSON-RPC 2.0 framed by Content-Length
 * headers.
 *
 * @see docs/roadmap/mcp-spec-v0.2.md
 * @see docs/roadmap/mcp-phase-a-plan.md
 */

#pragma once

#include <cjson/cJSON.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Tool handler callback. @p params is the JSON-RPC `params` object (may be
 * NULL). The handler returns a cJSON node that becomes the `result` field,
 * or NULL to signal an error — in which case the server replies with a
 * JSON-RPC internal-error response.
 */
typedef cJSON *(*u_mcp_tool_fn)(const cJSON *params, void *userdata);

/*!
 * Tool descriptor shown by `tools/list` and dispatched by `tools/call`.
 * All fields except @p fn are optional; the MCP protocol requires a
 * name + description and accepts a JSON Schema for parameters.
 */
struct u_mcp_tool
{
	const char *name;
	const char *description;
	const char *input_schema_json; //!< Optional static JSON Schema string.
	u_mcp_tool_fn fn;
	void *userdata;
};

/*!
 * Start the server if @c DISPLAYXR_MCP is set, bound to a per-PID socket.
 * Idempotent no-op if already running or env var unset. Best-effort:
 * failure logs U_LOG_W and returns without aborting instance creation.
 *
 * Used by handle apps that link @c libopenxr_displayxr in-process.
 */
void
u_mcp_server_maybe_start(void);

/*!
 * Start the server if @c DISPLAYXR_MCP is set, bound to a well-known
 * named socket (`/tmp/displayxr-mcp-<role>.sock` on POSIX,
 * `\\.\pipe\displayxr-mcp-<role>` on Windows). Idempotent.
 *
 * Used by singleton processes like @c displayxr-service where the adapter
 * should be able to discover the endpoint by role rather than PID.
 */
void
u_mcp_server_maybe_start_named(const char *role);

/*!
 * Stop the server (joins the thread, unlinks the socket). Safe to call
 * when the server was never started.
 */
void
u_mcp_server_stop(void);

/*!
 * Register a tool. Safe to call before or after the server starts.
 * The descriptor pointer must outlive the server (use static storage).
 */
void
u_mcp_server_register_tool(const struct u_mcp_tool *tool);

#ifdef __cplusplus
}
#endif
