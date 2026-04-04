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

#include <cjson/cJSON.h>

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

#define MAX_SAVED_WINDOWS 16

struct saved_window
{
	char app_name[128];
	float x, y, z;
	float width_m, height_m;
};

struct shell_config
{
	struct saved_window windows[MAX_SAVED_WINDOWS];
	int window_count;
};

static void
get_config_path(char *buf, size_t buf_size)
{
#ifdef _WIN32
	const char *appdata = getenv("LOCALAPPDATA");
	if (appdata) {
		snprintf(buf, buf_size, "%s\\DisplayXR\\shell_layout.json", appdata);
	} else {
		snprintf(buf, buf_size, "shell_layout.json");
	}
#else
	const char *home = getenv("HOME");
	if (home) {
		snprintf(buf, buf_size, "%s/.displayxr/shell_layout.json", home);
	} else {
		snprintf(buf, buf_size, "shell_layout.json");
	}
#endif
}

static void
shell_config_load(struct shell_config *cfg)
{
	cfg->window_count = 0;

	char path[512];
	get_config_path(path, sizeof(path));

	FILE *f = fopen(path, "rb");
	if (!f) return;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 64 * 1024) { fclose(f); return; }

	char *data = (char *)malloc(len + 1);
	fread(data, 1, len, f);
	data[len] = '\0';
	fclose(f);

	cJSON *root = cJSON_Parse(data);
	free(data);
	if (!root) return;

	cJSON *windows = cJSON_GetObjectItemCaseSensitive(root, "windows");
	if (cJSON_IsObject(windows)) {
		cJSON *entry = NULL;
		cJSON_ArrayForEach(entry, windows)
		{
			if (cfg->window_count >= MAX_SAVED_WINDOWS) break;
			struct saved_window *sw = &cfg->windows[cfg->window_count];
			snprintf(sw->app_name, sizeof(sw->app_name), "%s", entry->string);

			cJSON *jx = cJSON_GetObjectItemCaseSensitive(entry, "x");
			cJSON *jy = cJSON_GetObjectItemCaseSensitive(entry, "y");
			cJSON *jz = cJSON_GetObjectItemCaseSensitive(entry, "z");
			cJSON *jw = cJSON_GetObjectItemCaseSensitive(entry, "w");
			cJSON *jh = cJSON_GetObjectItemCaseSensitive(entry, "h");
			if (cJSON_IsNumber(jx)) sw->x = (float)jx->valuedouble;
			if (cJSON_IsNumber(jy)) sw->y = (float)jy->valuedouble;
			if (cJSON_IsNumber(jz)) sw->z = (float)jz->valuedouble;
			if (cJSON_IsNumber(jw)) sw->width_m = (float)jw->valuedouble;
			if (cJSON_IsNumber(jh)) sw->height_m = (float)jh->valuedouble;
			cfg->window_count++;
		}
	}
	cJSON_Delete(root);
	P("Loaded %d saved window poses from %s\n", cfg->window_count, path);
}

static void
shell_config_save(const struct shell_config *cfg)
{
	if (cfg->window_count == 0) return;

	char path[512];
	get_config_path(path, sizeof(path));

	// Ensure directory exists
#ifdef _WIN32
	{
		char dir[512];
		snprintf(dir, sizeof(dir), "%s", path);
		char *last = strrchr(dir, '\\');
		if (last) { *last = '\0'; CreateDirectoryA(dir, NULL); }
	}
#endif

	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "version", 1);
	cJSON *windows = cJSON_AddObjectToObject(root, "windows");

	for (int i = 0; i < cfg->window_count; i++) {
		const struct saved_window *sw = &cfg->windows[i];
		cJSON *entry = cJSON_AddObjectToObject(windows, sw->app_name);
		cJSON_AddNumberToObject(entry, "x", sw->x);
		cJSON_AddNumberToObject(entry, "y", sw->y);
		cJSON_AddNumberToObject(entry, "z", sw->z);
		cJSON_AddNumberToObject(entry, "w", sw->width_m);
		cJSON_AddNumberToObject(entry, "h", sw->height_m);
	}

	char *json_str = cJSON_Print(root);
	cJSON_Delete(root);

	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(json_str, 1, strlen(json_str), f);
		fclose(f);
	}
	free(json_str);
}

static struct saved_window *
shell_config_find(struct shell_config *cfg, const char *app_name)
{
	for (int i = 0; i < cfg->window_count; i++) {
		if (strcmp(cfg->windows[i].app_name, app_name) == 0) {
			return &cfg->windows[i];
		}
	}
	return NULL;
}

static void
shell_config_update(struct shell_config *cfg, const char *app_name,
                    float x, float y, float z, float w, float h)
{
	struct saved_window *sw = shell_config_find(cfg, app_name);
	if (!sw) {
		if (cfg->window_count >= MAX_SAVED_WINDOWS) return;
		sw = &cfg->windows[cfg->window_count++];
		snprintf(sw->app_name, sizeof(sw->app_name), "%s", app_name);
	}
	sw->x = x; sw->y = y; sw->z = z;
	sw->width_m = w; sw->height_m = h;
}

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
			// Minimal delay between app launches. The 3s delay from Phase 1 was
			// a workaround for #108 (intermittent crash with two apps). Tested
			// with 100ms and no crashes observed (2026-04-03).
			if (i + 1 < app_count) {
				Sleep(100);
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

	// Load saved window config
	struct shell_config config;
	shell_config_load(&config);

	int save_counter = 0; // Save every 5 seconds (10 polls * 500ms)

	// Client ID → numbered name mapping (for persistence with duplicate app names)
	static struct { uint32_t id; char name[128]; } client_names[MAX_SAVED_WINDOWS];
	static int client_name_count = 0;

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

		// Detect new clients and restore saved poses from config.
		// Use numbered names for duplicate apps (AppName, AppName-2, etc.)
		{

			struct ipc_client_list clients;
			xrt_result_t r = ipc_call_system_get_clients(&ipc_c, &clients);
			if (r == XRT_SUCCESS) {
				for (uint32_t c = 0; c < clients.id_count; c++) {
					bool is_new = true;
					for (uint32_t p = 0; p < prev_count; p++) {
						if (clients.ids[c] == prev_ids[p]) {
							is_new = false;
							break;
						}
					}
					if (is_new && clients.ids[c] != 0) {
						struct ipc_app_state ias;
						xrt_result_t ir = ipc_call_system_get_client_info(&ipc_c, clients.ids[c], &ias);
						if (ir == XRT_SUCCESS && ias.info.application_name[0] != '\0') {
							// Count existing instances to generate numbered name
							int instance = 1;
							for (int cn = 0; cn < client_name_count; cn++) {
								// Strip " (N)" suffix if present
								char existing_base[128];
								snprintf(existing_base, sizeof(existing_base), "%s", client_names[cn].name);
								char *paren = strrchr(existing_base, '(');
								if (paren && paren > existing_base && *(paren - 1) == ' ')
									*(paren - 1) = '\0';
								if (strcmp(existing_base, ias.info.application_name) == 0)
									instance++;
							}

							char numbered_name[128];
							if (instance > 1)
								snprintf(numbered_name, sizeof(numbered_name), "%s (%d)",
								         ias.info.application_name, instance);
							else
								snprintf(numbered_name, sizeof(numbered_name), "%s",
								         ias.info.application_name);

							// Track client_id → numbered_name mapping
							if (client_name_count < MAX_SAVED_WINDOWS) {
								client_names[client_name_count].id = clients.ids[c];
								snprintf(client_names[client_name_count].name, 128, "%s", numbered_name);
								client_name_count++;
							}

							// Restore saved pose if available
							struct saved_window *sw = shell_config_find(&config, numbered_name);
							if (sw && sw->width_m > 0 && sw->height_m > 0) {
								struct xrt_pose pose = XRT_POSE_IDENTITY;
								pose.position.x = sw->x;
								pose.position.y = sw->y;
								pose.position.z = sw->z;
								ipc_call_shell_set_window_pose(&ipc_c, clients.ids[c],
								                                &pose, sw->width_m, sw->height_m);
								P("  Restored pose for '%s' from config\n", numbered_name);
							}
						}
					}
				}
			}
		}

		print_clients(&ipc_c, prev_ids, &prev_count);

		// Periodic save: query current poses and update config
		save_counter++;
		if (save_counter >= 10) {
			save_counter = 0;
			bool changed = false;
			struct ipc_client_list clients;
			xrt_result_t r = ipc_call_system_get_clients(&ipc_c, &clients);
			if (r == XRT_SUCCESS) {
				for (uint32_t c = 0; c < clients.id_count; c++) {
					// Look up numbered name from our mapping
					const char *save_name = NULL;
					for (int cn = 0; cn < client_name_count; cn++) {
						if (client_names[cn].id == clients.ids[c]) {
							save_name = client_names[cn].name;
							break;
						}
					}
					if (!save_name) continue;

					struct xrt_pose pose;
					float w, h;
					if (ipc_call_shell_get_window_pose(&ipc_c, clients.ids[c],
					                                    &pose, &w, &h) == XRT_SUCCESS) {
						struct saved_window *sw = shell_config_find(&config, save_name);
						if (!sw || sw->x != pose.position.x || sw->y != pose.position.y ||
						    sw->width_m != w || sw->height_m != h) {
							shell_config_update(&config, save_name,
							                    pose.position.x, pose.position.y, pose.position.z,
							                    w, h);
							changed = true;
						}
					}
				}
			}
			if (changed) {
				shell_config_save(&config);
			}
		}

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
