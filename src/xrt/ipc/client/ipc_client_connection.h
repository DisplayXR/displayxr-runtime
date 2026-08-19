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
#endif // XRT_OS_ANDROID

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
