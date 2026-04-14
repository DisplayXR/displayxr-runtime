// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MCP tool implementations that read OpenXR state tracker state.
 *         Registered from @ref oxr_instance_create alongside
 *         @ref u_mcp_server_maybe_start.
 * @ingroup oxr_main
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <openxr/openxr.h>

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

#ifdef __cplusplus
}
#endif
