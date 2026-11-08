// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The in-process SERVICE LINK (ADR-049 Amendment 1, ADR-061 decision
 *         16): one sessionless service connection per in-process session,
 *         carrying its lift calls and its share capability.
 *
 * An in-process session renders on its own native compositor and has no IPC
 * connection. Lift streams belong to an IPC connection, not to a session (the
 * property `displayxr-cli lift` already relies on), so an in-process session
 * reaches the service's ONE conversion module through a second, sessionless
 * connection (ADR-049 option C). ADR-061 reuses that same connection to offer
 * the session's pre-weave views as a share source, so a session costs ONE
 * `APP` slot of IPC_MAX_CLIENTS, never two. The capabilities it declares are
 * fixed at create (@ref xrt_share_link_caps):
 *
 *  - LIFT only: exactly the ADR-049 lift link. It opens on the first lift
 *    call, sends the conversion prewarm hint, and never registers.
 *  - SHARE (with or without LIFT): a share WORKER thread owns everything that
 *    waits on the service for the share capability — it dials once the
 *    session runs AND its compositor reported a tap (or a reason it has
 *    none), retries a failed dial every 5 s, sends the registration
 *    (`service_link_register`) on every connect and change, and polls every
 *    250 ms. The app's API thread and the compositor's per-frame poll only
 *    take a mutex; only frame_ready and the ring publish (which carry the
 *    compositor's frame) run on the caller's thread.
 *
 * The link never blocks the caller on the connect: the first use starts a
 * background connect (a pipe open, the version-tag handshake, at worst the
 * pipe-busy / service-starting retry windows) and reports CONNECTING until it
 * finishes. It never launches the service (Windows: it dials only while the
 * service's singleton exists). A failed connect is retried at most every 5 s.
 * A connection that dies is DEAD: its streams are gone with it, the link
 * reconnects on the next use once nobody still holds the old connection, and
 * the GENERATION counter lets callers tell a stream of the dead connection
 * from a live one.
 *
 * Thread-safe. The connection pointer handed out by
 * ipc_client_service_link_acquire() stays valid until the matching release.
 *
 * Renamed from `ipc_client_lift_link` (no aliases: nothing outside this
 * repository links the IPC client).
 *
 * @ingroup ipc_client
 */

#pragma once

#include "xrt/xrt_handles.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_share_link.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_connection;
struct ipc_client_service_link;

enum ipc_client_service_link_state
{
	IPC_CLIENT_SERVICE_LINK_CONNECTING = 0, //!< a connect is in flight (report ACTIVATING)
	IPC_CLIENT_SERVICE_LINK_CONNECTED = 1,  //!< usable
	IPC_CLIENT_SERVICE_LINK_FAILED = 2,     //!< no service / refused; retried after a back-off
	IPC_CLIENT_SERVICE_LINK_DEAD = 3,       //!< was connected, the pipe died; reconnects on next use
};

/*!
 * Create the link; no connect is attempted until the first acquire / kick.
 * @p label names the connection in the service's client list
 * ("<exe> (in-process)"). @p caps: enum xrt_share_link_caps, fixed for the
 * link's life.
 */
xrt_result_t
ipc_client_service_link_create(const char *label, uint32_t caps, struct ipc_client_service_link **out_link);

/*!
 * Disconnect and free. Waits for an in-flight connect to finish (bounded by the
 * connect's own retry windows). Every stream of the connection dies with it on
 * the service side, and a share of it ends SOURCE_LOST. NULL-safe; clears
 * @p *link_ptr.
 */
void
ipc_client_service_link_destroy(struct ipc_client_service_link **link_ptr);

//! The capabilities the link was created with (enum xrt_share_link_caps).
uint32_t
ipc_client_service_link_caps(const struct ipc_client_service_link *link);

/*!
 * Get the connection if the link is CONNECTED, starting (or restarting, after
 * the back-off) a background connect otherwise. Never blocks on the connect.
 *
 * @param      link        The link.
 * @param[out] out_state   The state after this call.
 * @param[out] out_gen     The connection generation (bumped on every new
 *                         connection); valid when a connection is returned.
 * @return The connection, which the caller must hand back with
 *         ipc_client_service_link_release(); NULL unless CONNECTED.
 */
struct ipc_connection *
ipc_client_service_link_acquire(struct ipc_client_service_link *link,
                                enum ipc_client_service_link_state *out_state,
                                uint32_t *out_gen);

/*!
 * Like ipc_client_service_link_acquire(), but when a connect is in flight wait
 * up to @p timeout_ms for it to finish. For setup calls (stream creation),
 * never for per-frame ones.
 */
struct ipc_connection *
ipc_client_service_link_acquire_wait(struct ipc_client_service_link *link,
                                     uint32_t timeout_ms,
                                     enum ipc_client_service_link_state *out_state,
                                     uint32_t *out_gen);

/*!
 * Hand back a connection from acquire. @p ipc_failed: the call made on it
 * returned XRT_ERROR_IPC_FAILURE — the link turns DEAD (one WARN) and the
 * connection is closed once its last user has released it.
 */
void
ipc_client_service_link_release(struct ipc_client_service_link *link, uint32_t gen, bool ipc_failed);


/*
 *
 * The share capability (ADR-061 arm 2). Every call below is a no-op on a link
 * created without XRT_SHARE_LINK_CAP_SHARE.
 *
 */

/*!
 * Wake the share worker (it connects when the session runs and the compositor
 * reported its tap state). Never blocks. What xrBeginSession calls.
 */
void
ipc_client_service_link_kick(struct ipc_client_service_link *link);

/*!
 * Replace the registration (`service_link_register`). The share worker sends
 * the newest one (this call never waits on the service).
 * @ref xrt_share_link_registration::caps is overwritten with the link's.
 */
void
ipc_client_service_link_set_registration(struct ipc_client_service_link *link,
                                         const struct xrt_share_link_registration *reg);

/*!
 * The compositor's tap flag and adapter (comp_xbridge_share_tap.h), merged
 * into the registration; re-sent (by the worker) only when they changed.
 * @p luid_valid false clears a previously reported adapter.
 * @p unavailable_reason: enum xrt_app_share_availability when not available.
 */
void
ipc_client_service_link_set_tap(struct ipc_client_service_link *link,
                                bool available,
                                uint32_t unavailable_reason,
                                const uint8_t luid[XRT_LUID_SIZE],
                                bool luid_valid);

/*!
 * What the service asks of this link — the newest answer the worker's
 * 250 ms poll or a frame_ready reply brought, read under a mutex: never an
 * IPC call, safe every frame. SHARE_STOPPED is reported exactly once across
 * this call and frame_ready's @p out_poll, then NONE.
 *
 * @return true if @p out is valid (connected, registered); false = NONE.
 */
bool
ipc_client_service_link_share_poll(struct ipc_client_service_link *link, struct xrt_share_link_poll *out);

/*!
 * Publish the ring (`service_link_share_publish_ring`, then
 * `service_link_share_publish_fence` when @p fence is valid — the ring's
 * `has_fence` is set from it). @p textures:
 * @p texture_count (0 or XRT_SHARE_LINK_RING_SLOTS) shared handles the
 * caller keeps owning — the transport duplicates them into the service.
 */
xrt_result_t
ipc_client_service_link_share_publish_ring(struct ipc_client_service_link *link,
                                           const struct xrt_share_link_ring_desc *desc,
                                           const xrt_graphics_buffer_handle_t *textures,
                                           uint32_t texture_count,
                                           xrt_graphics_sync_handle_t fence);

/*!
 * One finished frame (`service_link_share_frame_ready`). @p out_poll (may be
 * NULL): the service's answer, under the same single-report rule as the
 * poll. Only XRT_ERROR_IPC_FAILURE (a dead pipe) affects the link; a
 * refusal (XRT_ERROR_OUTPUT_REQUEST_FAILURE, …) is just returned.
 */
xrt_result_t
ipc_client_service_link_share_frame_ready(struct ipc_client_service_link *link,
                                          const struct xrt_share_link_frame *frame,
                                          struct xrt_share_link_poll *out_poll);

#ifdef __cplusplus
}
#endif
