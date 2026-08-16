// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service-side verification of declared IPC client classes (#960).
 *
 * The IPC server verifies most class claims itself (orchestrator pid, verify-by-
 * use for RELAY / PRESENT_OWNER). Two claims need facts only the service target
 * owns, so `ipc_server` calls back here through
 * `ipc_server_set_client_class_verify_provider`:
 *   - CONTROLLER: the peer executable is a registered workspace-controller binary
 *     (`HKLM\Software\DisplayXR\WorkspaceControllers\<id>\Binary` on Windows, the
 *     JSON manifests on POSIX) or the orchestrator's selected entry (which covers
 *     the `workspace_binary` dev override).
 *   - DIAG: the peer executable lives in the service's own directory (displayxr-cli,
 *     the WebXR bridge's introspection connection).
 *
 * @ingroup ipc_server
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * `ipc_server_client_class_verify_fn` implementation. @p peer_exe_path may be
 * "" (the OS refused — Low-IL peer); then nothing path-based verifies.
 */
bool
service_client_class_verify(long peer_pid, const char *peer_exe_path, uint32_t declared_class);

#ifdef __cplusplus
}
#endif
