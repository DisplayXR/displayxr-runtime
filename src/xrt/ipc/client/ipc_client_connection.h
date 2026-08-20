// Copyright 2020-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  More-internal client side code.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_client
 */

#pragma once

#include "xrt/xrt_results.h"
#include "xrt/xrt_config_os.h"
#include "ipc_client.h"


#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_ANDROID
/*!
 * Adopt an ALREADY-CONNECTED runtime service socket instead of dialling one.
 *
 * Android's normal connect is Java — `org.freedesktop.monado.ipc.Client`, loaded
 * out of the runtime apk, binds `MonadoService` over AIDL and makes the
 * socketpair. That needs a `Context`, cross-apk class loading and `<queries>`
 * package visibility, none of which suit an embedder whose rendering process is
 * not the process that owns its Java world (Chromium's GPU process, #1056).
 *
 * Such an embedder connects ONCE in the process that does have the Java world and
 * ships the fd down its own transport (Mojo `PlatformHandle`, a
 * `ParcelFileDescriptor`, `SCM_RIGHTS`, the child-launch descriptor table); the
 * receiving process calls this before `xrCreateInstance` and the Java path is
 * skipped entirely. `DXR_IPC_FD=<n>` in the environment does the same thing for a
 * process handed the fd in its descriptor table at launch.
 *
 * The fd is validated (open, connected, AF_UNIX, stream/seqpacket) and **duped**,
 * so the caller keeps ownership of what it passed. It is consumed by the next
 * `ipc_client_connection_init()`; a second call before that replaces the first.
 *
 * The process that CREATED the socketpair remains the one `SO_PEERCRED` reports
 * to the server, so an adopted connection is attributed to the creator (#1056).
 *
 * Exported from the runtime library so an embedder can `dlsym` it.
 *
 * @param fd A connected socket end. Not consumed.
 *
 * @ingroup ipc_client
 */
void
ipc_client_connection_adopt_fd(int fd);

/*!
 * Has this process been handed a service socket — either through
 * @ref ipc_client_connection_adopt_fd or `DXR_IPC_FD`?
 *
 * A peek, not a take: the fd stays available for the connection that follows.
 * #1031 hybrid mode uses it at `xrt_instance_create` to route an fd-adopting
 * client onto the IPC path, since adopting a connected service socket only
 * means anything there. Safe to call before any connection exists.
 *
 * @ingroup ipc_client
 */
bool
ipc_client_connection_has_adopted_fd(void);
#endif // XRT_OS_ANDROID

#ifdef XRT_OS_WINDOWS
/*!
 * Adopt an ALREADY-CONNECTED service pipe endpoint instead of dialling one.
 *
 * The Windows twin of @ref ipc_client_connection_adopt_fd (#1056), with the same
 * contract, and for the same reason: a sandboxed embedder process that cannot
 * reach the transport itself. Chromium's GPU process runs a `USER_LIMITED`
 * restricted token that matches neither ACE on the service pipe's security
 * descriptor, so `CreateFileA` on the pipe returns ACCESS_DENIED (browser#103
 * experiment E0). Its unsandboxed browser process opens the pipe for it — with
 * @ref ipc_client_connection_export — and ships the raw HANDLE down its own
 * transport (mojo duplicates it into the target process).
 *
 * The handle is validated (a named pipe, and the CLIENT end of one) and
 * **duplicated**, so the caller keeps ownership of what it passed. It is
 * consumed by the next `ipc_client_connection_init()`; a second call before that
 * replaces the first. `DXR_IPC_HANDLE=<decimal>` in the environment does the
 * same thing for a process handed the HANDLE at launch.
 *
 * NOT a security boundary: a handle is a capability, and whoever can call this
 * is already inside the process.
 *
 * Unlike the POSIX case, mis-attribution matters here: the server duplicates
 * shared memory, the woven texture and the fence into whatever process
 * `GetNamedPipeClientProcessId` names, i.e. the OPENER. An adopted connection
 * therefore DECLARES its own pid as the duplication target during
 * `ipc_client_connection_init` (browser#103 RC-1), before any handle crosses;
 * the server accepts the declaration only when the opener's integrity level is
 * at least the declared target's.
 *
 * Exported from the runtime library so an embedder can `GetProcAddress` it.
 *
 * @param pipe_handle A connected pipe client end (a Win32 `HANDLE`; typed
 * `void *` so this header does not drag in windows.h). Not consumed.
 *
 * @ingroup ipc_client
 */
void
ipc_client_connection_adopt_handle(void *pipe_handle);

/*!
 * Has this process been handed a service pipe endpoint — either through
 * @ref ipc_client_connection_adopt_handle or `DXR_IPC_HANDLE`?
 *
 * A peek, not a take. Twin of @ref ipc_client_connection_has_adopted_fd.
 *
 * @ingroup ipc_client
 */
bool
ipc_client_connection_has_adopted_handle(void);
#endif // XRT_OS_WINDOWS

/*!
 * Open a FRESH, CONNECTED, **UN-HANDSHAKEN** service endpoint (browser#103 RC-4).
 *
 * This is the broker half of the adopt mechanism above: a process that CAN reach
 * the transport opens an endpoint on behalf of one that cannot, and ships it
 * across. Windows hands back a pipe `HANDLE`; POSIX hands back a socket fd.
 *
 * It performs the connect and NOTHING ELSE. `ipc_client_connection_init` is
 * connect -> `ipc_client_setup_shm` -> `ipc_client_check_git_tag` ->
 * `ipc_client_describe_client`, and all three of the latter MUST run in the
 * ADOPTING process: the shmem handle has to be duplicated into the adopter's
 * address space, and the peer identity settled has to be the adopter's.
 *
 * The caller's own connection is untouched — this opens a new pipe instance /
 * socket. The returned handle is owned by the caller.
 *
 * Not implemented on Android, where the connect is Java-side and needs a
 * `Context` the runtime only sees at `xrCreateInstance`; an Android embedder
 * brokers with the shipped Java connect + `DXR_IPC_FD` instead (#1056).
 *
 * @param log_level  Log level for messages emitted while connecting.
 * @param[out] out_handle Receives the connected endpoint.
 * @return XRT_SUCCESS on success.
 *
 * @ingroup ipc_client
 */
xrt_result_t
ipc_client_connection_export(enum u_logging_level log_level, xrt_ipc_handle_t *out_handle);

/*!
 * Set up the basics of the client connection: socket and shared mem
 * @param ipc_c     Empty IPC connection struct
 * @param log_level Log level for IPC messages
 * @param i_info    Instance info to send to server
 * @return XRT_SUCCESS on success
 *
 * @ingroup ipc_client
 */
xrt_result_t
ipc_client_connection_init(struct ipc_connection *ipc_c,
                           enum u_logging_level log_level,
                           const struct xrt_instance_info *i_info);

/*!
 * Locks the connection, allows sending complex messages.
 *
 * @param ipc_c The IPC connection to lock.
 *
 * @ingroup ipc_client
 */
static inline void
ipc_client_connection_lock(struct ipc_connection *ipc_c)
{
	os_mutex_lock(&ipc_c->mutex);
}

/*!
 * Unlocks the connection.
 *
 * @param ipc_c A locked IPC connection to unlock.
 *
 * @ingroup ipc_client
 */
static inline void
ipc_client_connection_unlock(struct ipc_connection *ipc_c)
{
	os_mutex_unlock(&ipc_c->mutex);
}

/*!
 * Tear down the basics of the client connection: socket and shared mem
 * @param ipc_c initialized IPC connection struct
 *
 * @ingroup ipc_client
 */
void
ipc_client_connection_fini(struct ipc_connection *ipc_c);

#ifdef __cplusplus
} // extern "C"
#endif
