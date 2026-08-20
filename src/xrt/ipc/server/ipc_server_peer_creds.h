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

/*!
 * browser#103 RC-1: **the enforcement point for the IL-monotonicity rule.**
 *
 * May the process that OPENED this connection hand it over to @p declared_pid?
 *
 * The rule, and the only thing that makes a declared duplication target safe:
 *
 * > The pipe opener is the AUTHORISER; the declared target is the PEER. A
 * > declaration is accepted only when the opener's integrity level is **at
 * > least** the declared target's.
 *
 * So a Medium-integrity browser may delegate to its own Low-integrity sandboxed
 * child — the case this exists for — and a Low-integrity process can never
 * escalate a connection into a Medium/High one. Declaring yourself is trivially
 * allowed (equal levels), which is the normal case: an adopting client always
 * declares its own pid, since that is unconditionally the process the service
 * must duplicate into, whoever happened to open the pipe.
 *
 * Fails CLOSED. Anything unreadable — a pid that cannot be opened even for
 * PROCESS_QUERY_LIMITED_INFORMATION, a token that will not yield its integrity
 * level, an unknown pid — refuses the declaration and leaves opener attribution
 * in place.
 *
 * Windows-only in substance. On POSIX there is no handle duplication and no
 * integrity level, so a declaration buys nothing and only a self-declaration
 * (declared == opener) is accepted; the OS-derived creds stay authoritative.
 *
 * @param opener_pid    the OS-derived pid of the process that opened the
 *                      connection (`ipc_server_derive_peer_pid`).
 * @param declared_pid  the pid the client declared.
 * @param[out] out_why  NULL, or a static string naming the reason on refusal.
 * @return true if the declaration may be honoured.
 */
bool
ipc_server_peer_declaration_allowed(long opener_pid, long declared_pid, const char **out_why);

#ifdef __cplusplus
}
#endif
