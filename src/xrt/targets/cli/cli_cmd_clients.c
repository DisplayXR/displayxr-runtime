// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli clients` — list the live IPC clients of the running
 *         service with their verified class (#960 / #951 follow-up).
 *
 * Unlike every other verb, this one connects to the service over IPC (as a DIAG
 * client, verified by the service through the exe path — the CLI must live in
 * the runtime install dir). It therefore appears in its own listing, marked.
 *
 * Windows caveat: the service duplicates the shm handle into the client with
 * PROCESS_DUP_HANDLE, which fails against an ELEVATED client — run this from a
 * non-elevated prompt (or `schtasks /RL LIMITED`) or it reports "not connected".
 */

#include "cli_common.h"

#ifdef CLI_HAVE_IPC

#include "xrt/xrt_instance.h"
#include "xrt/xrt_results.h"
#include "util/u_logging.h"

#include "client/ipc_client_connection.h"
#include "client/ipc_client.h"
#include "ipc_client_generated.h"

#include <stdio.h>
#include <string.h>

static const char *
class_str(uint32_t c)
{
	switch (c) {
	case XRT_CLIENT_CLASS_APP: return "APP";
	case XRT_CLIENT_CLASS_CONTROLLER: return "CONTROLLER";
	case XRT_CLIENT_CLASS_PRESENT_OWNER: return "PRESENT_OWNER";
	case XRT_CLIENT_CLASS_RELAY: return "RELAY";
	case XRT_CLIENT_CLASS_PROVIDER_HOST: return "PROVIDER_HOST";
	case XRT_CLIENT_CLASS_DIAG: return "DIAG";
	default: return "?";
	}
}

int
cli_cmd_clients(int argc, const char **argv)
{
	bool json = false;
	uint32_t declare = XRT_CLIENT_CLASS_DIAG;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0) {
			json = true;
		} else if (strcmp(argv[i], "--declare") == 0 && i + 1 < argc) {
			// Dev knob: declare another class to exercise the service's
			// verification / quota path (e.g. `--declare CONTROLLER` is demoted
			// to APP unless DXR_ALLOW_UNVERIFIED_CONTROLLER=1 is set on the
			// service, in which case a second CONTROLLER is refused at the quota).
			const char *name = argv[++i];
			for (uint32_t c = 0; c < XRT_CLIENT_CLASS_COUNT; c++) {
				if (strcmp(class_str(c), name) == 0) {
					declare = c;
				}
			}
		}
	}

	struct xrt_instance_info ii = {0};
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-cli");
	ii.app_info.declared_client_class = declare;

	struct ipc_connection ipc_c = {0};
	xrt_result_t xret = ipc_client_connection_init(&ipc_c, U_LOGGING_ERROR, &ii);
	if (xret != XRT_SUCCESS) {
		if (json) {
			printf("{\"connected\": false, \"xrt_result\": %d}\n", (int)xret);
		} else {
			printf("displayxr-cli clients: not connected to the service (xrt_result=%d%s).\n", (int)xret,
			       xret == XRT_ERROR_CLIENT_LIMIT_REACHED ? " = CLIENT_LIMIT_REACHED, class quota exhausted"
			                                              : "");
			printf("  Is displayxr-service running? On Windows, run from a NON-elevated prompt.\n");
		}
		return 2;
	}

	bool ws_active = false;
	(void)ipc_call_workspace_get_state(&ipc_c, &ws_active);

	struct ipc_client_list list = {0};
	xret = ipc_call_system_get_clients(&ipc_c, &list);
	if (xret != XRT_SUCCESS) {
		printf("system_get_clients failed: %d\n", (int)xret);
		ipc_client_connection_fini(&ipc_c);
		return 2;
	}

	if (json) {
		printf("{\"connected\": true, \"workspace_active\": %s, \"clients\": [", ws_active ? "true" : "false");
	} else {
		printf("service: connected, workspace_mode=%s, clients=%u\n", ws_active ? "on" : "off", list.id_count);
		printf("%-5s %-7s %-14s %-32s %-8s %-3s\n", "id", "pid", "class", "name", "session", "io");
	}

	uint32_t printed = 0;
	for (uint32_t i = 0; i < list.id_count; i++) {
		struct ipc_app_state ias = {0};
		if (ipc_call_system_get_client_info(&ipc_c, list.ids[i], &ias) != XRT_SUCCESS) {
			continue;
		}
		bool self = strcmp(ias.info.application_name, "displayxr-cli") == 0 &&
		            ias.client_class == XRT_CLIENT_CLASS_DIAG;
		if (json) {
			printf(
			    "%s{\"id\": %u, \"pid\": %d, \"class\": \"%s\", \"name\": \"%s\", \"active\": %s, "
			    "\"visible\": %s, \"focused\": %s, \"primary\": %s, \"io_active\": %s}",
			    printed ? ", " : "", ias.id, (int)ias.pid, class_str(ias.client_class),
			    ias.info.application_name, ias.session_active ? "true" : "false",
			    ias.session_visible ? "true" : "false", ias.session_focused ? "true" : "false",
			    ias.primary_application ? "true" : "false", ias.io_active ? "true" : "false");
		} else {
			printf("%-5u %-7d %-14s %-32.32s %c%c%c      %c%s\n", ias.id, (int)ias.pid,
			       class_str(ias.client_class), ias.info.application_name, ias.session_active ? 'a' : '-',
			       ias.session_visible ? 'v' : '-', ias.session_focused ? 'f' : '-',
			       ias.io_active ? 'y' : 'n',
			       self                      ? "  (self)"
			       : ias.primary_application ? "  PRIMARY"
			                                 : "");
		}
		printed++;
	}
	if (json) {
		printf("]}\n");
	} else {
		printf("(this listing includes displayxr-cli's own DIAG connection)\n");
	}

	ipc_client_connection_fini(&ipc_c);
	return 0;
}

#else // !CLI_HAVE_IPC

#include <stdio.h>

int
cli_cmd_clients(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	printf("displayxr-cli clients: this build has no IPC client (XRT_FEATURE_SERVICE off).\n");
	return 2;
}

#endif
