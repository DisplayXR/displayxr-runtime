// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cross-thread hand-off for MCP capture_frame requests.
 *
 * The MCP server thread fills a request with a target PNG path and
 * blocks on a condvar. The compositor thread polls at end-of-frame,
 * does the GPU→CPU readback + PNG encode, and signals done. Keeps
 * GPU calls on the thread that owns the resource.
 *
 * @ingroup aux_util
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_MCP_CAPTURE_PATH_MAX 256

/*!
 * Which tiles the caller wants written. The compositor resolves
 * LEFT/RIGHT against its active tile layout; if the current mode has
 * no stereo split (1-view mono), LEFT/RIGHT fall back to ATLAS.
 *
 * Semantics mirror the shell-phase8 IPC capture flags so the two
 * capture surfaces (agent-facing MCP, user-facing hotkey) produce
 * comparable file sets.
 */
enum u_mcp_capture_view
{
	U_MCP_CAPTURE_ATLAS = 1u << 0, //!< Full atlas (whatever the compositor composed)
	U_MCP_CAPTURE_LEFT = 1u << 1,  //!< Left eye tile as a standalone PNG
	U_MCP_CAPTURE_RIGHT = 1u << 2, //!< Right eye tile as a standalone PNG
};

#define U_MCP_CAPTURE_ALL (U_MCP_CAPTURE_ATLAS | U_MCP_CAPTURE_LEFT | U_MCP_CAPTURE_RIGHT)

struct u_mcp_capture_request
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	/*! Path *prefix* without extension — the compositor appends
	 *  `_atlas.png`, `_L.png`, `_R.png` per requested view. */
	char path_prefix[U_MCP_CAPTURE_PATH_MAX];
	uint32_t views; //!< Bitmask of @ref u_mcp_capture_view
	bool pending;
	bool done;
	bool success;
	uint32_t views_written; //!< Bitmask of what succeeded (subset of @c views)
};

/*!
 * Initialise a request (mutex + condvar). Call once in the compositor
 * creator. @ref u_mcp_capture_fini frees.
 */
void
u_mcp_capture_init(struct u_mcp_capture_request *req);

void
u_mcp_capture_fini(struct u_mcp_capture_request *req);

/*!
 * Register @p req as the MCP @c capture_frame handler. The MCP server
 * thread will fill the request and wait. Safe to call even when the
 * MCP server is not running — `oxr_mcp_tools_set_capture_handler` is
 * a no-op in that case.
 */
void
u_mcp_capture_install(struct u_mcp_capture_request *req);

/*!
 * Unregister. Call from the compositor destroy path before freeing @p req.
 */
void
u_mcp_capture_uninstall(void);

/*!
 * Check for a pending request. Returns true if one is in flight, in which
 * case @p out_path_prefix (must be at least @c U_MCP_CAPTURE_PATH_MAX
 * bytes) is filled and @p out_views holds the requested bitmask. The
 * caller must follow up with @ref u_mcp_capture_complete once the PNGs
 * are written (or failed).
 */
bool
u_mcp_capture_poll(struct u_mcp_capture_request *req, char *out_path_prefix, uint32_t *out_views);

/*!
 * Signal the waiting MCP thread. @p views_written records which
 * views actually produced PNGs; empty means total failure.
 */
void
u_mcp_capture_complete(struct u_mcp_capture_request *req, uint32_t views_written);

#ifdef __cplusplus
}
#endif
