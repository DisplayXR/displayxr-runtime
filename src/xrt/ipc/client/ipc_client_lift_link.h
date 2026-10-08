// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift (ADR-042, ADR-049) for IN-PROCESS sessions: a lift-only
 *         service connection owned by the session.
 *
 * An in-process session renders on its own native compositor and has no IPC
 * connection. Lift streams belong to an IPC connection, not to a session (the
 * property `displayxr-cli lift` already relies on), so an in-process session
 * reaches the service's ONE conversion module through a second, sessionless
 * connection that carries lift calls only (ADR-049 option C).
 *
 * The link never blocks the caller on the connect: the first use starts a
 * background connect (a pipe open, the version-tag handshake, at worst the
 * pipe-busy / service-starting retry windows) and reports CONNECTING until it
 * finishes. A failed connect is retried at most every few seconds. A connection
 * that dies is DEAD: its streams are gone with it, the link reconnects on the
 * next use once nobody still holds the old connection, and the GENERATION
 * counter lets callers tell a stream of the dead connection from a live one.
 *
 * Thread-safe. The connection pointer handed out by
 * ipc_client_lift_link_acquire() stays valid until the matching release.
 *
 * @ingroup ipc_client
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_connection;
struct ipc_client_lift_link;

enum ipc_client_lift_link_state
{
	IPC_CLIENT_LIFT_LINK_CONNECTING = 0, //!< a connect is in flight (report ACTIVATING)
	IPC_CLIENT_LIFT_LINK_CONNECTED = 1,  //!< usable
	IPC_CLIENT_LIFT_LINK_FAILED = 2,     //!< no service / refused; retried after a back-off
	IPC_CLIENT_LIFT_LINK_DEAD = 3,       //!< was connected, the pipe died; reconnects on next use
};

/*!
 * Create the link; no connect is attempted until the first
 * ipc_client_lift_link_acquire(). @p label names the connection in the
 * service's client list (e.g. "<exe> (in-process lift)").
 */
xrt_result_t
ipc_client_lift_link_create(const char *label, struct ipc_client_lift_link **out_link);

/*!
 * Disconnect and free. Waits for an in-flight connect to finish (bounded by the
 * connect's own retry windows). Every stream of the connection dies with it on
 * the service side. NULL-safe; clears @p *link_ptr.
 */
void
ipc_client_lift_link_destroy(struct ipc_client_lift_link **link_ptr);

/*!
 * Get the connection if the link is CONNECTED, starting (or restarting, after
 * the back-off) a background connect otherwise. Never blocks on the connect.
 *
 * @param      link        The link.
 * @param[out] out_state   The state after this call.
 * @param[out] out_gen     The connection generation (bumped on every new
 *                         connection); valid when a connection is returned.
 * @return The connection, which the caller must hand back with
 *         ipc_client_lift_link_release(); NULL unless CONNECTED.
 */
struct ipc_connection *
ipc_client_lift_link_acquire(struct ipc_client_lift_link *link,
                             enum ipc_client_lift_link_state *out_state,
                             uint32_t *out_gen);

/*!
 * Like ipc_client_lift_link_acquire(), but when a connect is in flight wait up
 * to @p timeout_ms for it to finish. For setup calls (stream creation), never
 * for per-frame ones.
 */
struct ipc_connection *
ipc_client_lift_link_acquire_wait(struct ipc_client_lift_link *link,
                                  uint32_t timeout_ms,
                                  enum ipc_client_lift_link_state *out_state,
                                  uint32_t *out_gen);

/*!
 * Hand back a connection from acquire. @p ipc_failed: the call made on it
 * returned XRT_ERROR_IPC_FAILURE — the link turns DEAD (one WARN) and the
 * connection is closed once its last user has released it.
 */
void
ipc_client_lift_link_release(struct ipc_client_lift_link *link, uint32_t gen, bool ipc_failed);

#ifdef __cplusplus
}
#endif
