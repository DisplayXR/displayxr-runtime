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
#include <stddef.h>
#include <stdbool.h>

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

/*!
 * #960: best-effort absolute executable path of a peer process, for class
 * verification (CONTROLLER = a registered controller binary; DIAG = a binary in
 * the runtime's own directory).
 *
 * Windows: `QueryFullProcessImageNameW` (needs PROCESS_QUERY_LIMITED_INFORMATION —
 * fails against a Low-integrity/restricted-token peer such as the browser GPU
 * process, which is fine: those never claim a path-verified class). Linux:
 * `/proc/<pid>/exe`. macOS: `proc_pidpath`.
 *
 * @return true and a NUL-terminated UTF-8 path in @p out_path on success; false
 *         (out_path[0] == 0) when it cannot be derived.
 */
bool
ipc_server_peer_exe_path(long pid, char *out_path, size_t out_len);

#ifdef __cplusplus
}
#endif
