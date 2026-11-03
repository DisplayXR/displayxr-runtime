// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The CLI's one way to reach the running service: never starts it.
 * @ingroup cli
 */

#include "cli_ipc.h"

#ifdef CLI_HAVE_IPC

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_instance.h"
#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "ipc_client_generated.h"

#include <stdio.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#endif

#ifdef XRT_OS_LINUX_DESKTOP
#include "util/u_debug.h"

/*
 * #2515: a service that wedged on its way out (or anywhere else) can leave the
 * activated socket accepting connections nobody answers; the handshake then
 * blocked forever and the CLI just hung. Bound it (0 = wait forever, as before).
 */
DEBUG_GET_ONCE_NUM_OPTION(cli_ipc_timeout_ms, "DXR_CLI_IPC_TIMEOUT_MS", 5000)
#endif

bool
cli_service_pipe_present(void)
{
#ifdef XRT_OS_WINDOWS
	if (WaitNamedPipeA("\\\\.\\pipe\\displayxr\\displayxr_comp_ipc", 1)) {
		return true;
	}
	return GetLastError() != ERROR_FILE_NOT_FOUND; // busy / timeout = it exists
#else
	return true;
#endif
}

xrt_result_t
cli_ipc_connect(struct ipc_connection *ipc_c, enum u_logging_level log_level, const struct xrt_instance_info *ii)
{
	// The pre-check keeps a missing service quiet (no "Connect ... failed" ERROR
	// on every reconnect poll); the flag is what makes it race-free: the
	// service can die between this check and the connect below.
	ipc_client_connection_disable_service_launch();
	if (!cli_service_pipe_present()) {
		return XRT_ERROR_IPC_FAILURE;
	}
#ifdef XRT_OS_LINUX_DESKTOP
	const int64_t timeout_ms = debug_get_num_option_cli_ipc_timeout_ms();
	ipc_client_connection_set_handshake_timeout_ms(timeout_ms > 0 ? (uint32_t)timeout_ms : 0);
#endif
	const xrt_result_t xret = ipc_client_connection_init(ipc_c, log_level, ii);
#ifdef XRT_OS_LINUX_DESKTOP
	if (xret != XRT_SUCCESS && ipc_client_connection_handshake_timed_out()) {
		// Once per process: `status --watch` reconnects in a loop.
		static bool said = false;
		if (!said) {
			said = true;
			fprintf(stderr,
			        "displayxr-cli: displayxr-service accepted the connection but did not answer the IPC "
			        "handshake within %lld ms — the service is wedged (#2515).\n"
			        "  Recover with: systemctl --user restart displayxr.service\n"
			        "  (DXR_CLI_IPC_TIMEOUT_MS=<ms> changes this bound; 0 waits forever.)\n",
			        (long long)timeout_ms);
		}
	}
#endif
	return xret;
}

bool
cli_ipc_service_wedged(void)
{
	return ipc_client_connection_handshake_timed_out();
}

enum cli_ipc_poke_result
cli_ipc_poke_component_reselect(uint32_t class_id, xrt_result_t *out_xret)
{
	struct xrt_instance_info ii;
	memset(&ii, 0, sizeof(ii));
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-cli");
	ii.app_info.declared_client_class = XRT_CLIENT_CLASS_DIAG;
	struct ipc_connection ipc_c;
	memset(&ipc_c, 0, sizeof(ipc_c));
	if (cli_ipc_connect(&ipc_c, U_LOGGING_RAW, &ii) != XRT_SUCCESS) {
		return CLI_IPC_POKE_UNREACHABLE;
	}
	const xrt_result_t xret = ipc_call_system_request_component_reselect(&ipc_c, class_id);
	ipc_client_connection_fini(&ipc_c);
	if (out_xret != NULL) {
		*out_xret = xret;
	}
	if (xret == XRT_SUCCESS) {
		return CLI_IPC_POKE_ACCEPTED;
	}
	return xret == XRT_ERROR_NOT_IMPLEMENTED ? CLI_IPC_POKE_NO_WATCH : CLI_IPC_POKE_REFUSED;
}

#endif // CLI_HAVE_IPC
