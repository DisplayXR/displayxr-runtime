// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Child-process helpers the service's spawners share: the workspace
 *         orchestrator (service_orchestrator.c) and the conversion-host
 *         supervisor (service_conversion.c, ADR-053 P2, roadmap §3.4).
 *
 * What is shared is the mechanics only: where a sibling executable lives, how
 * a child is started (no console window, optional log redirection, optionally
 * suspended so the caller can put it in a job object before it runs) and how
 * it is ended. Policy stays with each spawner: the orchestrator watches and
 * respawns the workspace controller; the conversion supervisor owns its hosts'
 * job object, backoff and crash-loop accounting. Conversion hosts are NOT
 * workspace controllers and never reach the orchestrator's watchdog.
 *
 * @ingroup ipc
 */

#pragma once

#include "xrt/xrt_config_os.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef XRT_OS_WINDOWS
#include "xrt/xrt_windows.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

//! Platform suffix of an executable ("" on POSIX).
#ifdef XRT_OS_WINDOWS
#define SERVICE_SPAWN_EXE_SUFFIX ".exe"
#else
#define SERVICE_SPAWN_EXE_SUFFIX ""
#endif

/*!
 * The path of @p exe_name in the directory the running service executable
 * lives in (the installed layouts keep every DisplayXR binary side by side:
 * `C:\Program Files\DisplayXR\Runtime\`, `/usr/lib/displayxr/bin/`,
 * `<bundle>/bin/`). @p exe_name carries its own suffix. Does not check that
 * the file exists (service_spawn_file_exists).
 */
bool
service_spawn_sibling_path(const char *exe_name, char *buf, size_t buf_size);

//! A regular file exists at @p path (directories excluded).
bool
service_spawn_file_exists(const char *path);

#ifdef XRT_OS_WINDOWS

/*! @name service_spawn_child flags @{ */
//! CREATE_SUSPENDED: the caller assigns a job object, then resumes the
//! thread (pi->hThread is kept open for that, and closed by the caller).
#define SERVICE_SPAWN_SUSPENDED (1u << 0)
/*! @} */

/*!
 * Start `"exe_path" args` with no console window. With @p log_path, the
 * child's stdout and stderr append to that file. The process handle is
 * returned in @p pi; the thread handle is closed unless
 * SERVICE_SPAWN_SUSPENDED. The child inherits the service's token (medium
 * integrity, never elevated) and environment.
 */
bool
service_spawn_child(const char *exe_path, const char *args, const char *log_path, uint32_t flags, PROCESS_INFORMATION *pi);

//! `%LOCALAPPDATA%\DisplayXR\<basename>.log`.
bool
service_spawn_appdata_log_path(const char *basename, char *buf, size_t buf_size);

/*!
 * TerminateProcess(@p exit_code), wait up to @p wait_ms, close the handles
 * and clear @p pi. No-op on an empty @p pi.
 */
void
service_spawn_terminate(PROCESS_INFORMATION *pi, uint32_t exit_code, uint32_t wait_ms);

#endif // XRT_OS_WINDOWS

#ifdef __cplusplus
}
#endif
