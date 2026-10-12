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
#include "client/ipc_client_connection.h"

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#endif

bool
cli_service_pipe_present(void)
{
#ifdef XRT_OS_WINDOWS
	if (WaitNamedPipeA("\\.\pipe\displayxr\displayxr_comp_ipc", 1)) {
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
	if (!cli_service_pipe_present()) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_client_connection_init(ipc_c, log_level, ii);
}

#endif // CLI_HAVE_IPC
