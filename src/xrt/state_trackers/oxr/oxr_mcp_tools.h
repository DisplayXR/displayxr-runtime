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
 * Latch a capture request on the installed in-process compositor handler and
 * return immediately — does NOT block. The compositor consumes the latch at
 * its next layer_commit poll (the next xrEndFrame), does the GPU readback, and
 * writes the PNG to @p path. This is the non-blocking sibling of the MCP
 * @c capture_frame tool's blocking handler: it must NOT block here because
 * @ref oxr_xrCaptureAtlasEXT runs on the app's own xrEndFrame thread, which is
 * also the thread that drives the in-process compositor's layer_commit —
 * blocking it would deadlock the very poll we are waiting on.
 *
 * @p mode is an @ref mcp_capture_mode value. Returns @c false if no in-process
 * compositor handler is installed (e.g. an IPC/service session). The PNG
 * exists shortly after the next composed frame, not when this call returns.
 */
bool
oxr_mcp_tools_submit_capture(const char *path, uint32_t mode);

/*!
 * Read the MCP capability marker written by the displayxr-mcp installer.
 * Returns @c true iff the marker is present AND set to enabled.
 *
 *   Windows: @c HKLM\Software\DisplayXR\Capabilities\MCP\Enabled
 *            (REG_DWORD == 1), 64-bit registry view.
 *   macOS:   @c /Library/Application Support/DisplayXR/Capabilities/MCP/Enabled
 *            (first byte == @c '1', written root:wheel 0644 by the
 *            postinstall script in DisplayXRMCP-*.pkg).
 *
 * Combine with @ref mcp_check_env_or so the @c DISPLAYXR_MCP env var
 * still wins as an explicit override:
 *
 *   if (mcp_check_env_or(oxr_mcp_capability_enabled())) {
 *       mcp_server_start();
 *   }
 *
 * Caller-side helper rather than framework-side because the marker
 * path is DisplayXR-specific; the framework stays consumer-agnostic.
 */
bool
oxr_mcp_capability_enabled(void);

#ifdef __cplusplus
}
#endif
