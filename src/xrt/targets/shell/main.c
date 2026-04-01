// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR spatial shell — launches service, monitors apps.
 *
 * This is a management client that:
 * 1. Starts displayxr-service --shell if not already running
 * 2. Connects via IPC to monitor connected apps
 * 3. Polls for client connect/disconnect events
 * 4. Can issue shell_set_window_pose for programmatic layout
 *
 * The shell does NOT create an OpenXR session — it's purely a
 * management/launcher tool. Interactive window drag is handled
 * server-side in the multi-compositor render loop.
 *
 * @ingroup ipc
 */

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"

#include "ipc_client_generated.h"
#include "shared/ipc_protocol.h"
#include "xrt/xrt_results.h"
#include "util/u_logging.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <stdlib.h>
#endif


#define P(...) fprintf(stdout, __VA_ARGS__)
#define PE(...) fprintf(stderr, __VA_ARGS__)

static volatile int g_running = 1;

static void
signal_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

#ifdef _WIN32
/*!
 * Try to launch displayxr-service --shell as a detached process.
 * Returns true if launched (or already running).
 */
static bool
try_launch_service(void)
{
	// Find service executable next to this shell executable
	char exe_path[MAX_PATH];
	GetModuleFileNameA(NULL, exe_path, MAX_PATH);

	// Replace "displayxr-shell.exe" with "displayxr-service.exe"
	char *last_sep = strrchr(exe_path, '\\');
	if (last_sep == NULL) {
		last_sep = strrchr(exe_path, '/');
	}
	if (last_sep != NULL) {
		strcpy(last_sep + 1, "displayxr-service.exe");
	} else {
		strcpy(exe_path, "displayxr-service.exe");
	}

	// Build command line
	char cmd[MAX_PATH + 32];
	snprintf(cmd, sizeof(cmd), "\"%s\" --shell", exe_path);

	STARTUPINFOA si = {0};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {0};

	BOOL ok = CreateProcessA(
	    NULL,                        // lpApplicationName
	    cmd,                         // lpCommandLine
	    NULL,                        // lpProcessAttributes
	    NULL,                        // lpThreadAttributes
	    FALSE,                       // bInheritHandles
	    DETACHED_PROCESS |           // Don't share console
	        CREATE_NEW_PROCESS_GROUP, // New process group
	    NULL,                        // lpEnvironment
	    NULL,                        // lpCurrentDirectory
	    &si,                         // lpStartupInfo
	    &pi);                        // lpProcessInformation

	if (ok) {
		P("Launched displayxr-service --shell (PID %lu)\n", pi.dwProcessId);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return true;
	} else {
		PE("Failed to launch service: error %lu\n", GetLastError());
		PE("Tried: %s\n", cmd);
		return false;
	}
}
#endif

static void
print_clients(struct ipc_connection *ipc_c, uint32_t *prev_ids, uint32_t *prev_count)
{
	struct ipc_client_list clients;
	xrt_result_t r = ipc_call_system_get_clients(ipc_c, &clients);
	if (r != XRT_SUCCESS) {
		return;
	}

	// Detect changes
	bool changed = (clients.id_count != *prev_count);
	if (!changed) {
		for (uint32_t i = 0; i < clients.id_count; i++) {
			if (clients.ids[i] != prev_ids[i]) {
				changed = true;
				break;
			}
		}
	}

	if (!changed) {
		return;
	}

	// Print current state
	P("\n--- %u client(s) connected ---\n", clients.id_count);
	for (uint32_t i = 0; i < clients.id_count; i++) {
		uint32_t id = clients.ids[i];
		struct ipc_app_state cs;
		r = ipc_call_system_get_client_info(ipc_c, id, &cs);
		if (r != XRT_SUCCESS) {
			P("  [%u] (failed to get info)\n", id);
			continue;
		}
		P("  [%u] %s (PID %d)\n", id, cs.info.application_name, cs.pid);
	}

	// Update previous state
	*prev_count = clients.id_count;
	for (uint32_t i = 0; i < clients.id_count; i++) {
		prev_ids[i] = clients.ids[i];
	}
}

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	P("DisplayXR Shell\n");
	P("Connecting to service...\n");

	// Try to connect to an already-running service
	struct ipc_connection ipc_c = {0};
	struct xrt_instance_info info = {0};
	snprintf(info.app_info.application_name,
	         sizeof(info.app_info.application_name),
	         "displayxr-shell");

	xrt_result_t xret = ipc_client_connection_init(&ipc_c, U_LOGGING_WARN, &info);

	if (xret != XRT_SUCCESS) {
#ifdef _WIN32
		P("Service not running. Launching...\n");
		if (!try_launch_service()) {
			PE("Could not start service. Exiting.\n");
			return 1;
		}

		// Wait for service to start, retry connection
		for (int attempt = 0; attempt < 10; attempt++) {
			Sleep(500);
			xret = ipc_client_connection_init(&ipc_c, U_LOGGING_WARN, &info);
			if (xret == XRT_SUCCESS) {
				break;
			}
		}

		if (xret != XRT_SUCCESS) {
			PE("Failed to connect after launching service.\n");
			return 1;
		}
#else
		PE("Failed to connect to service. Is displayxr-service --shell running?\n");
		return 1;
#endif
	}

	P("Connected to service.\n");
	P("Monitoring clients (Ctrl+C to exit)...\n");

	uint32_t prev_ids[IPC_MAX_CLIENTS] = {0};
	uint32_t prev_count = 0;

	// Poll loop
	while (g_running) {
		print_clients(&ipc_c, prev_ids, &prev_count);

#ifdef _WIN32
		Sleep(500);
#else
		usleep(500000);
#endif
	}

	P("\nShell exiting.\n");
	ipc_client_connection_fini(&ipc_c);

	return 0;
}
