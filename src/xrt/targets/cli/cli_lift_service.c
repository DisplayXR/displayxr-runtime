// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `lift_caps` for `info` / `selftest` from the RUNNING service
 *         (ADR-053 §7.1; replaces the retired headless WARP probe).
 *
 * The CLI never instantiates a second copy of a conversion module. When a
 * service is listening (checked first: a diagnostic never launches it, #1966)
 * one session-free DIAG `lift_get_properties` reads the caps the service's own
 * module reports, and @ref cli_query_set_lift_caps records them in the
 * existing `lift_caps` shape (exit 11 on a malformed answer, as before).
 *
 * TODO(#2003): once the service publishes the `dp-lift` conversion row in its
 * status snapshot, read that row (system_get_conv_*_row) instead of the v3
 * property call; the `lift_caps` JSON keeps its shape.
 *
 * @ingroup cli
 */

#include "cli_common.h"
#include "cli_query.h"

#ifdef CLI_HAVE_IPC
#include "cli_ipc.h"

#include "xrt/xrt_instance.h"
#include "xrt/xrt_results.h"
#include "util/u_logging.h"

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client_lift.h"
#endif

#include <stdio.h>
#include <string.h>

void
cli_query_lift_caps_from_service(struct cli_query_result *r)
{
#ifdef CLI_HAVE_IPC
	if (!cli_service_pipe_present()) {
		return; // headless: keep the static dp-lift note
	}
	struct xrt_instance_info ii;
	memset(&ii, 0, sizeof(ii));
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-cli");
	ii.app_info.declared_client_class = XRT_CLIENT_CLASS_DIAG;
	struct ipc_connection ipc_c;
	memset(&ipc_c, 0, sizeof(ipc_c));
	if (cli_ipc_connect(&ipc_c, U_LOGGING_ERROR, &ii) != XRT_SUCCESS) {
		return;
	}
	struct xrt_dp_lift_caps caps;
	memset(&caps, 0, sizeof(caps));
	const xrt_result_t xret = ipc_client_lift_get_properties(&ipc_c, &caps);
	ipc_client_connection_fini(&ipc_c);
	if (xret == XRT_SUCCESS) {
		cli_query_set_lift_caps(r, &caps, "running service, DIAG");
	}
#else
	(void)r;
#endif
}
