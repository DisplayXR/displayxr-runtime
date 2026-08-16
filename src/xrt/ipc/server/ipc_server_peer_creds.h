// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Server-derived peer identity for IPC clients (#954).
 *
 * The authorization gates historically compared against a PID the client sent
 * in `describe_client` — which the client fills with its own `getpid()` and can
 * change at will. This derives the connecting peer's PID from the OS at accept
 * time (the value the kernel attributes to the pipe/socket), so no client can
 * claim to be the workspace controller by lying about its PID.
 *
 * @ingroup ipc_server
 */

#pragma once

#include "xrt/xrt_handles.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Derive the connecting peer's process id from an accepted IPC handle.
 *
 * Windows: `GetNamedPipeClientProcessId`. Linux/Android: `SO_PEERCRED`.
 * macOS: `LOCAL_PEERPID`.
 *
 * @param handle              the just-accepted pipe handle / socket fd.
 * @param[out] out_create_ns  best-effort process creation time in ns since the
 *                            Unix epoch (Windows only; 0 elsewhere or when it
 *                            cannot be read — e.g. a low-integrity peer). Used
 *                            by the auth layer to defend against PID reuse.
 *                            May be NULL.
 * @return the peer PID, or 0 if it could not be derived (callers must treat 0
 *         as "unknown identity" and fail closed on privileged gates).
 */
long
ipc_server_derive_peer_pid(xrt_ipc_handle_t handle, uint64_t *out_create_ns);

#ifdef __cplusplus
}
#endif
