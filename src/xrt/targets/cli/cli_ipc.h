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
#include <stdint.h>

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

//! What @ref cli_ipc_poke_component_reselect got back.
enum cli_ipc_poke_result
{
	CLI_IPC_POKE_UNREACHABLE = 0, //!< no service listening (never started here)
	CLI_IPC_POKE_ACCEPTED,        //!< the service re-selects now
	CLI_IPC_POKE_NO_WATCH,        //!< a service without the watch (XRT_ERROR_NOT_IMPLEMENTED)
	CLI_IPC_POKE_REFUSED,         //!< any other error; see @p out_xret
};

/*!
 * ADR-054 N3d (#2051): after a `dp|input|conv use|reset|enable|disable`
 * write, ask the running service to re-read that class's policy and re-select
 * now (DIAG RPC `system_request_component_reselect`). @p class_id is a
 * `TARGET_COMPONENT_CLASS_*` (target_component_loader.h). One short DIAG
 * connection; never starts the service. @p out_xret may be NULL.
 */
enum cli_ipc_poke_result
cli_ipc_poke_component_reselect(uint32_t class_id, xrt_result_t *out_xret);

#ifdef __cplusplus
}
#endif

#endif // CLI_HAVE_IPC
