// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Transport abstraction for the MCP server — unix socket on POSIX,
 *         named pipe on Windows (Phase A slice 7).
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_mcp_listener;
struct u_mcp_conn;

/*!
 * Bind a per-PID listener. Returns NULL on failure.
 *
 * On POSIX this creates `/tmp/displayxr-mcp-<pid>.sock` with 0600 perms.
 * The caller unlinks via u_mcp_listener_close().
 */
struct u_mcp_listener *
u_mcp_listener_open(pid_t pid);

/*!
 * Accept one connection. Blocks. Returns NULL when the listener is closed
 * (u_mcp_listener_close from another thread wakes us via shutdown()).
 */
struct u_mcp_conn *
u_mcp_listener_accept(struct u_mcp_listener *listener);

/*!
 * Close the listener. Safe to call from another thread to unblock accept().
 */
void
u_mcp_listener_close(struct u_mcp_listener *listener);

/*!
 * Blocking read of exactly @p len bytes. Returns false on EOF / error.
 */
bool
u_mcp_conn_read(struct u_mcp_conn *conn, void *buf, size_t len);

/*!
 * Blocking write of exactly @p len bytes. Returns false on error.
 */
bool
u_mcp_conn_write(struct u_mcp_conn *conn, const void *buf, size_t len);

/*!
 * Close and free the connection.
 */
void
u_mcp_conn_close(struct u_mcp_conn *conn);

/*!
 * Connect to a per-PID listener as a client (used by the displayxr-mcp adapter).
 */
struct u_mcp_conn *
u_mcp_conn_connect(pid_t pid);

/*!
 * Raw fd / handle for poll()-style multiplexing by the adapter.
 * Returns -1 when unavailable (e.g. Windows named-pipe slice).
 */
int
u_mcp_conn_fd(struct u_mcp_conn *conn);

/*!
 * Enumerate running MCP sessions by scanning for socket files.
 * Fills @p out_pids (up to @p cap entries), returns count found.
 */
size_t
u_mcp_enumerate_sessions(pid_t *out_pids, size_t cap);

#ifdef __cplusplus
}
#endif
