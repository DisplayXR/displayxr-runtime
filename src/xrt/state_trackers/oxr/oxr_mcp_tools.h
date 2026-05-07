// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MCP tool implementations that read OpenXR state tracker state.
 *         Registered from @ref oxr_instance_create alongside
 *         @ref mcp_server_maybe_start.
 * @ingroup oxr_main
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <openxr/openxr.h>

#include <stdbool.h>
#include <stdint.h>

struct oxr_session;
struct xrt_layer_data;

/*!
 * Register the tool table with the MCP server. Idempotent; safe when the
 * MCP server is disabled (registrations are harmless).
 */
void
oxr_mcp_tools_register_all(void);

/*!
 * Publish the current session so tool handlers can read its state.
 * Called from @ref oxr_session_create on success.
 */
void
oxr_mcp_tools_attach_session(struct oxr_session *sess);

/*!
 * Clear the session pointer. Called from @ref oxr_session_destroy before
 * the session backing memory is freed.
 */
void
oxr_mcp_tools_detach_session(struct oxr_session *sess);

/*!
 * Record the recommended per-view pose + fov as returned by
 * xrLocateViews. Called once per locate on the app thread.
 */
void
oxr_mcp_tools_record_recommended(struct oxr_session *sess,
                                 uint32_t view_count,
                                 const XrView *views,
                                 uint64_t display_time_ns);

/*!
 * Record the app-submitted projection layer data at frame submit
 * and atomically publish the completed snapshot. Called once per
 * successfully submitted projection layer from @ref oxr_session_frame_end.
 */
void
oxr_mcp_tools_record_submitted(struct oxr_session *sess, const struct xrt_layer_data *data);

/*!
 * Register a per-compositor capture handler. The handler is invoked on
 * the MCP server thread with a target path and a capture mode (see
 * @ref mcp_capture_mode); it must write a PNG matching the requested
 * mode to that path (synchronously or with its own synchronization)
 * and return true on success. The state tracker passes a stable path
 * of the form `/tmp/displayxr-mcp-capture-<pid>-<frame>{,.projection}.png`.
 *
 * Pass NULL to unregister.
 */
typedef bool (*oxr_mcp_capture_fn)(const char *path, uint32_t mode, void *userdata);

void
oxr_mcp_tools_set_capture_handler(oxr_mcp_capture_fn fn, void *userdata);

/*!
 * Read the @c HKLM\Software\DisplayXR\Capabilities\MCP\Enabled DWORD on
 * Windows. Returns @c true iff the key exists and the value is 1. On
 * non-Windows platforms (no installer infrastructure) returns @c false.
 *
 * Combine with @ref mcp_check_env_or so the @c DISPLAYXR_MCP env var
 * still wins as an explicit override:
 *
 *   if (mcp_check_env_or(oxr_mcp_capability_enabled())) {
 *       mcp_server_start();
 *   }
 *
 * Caller-side helper rather than framework-side because the registry
 * path is DisplayXR-specific; the framework stays consumer-agnostic.
 */
bool
oxr_mcp_capability_enabled(void);

#ifdef __cplusplus
}
#endif
