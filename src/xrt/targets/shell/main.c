// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR spatial shell — launches service, launches apps, monitors.
 *
 * Usage:
 *   displayxr-shell.exe [--pose x,y,z,w,h] app1.exe [--pose x,y,z,w,h] app2.exe ...
 *
 * - Auto-starts displayxr-service --shell if not running
 * - Launches each app with DISPLAYXR_SHELL_SESSION=1 and XR_RUNTIME_JSON set
 * - Optionally assigns per-app window pose via --pose x,y,z,width_m,height_m
 * - Monitors client connect/disconnect until Ctrl+C
 *
 * @ingroup ipc
 */

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"

#include "ipc_client_generated.h"
#include "shared/ipc_protocol.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_defines.h"
#include "util/u_logging.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif


#define P(...) fprintf(stdout, __VA_ARGS__)
#define PE(...) fprintf(stderr, __VA_ARGS__)

#define MAX_APPS 8

static volatile int g_running = 1;

static void
signal_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

struct app_entry
{
	const char *exe_path;
	bool has_pose;
	float px, py, pz;       // position in meters from display center
	float width_m, height_m; // window physical size in meters
#ifdef _WIN32
	HANDLE process;
	DWORD pid;
#endif
	bool pose_applied;
};

#ifdef _WIN32
/*!
 * Get the directory containing the shell executable.
 * Returns path with trailing backslash, e.g. "C:\..._package\bin\"
 */
static void
get_exe_dir(char *buf, size_t buf_size)
{
	GetModuleFileNameA(NULL, buf, (DWORD)buf_size);
	char *last_sep = strrchr(buf, '\\');
	if (last_sep == NULL) {
		last_sep = strrchr(buf, '/');
	}
	if (last_sep != NULL) {
		last_sep[1] = '\0';
	}
}



/*!
 * Resolve the XR_RUNTIME_JSON manifest path.
 * Tries (in order):
 * 1. _package/DisplayXR_win64.json (installed manifest, relative to bin/)
 * 2. build/Release/openxr_displayxr-dev.json (dev build, relative to CWD)
 */
static bool
get_runtime_json_path(char *buf, size_t buf_size)
{
	// Try dev build manifest relative to CWD first (correct DLL path)
	char cwd[MAX_PATH];
	GetCurrentDirectoryA(MAX_PATH, cwd);
	snprintf(buf, buf_size, "%s\\build\\Release\\openxr_displayxr-dev.json", cwd);
	if (GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES) {
		return true;
	}

	// Try installed manifest: _package/DisplayXR_win64.json
	char exe_dir[MAX_PATH];
	get_exe_dir(exe_dir, sizeof(exe_dir));

	// exe_dir = "..._package/bin/" → go up to "_package/"
	size_t len = strlen(exe_dir);
	if (len >= 4) {
		char *tail = exe_dir + len - 4;
		if ((tail[0] == 'b' || tail[0] == 'B') &&
		    (tail[1] == 'i' || tail[1] == 'I') &&
		    (tail[2] == 'n' || tail[2] == 'N') &&
		    (tail[3] == '\\' || tail[3] == '/')) {
			tail[0] = '\0';
		}
	}

	snprintf(buf, buf_size, "%sDisplayXR_win64.json", exe_dir);
	if (GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES) {
		return true;
	}

	PE("Warning: no runtime manifest found, apps may fail to connect\n");
	return false;
}

/*!
 * Launch an app with DISPLAYXR_SHELL_SESSION=1 and XR_RUNTIME_JSON set.
 */
static bool
launch_app(struct app_entry *app, const char *runtime_json)
{
	// Build environment block: inherit current env + add our vars
	// Use SetEnvironmentVariable before CreateProcess with NULL env
	// (simpler than building a full env block)
	if (runtime_json != NULL) {
		SetEnvironmentVariableA("XR_RUNTIME_JSON", runtime_json);
	}
	SetEnvironmentVariableA("DISPLAYXR_SHELL_SESSION", "1");

	// Resolve to absolute path (relative paths fail with CreateProcessA)
	char abs_path[MAX_PATH];
	if (_fullpath(abs_path, app->exe_path, MAX_PATH) == NULL) {
		PE("Failed to resolve path: %s\n", app->exe_path);
		return false;
	}

	// Quote the exe path in case of spaces
	char cmd[MAX_PATH + 16];
	snprintf(cmd, sizeof(cmd), "\"%s\"", abs_path);

	STARTUPINFOA si = {0};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {0};

	BOOL ok = CreateProcessA(
	    NULL, cmd, NULL, NULL, FALSE,
	    CREATE_NEW_CONSOLE,  // Each app gets its own console
	    NULL,                // Inherit our (modified) environment
	    NULL, &si, &pi);

	// Clean up env vars so they don't leak to future CreateProcess calls
	// (though it doesn't matter since we set them every time)

	if (ok) {
		app->process = pi.hProcess;
		app->pid = pi.dwProcessId;
		P("Launched: %s (PID %lu)\n", app->exe_path, pi.dwProcessId);
		CloseHandle(pi.hThread);
		return true;
	} else {
		PE("Failed to launch %s: error %lu\n", app->exe_path, GetLastError());
		return false;
	}
}
#endif // _WIN32

static void
print_usage(void)
{
	P("Usage: displayxr-shell [--pose x,y,z,w,h] app1.exe [--pose x,y,z,w,h] app2.exe ...\n");
	P("\n");
	P("Options:\n");
	P("  --pose x,y,z,w,h   Set window pose for the next app argument\n");
	P("                      x,y,z = position (meters from display center)\n");
	P("                      w,h = window width and height (meters)\n");
	P("  --help              Show this help\n");
	P("\n");
	P("If no apps are specified, runs in monitor-only mode.\n");
	P("The service (displayxr-service --shell) is auto-started if needed.\n");
}

static int
parse_args(int argc, char *argv[], struct app_entry *apps, int *app_count)
{
	*app_count = 0;
	bool next_has_pose = false;
	float px = 0, py = 0, pz = 0, pw = 0, ph = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_usage();
			return -1;
		}
		if (strcmp(argv[i], "--pose") == 0) {
			if (i + 1 >= argc) {
				PE("Error: --pose requires argument x,y,z,w,h\n");
				return -1;
			}
			i++;
			int n = sscanf(argv[i], "%f,%f,%f,%f,%f", &px, &py, &pz, &pw, &ph);
			if (n < 5) {
				PE("Error: --pose needs 5 comma-separated values (x,y,z,w,h), got %d\n", n);
				return -1;
			}
			next_has_pose = true;
			continue;
		}
		if (strcmp(argv[i], "--") == 0) {
			continue;
		}

		// This is an app path
		if (*app_count >= MAX_APPS) {
			PE("Warning: max %d apps, ignoring %s\n", MAX_APPS, argv[i]);
			continue;
		}
		struct app_entry *a = &apps[*app_count];
		memset(a, 0, sizeof(*a));
		a->exe_path = argv[i];
		if (next_has_pose) {
			a->has_pose = true;
			a->px = px;
			a->py = py;
			a->pz = pz;
			a->width_m = pw;
			a->height_m = ph;
			next_has_pose = false;
		}
		(*app_count)++;
	}
	return 0;
}

static void
try_apply_poses(struct ipc_connection *ipc_c, struct app_entry *apps, int app_count,
                uint32_t *prev_ids, uint32_t prev_count)
{
	// Get current client list
	struct ipc_client_list clients;
	xrt_result_t r = ipc_call_system_get_clients(ipc_c, &clients);
	if (r != XRT_SUCCESS) {
		return;
	}

	// Find new clients (IDs not in prev_ids)
	for (uint32_t i = 0; i < clients.id_count; i++) {
		uint32_t id = clients.ids[i];
		bool is_new = true;
		for (uint32_t j = 0; j < prev_count; j++) {
			if (prev_ids[j] == id) {
				is_new = false;
				break;
			}
		}
		if (!is_new) {
			continue;
		}

		// Find the first app that hasn't had its pose applied yet
		for (int a = 0; a < app_count; a++) {
			if (apps[a].has_pose && !apps[a].pose_applied) {
				struct xrt_pose pose;
				pose.orientation.x = 0;
				pose.orientation.y = 0;
				pose.orientation.z = 0;
				pose.orientation.w = 1;
				pose.position.x = apps[a].px;
				pose.position.y = apps[a].py;
				pose.position.z = apps[a].pz;

				r = ipc_call_shell_set_window_pose(
				    ipc_c, id, &pose,
				    apps[a].width_m, apps[a].height_m);
				if (r == XRT_SUCCESS) {
					P("Applied pose to client %u: pos=(%.3f,%.3f,%.3f) size=%.3fx%.3f\n",
					  id, apps[a].px, apps[a].py, apps[a].pz,
					  apps[a].width_m, apps[a].height_m);
				}
				apps[a].pose_applied = true;
				break;
			}
		}
	}
}

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
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	// Unbuffered output so messages appear immediately in redirected mode
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	// Parse arguments
	struct app_entry apps[MAX_APPS];
	int app_count = 0;
	if (parse_args(argc, argv, apps, &app_count) < 0) {
		return 1;
	}

	P("DisplayXR Shell\n");
	if (app_count > 0) {
		P("Will launch %d app(s)\n", app_count);
	}

	// Connect to service. The IPC client library auto-starts displayxr-service
	// if not running. We then send shell_activate to enter shell mode dynamically.
	P("Connecting to service...\n");

	struct ipc_connection ipc_c = {0};
	struct xrt_instance_info info = {0};
	snprintf(info.app_info.application_name,
	         sizeof(info.app_info.application_name),
	         "displayxr-shell");

	xrt_result_t xret = XRT_ERROR_IPC_FAILURE;
	for (int attempt = 0; attempt < 10; attempt++) {
		xret = ipc_client_connection_init(&ipc_c, U_LOGGING_WARN, &info);
		if (xret == XRT_SUCCESS) {
			break;
		}
#ifdef _WIN32
		Sleep(1000);
#else
		usleep(1000000);
#endif
	}
	if (xret != XRT_SUCCESS) {
		PE("Failed to connect to service.\n");
		return 1;
	}

	// Activate shell mode on the service (creates multi-comp window on next client)
	P("Activating shell mode...\n");
	xret = ipc_call_shell_activate(&ipc_c);
	if (xret != XRT_SUCCESS) {
		PE("Warning: shell_activate failed (service may already be in shell mode)\n");
	}

	P("Connected to service.\n");

	// Launch apps
#ifdef _WIN32
	if (app_count > 0) {
		char runtime_json[MAX_PATH] = {0};
		bool have_json = get_runtime_json_path(runtime_json, sizeof(runtime_json));
		P("XR_RUNTIME_JSON = %s\n", have_json ? runtime_json : "(not set)");

		for (int i = 0; i < app_count; i++) {
			launch_app(&apps[i], have_json ? runtime_json : NULL);
			// Delay between launches so each app has time to connect
			// before the next one starts (IPC pipe is single-instance)
			if (i + 1 < app_count) {
				Sleep(3000);
			}
		}
	}
#endif

	P("Monitoring clients (Ctrl+C to exit)...\n");

	uint32_t prev_ids[IPC_MAX_CLIENTS] = {0};
	uint32_t prev_count = 0;
	bool poses_pending = false;
	for (int i = 0; i < app_count; i++) {
		if (apps[i].has_pose) {
			poses_pending = true;
		}
	}

	// Poll loop
	while (g_running) {
		// Apply pending poses when new clients appear
		if (poses_pending) {
			try_apply_poses(&ipc_c, apps, app_count, prev_ids, prev_count);

			// Check if all poses applied
			poses_pending = false;
			for (int i = 0; i < app_count; i++) {
				if (apps[i].has_pose && !apps[i].pose_applied) {
					poses_pending = true;
				}
			}
		}

		print_clients(&ipc_c, prev_ids, &prev_count);

#ifdef _WIN32
		Sleep(500);
#else
		usleep(500000);
#endif
	}

	P("\nShell exiting.\n");

#ifdef _WIN32
	// Close process handles
	for (int i = 0; i < app_count; i++) {
		if (apps[i].process != NULL) {
			CloseHandle(apps[i].process);
		}
	}
#endif

	ipc_client_connection_fini(&ipc_c);
	return 0;
}
