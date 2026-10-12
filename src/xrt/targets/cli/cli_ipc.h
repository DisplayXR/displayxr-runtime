// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The CLI's one way to reach the running service: never starts it.
 * @ingroup cli
 */

#pragma once

#ifdef CLI_HAVE_IPC

#include "xrt/xrt_results.h"
#include "util/u_logging.h"

#include <stdbool.h>

struct ipc_connection;
struct xrt_instance_info;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Is a service listening? On Windows the IPC client AUTO-LAUNCHES
 * displayxr-service when its pipe is missing, so every CLI connect checks this
 * first. A diagnostic must never start the service: the dashboard polls these
 * commands from inside a kill-on-close job, so a service the CLI launched
 * would die with the dashboard (#1966). POSIX dialing never spawns: always true.
 */
bool
cli_service_pipe_present(void);

/*!
 * ipc_client_connection_init() without the auto-launch: returns
 * XRT_ERROR_IPC_FAILURE (and leaves @p ipc_c untouched) when no service is
 * listening. Every CLI command that talks to the service connects through here.
 */
xrt_result_t
cli_ipc_connect(struct ipc_connection *ipc_c, enum u_logging_level log_level, const struct xrt_instance_info *ii);

#ifdef __cplusplus
}
#endif

#endif // CLI_HAVE_IPC
