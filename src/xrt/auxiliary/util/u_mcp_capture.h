// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cross-thread hand-off for MCP capture_frame requests.
 *
 * The MCP server thread fills a request with a target PNG path and
 * blocks on a condvar. The compositor thread polls at end-of-frame,
 * does the GPU→CPU readback, encodes PNG, and signals done. Keeps
 * GPU calls on the thread that owns the resource.
 *
 * Each capture produces a single PNG of the content region that the
 * compositor hands to the display processor (tile_columns × view_width
 * by tile_rows × view_height) — i.e. what the app actually wrote into
 * its swapchain, not the worst-case atlas allocation.
 *
 * @ingroup aux_util
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_MCP_CAPTURE_PATH_MAX 256

struct u_mcp_capture_request
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	char path[U_MCP_CAPTURE_PATH_MAX];
	bool pending;
	bool done;
	bool success;
};

void
u_mcp_capture_init(struct u_mcp_capture_request *req);

void
u_mcp_capture_fini(struct u_mcp_capture_request *req);

/*!
 * Register @p req as the MCP @c capture_frame handler. Safe to call
 * even when the MCP server is not running.
 */
void
u_mcp_capture_install(struct u_mcp_capture_request *req);

/*!
 * Unregister. Call from compositor destroy before freeing @p req.
 */
void
u_mcp_capture_uninstall(void);

/*!
 * Check for a pending request. Returns true if one is in flight and
 * fills @p out_path (must be at least @c U_MCP_CAPTURE_PATH_MAX bytes).
 * Caller must follow up with @ref u_mcp_capture_complete after the PNG
 * is written (or failed).
 */
bool
u_mcp_capture_poll(struct u_mcp_capture_request *req, char *out_path);

/*!
 * Signal the waiting MCP thread.
 */
void
u_mcp_capture_complete(struct u_mcp_capture_request *req, bool success);

/*!
 * Return the currently installed capture request, or NULL if none.
 * Used by the service-side capture_frame tool to submit requests
 * directly to the compositor's capture handler.
 */
struct u_mcp_capture_request *
u_mcp_capture_get_installed(void);

/*!
 * Blocking handler for the Phase A per-PID capture_frame tool. Submits
 * a capture request and waits (up to 3 s) for the compositor thread to
 * complete it. Called from the oxr tool_capture_frame handler.
 */
bool
u_mcp_capture_blocking_handler(const char *path, void *userdata);

typedef void (*u_mcp_capture_notify_fn)(void *req);

/*!
 * Register optional callbacks for Phase A (per-PID) integration.
 * Called by the oxr state tracker at startup to wire capture_frame
 * into the per-PID MCP server. Service processes don't call this.
 */
void
u_mcp_capture_set_notify(u_mcp_capture_notify_fn on_install,
                         u_mcp_capture_notify_fn on_uninstall);

#ifdef __cplusplus
}
#endif
