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

#include "shell_app_scan.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <tlhelp32.h> // For process enumeration (service PID lookup)
#include <shellapi.h> // For Shell_NotifyIcon (system tray)
#include <commdlg.h>  // For GetOpenFileNameA (Phase 5.14 Browse tile)
#else
#include <unistd.h>
#endif


#define P(...) fprintf(stdout, __VA_ARGS__)
#define PE(...) fprintf(stderr, __VA_ARGS__)

#ifdef _WIN32
#define HOTKEY_TOGGLE 1
#define HOTKEY_LAUNCH 2
#define WM_TRAYICON (WM_USER + 1)
#define TRAY_CMD_ACTIVATE 2001
#define TRAY_CMD_EXIT 2002
#define POLL_INTERVAL_MS 500
#endif

#define MAX_APPS 8
#define MAX_CAPTURES 24

static volatile int g_running = 1;
#ifdef _WIN32
static bool g_shell_active = false;
static bool g_launcher_visible = false; // Phase 5.7: spatial launcher panel toggle (Ctrl+L)
static HWND g_msg_hwnd = NULL;
#endif

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

struct capture_entry
{
	uint64_t hwnd;
	char name[128];
	uint32_t client_id; // filled after IPC call
	bool added;
};

#define MAX_SAVED_WINDOWS 32

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

// --- 4C.9 / 5.5: Registered apps config ---

#define MAX_REGISTERED_APPS 32

struct registered_app
{
	char name[128];
	char exe_path[MAX_PATH];
	char type[8];          // "3d", "2d", or "" (unknown)
	char source[8];        // "user" | "scan"  (5.5)
	char category[32];     // sidecar "category", default "app"
	char description[256]; // sidecar "description"
	char display_mode[16]; // sidecar "display_mode", default "auto"
	char icon_path[MAX_PATH];     // resolved 2D icon (absolute) or ""
	char icon_3d_path[MAX_PATH];  // resolved SBS icon (absolute) or ""
	char icon_3d_layout[8];       // "sbs-lr"|"sbs-rl"|"tb"|"bt"|""
};

static struct registered_app g_registered_apps[MAX_REGISTERED_APPS];
static int g_registered_app_count = 0;

static void
get_registered_apps_path(char *buf, size_t buf_size)
{
#ifdef _WIN32
	const char *appdata = getenv("LOCALAPPDATA");
	if (appdata) {
		snprintf(buf, buf_size, "%s\\DisplayXR\\registered_apps.json", appdata);
	} else {
		snprintf(buf, buf_size, "registered_apps.json");
	}
#else
	const char *home = getenv("HOME");
	if (home) {
		snprintf(buf, buf_size, "%s/.displayxr/registered_apps.json", home);
	} else {
		snprintf(buf, buf_size, "registered_apps.json");
	}
#endif
}

// Case-insensitive exe_path comparison. Normalizes forward slashes to
// backslashes before comparing so "test_apps/x/build/x.exe" matches
// "test_apps\x\build\x.exe".
static bool
exe_path_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL) return a == b;
	while (*a && *b) {
		int ca = (unsigned char)*a;
		int cb = (unsigned char)*b;
		if (ca == '/') ca = '\\';
		if (cb == '/') cb = '\\';
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb) return false;
		a++; b++;
	}
	return *a == '\0' && *b == '\0';
}

// Read a string field from a cJSON object into a fixed buffer. If the field
// is missing or not a string, dst is left untouched.
static void
json_copy_str(char *dst, size_t dst_size, const cJSON *obj, const char *key)
{
	const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsString(j) && j->valuestring != NULL) {
		snprintf(dst, dst_size, "%s", j->valuestring);
	}
}

static void
registered_app_zero(struct registered_app *app)
{
	memset(app, 0, sizeof(*app));
}

// Merge a single scanned app into the in-memory registry. If an existing
// entry has a matching exe_path it is replaced; otherwise the entry is
// appended.
static void
merge_scanned_app(const struct shell_scanned_app *s)
{
	// Replace any existing entry (user or scan) with the same exe_path.
	for (int i = 0; i < g_registered_app_count; i++) {
		if (exe_path_equal(g_registered_apps[i].exe_path, s->exe_path)) {
			struct registered_app *app = &g_registered_apps[i];
			registered_app_zero(app);
			snprintf(app->name, sizeof(app->name), "%s", s->name);
			snprintf(app->exe_path, sizeof(app->exe_path), "%s", s->exe_path);
			snprintf(app->type, sizeof(app->type), "%s", s->type);
			snprintf(app->source, sizeof(app->source), "scan");
			snprintf(app->category, sizeof(app->category), "%s", s->category);
			snprintf(app->description, sizeof(app->description), "%s", s->description);
			snprintf(app->display_mode, sizeof(app->display_mode), "%s", s->display_mode);
			snprintf(app->icon_path, sizeof(app->icon_path), "%s", s->icon_path);
			snprintf(app->icon_3d_path, sizeof(app->icon_3d_path), "%s", s->icon_3d_path);
			snprintf(app->icon_3d_layout, sizeof(app->icon_3d_layout), "%s", s->icon_3d_layout);
			return;
		}
	}

	// Append as a new entry.
	if (g_registered_app_count >= MAX_REGISTERED_APPS) {
		PE("registry full — dropping scanned app '%s'\n", s->name);
		return;
	}
	struct registered_app *app = &g_registered_apps[g_registered_app_count++];
	registered_app_zero(app);
	snprintf(app->name, sizeof(app->name), "%s", s->name);
	snprintf(app->exe_path, sizeof(app->exe_path), "%s", s->exe_path);
	snprintf(app->type, sizeof(app->type), "%s", s->type);
	snprintf(app->source, sizeof(app->source), "scan");
	snprintf(app->category, sizeof(app->category), "%s", s->category);
	snprintf(app->description, sizeof(app->description), "%s", s->description);
	snprintf(app->display_mode, sizeof(app->display_mode), "%s", s->display_mode);
	snprintf(app->icon_path, sizeof(app->icon_path), "%s", s->icon_path);
	snprintf(app->icon_3d_path, sizeof(app->icon_3d_path), "%s", s->icon_3d_path);
	snprintf(app->icon_3d_layout, sizeof(app->icon_3d_layout), "%s", s->icon_3d_layout);
}

// Remove every entry whose source == "scan". Called before re-running the
// scanner so stale scan results from previous runs don't linger.
static void
drop_scan_entries(void)
{
	int dst = 0;
	for (int i = 0; i < g_registered_app_count; i++) {
		if (strcmp(g_registered_apps[i].source, "scan") != 0) {
			if (dst != i) {
				g_registered_apps[dst] = g_registered_apps[i];
			}
			dst++;
		}
	}
	g_registered_app_count = dst;
}

// Forward declarations — these are called from registered_apps_load but
// defined later in this file.
static void
registered_apps_save(void);
#ifdef _WIN32
static void
get_exe_dir(char *buf, size_t buf_size);
#endif

static void
registered_apps_load(void)
{
	g_registered_app_count = 0;

	char path[512];
	get_registered_apps_path(path, sizeof(path));

#ifdef _WIN32
	{
		// Make sure the parent directory exists before we try to write.
		char dir[512];
		snprintf(dir, sizeof(dir), "%s", path);
		char *last = strrchr(dir, '\\');
		if (last) { *last = '\0'; CreateDirectoryA(dir, NULL); }
	}
#endif

	// -------- 1) Load existing JSON, if present. --------
	FILE *f = fopen(path, "rb");
	if (f != NULL) {
		fseek(f, 0, SEEK_END);
		long len = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (len > 0 && len <= 256 * 1024) {
			char *data = (char *)malloc((size_t)len + 1);
			if (data != NULL) {
				fread(data, 1, (size_t)len, f);
				data[len] = '\0';

				cJSON *root = cJSON_Parse(data);
				free(data);
				if (root && cJSON_IsArray(root)) {
					cJSON *entry = NULL;
					cJSON_ArrayForEach(entry, root)
					{
						if (g_registered_app_count >= MAX_REGISTERED_APPS) break;
						struct registered_app *app =
						    &g_registered_apps[g_registered_app_count];
						registered_app_zero(app);

						json_copy_str(app->name, sizeof(app->name), entry, "name");
						json_copy_str(app->exe_path, sizeof(app->exe_path), entry, "exe_path");
						json_copy_str(app->type, sizeof(app->type), entry, "type");
						json_copy_str(app->source, sizeof(app->source), entry, "source");
						json_copy_str(app->category, sizeof(app->category), entry, "category");
						json_copy_str(app->description, sizeof(app->description), entry, "description");
						json_copy_str(app->display_mode, sizeof(app->display_mode), entry, "display_mode");
						json_copy_str(app->icon_path, sizeof(app->icon_path), entry, "icon_path");
						json_copy_str(app->icon_3d_path, sizeof(app->icon_3d_path), entry, "icon_3d_path");
						json_copy_str(app->icon_3d_layout, sizeof(app->icon_3d_layout), entry, "icon_3d_layout");

						// Back-compat: a Phase 4C JSON has no `source` field.
						// Treat pre-existing entries as user-added.
						if (app->source[0] == '\0') {
							snprintf(app->source, sizeof(app->source), "user");
						}

						g_registered_app_count++;
					}
				}
				cJSON_Delete(root);
			}
		}
		fclose(f);
	} else {
		P("No registered_apps.json found — starting with empty registry.\n");
		// Per the spec, the launcher is manifest-gated: only apps with a
		// .displayxr.json sidecar (or apps the user explicitly adds via
		// Browse-for-app) appear. No built-in defaults. The scanner runs
		// next and populates anything it finds.
	}

	// -------- 2) Drop stale scan entries, re-run scanner, merge. --------
	drop_scan_entries();

#ifdef _WIN32
	{
		char exe_dir[MAX_PATH] = {0};
		get_exe_dir(exe_dir, sizeof(exe_dir));
		size_t edl = strlen(exe_dir);
		if (edl > 0 && (exe_dir[edl - 1] == '\\' || exe_dir[edl - 1] == '/')) {
			exe_dir[edl - 1] = '\0';
		}

		struct shell_scanned_app scanned[MAX_REGISTERED_APPS];
		int n = shell_scan_apps(exe_dir, scanned, MAX_REGISTERED_APPS);
		for (int i = 0; i < n; i++) {
			merge_scanned_app(&scanned[i]);
		}
	}
#endif

	P("Registry: %d app(s) after merge.\n", g_registered_app_count);
	for (int i = 0; i < g_registered_app_count; i++) {
		P("  [%s] %s  (%s)\n", g_registered_apps[i].source, g_registered_apps[i].name,
		  g_registered_apps[i].exe_path);
	}

	// -------- 3) Persist merged registry so users can hand-edit later. --------
	registered_apps_save();
}

static void
registered_apps_save(void)
{
	char path[512];
	get_registered_apps_path(path, sizeof(path));

	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < g_registered_app_count; i++) {
		cJSON *obj = cJSON_CreateObject();
		const struct registered_app *app = &g_registered_apps[i];
		cJSON_AddStringToObject(obj, "name", app->name);
		cJSON_AddStringToObject(obj, "exe_path", app->exe_path);
		cJSON_AddStringToObject(obj, "type", app->type);
		cJSON_AddStringToObject(obj, "source", app->source[0] ? app->source : "user");
		if (app->category[0])       cJSON_AddStringToObject(obj, "category", app->category);
		if (app->description[0])    cJSON_AddStringToObject(obj, "description", app->description);
		if (app->display_mode[0])   cJSON_AddStringToObject(obj, "display_mode", app->display_mode);
		if (app->icon_path[0])      cJSON_AddStringToObject(obj, "icon_path", app->icon_path);
		if (app->icon_3d_path[0])   cJSON_AddStringToObject(obj, "icon_3d_path", app->icon_3d_path);
		if (app->icon_3d_layout[0]) cJSON_AddStringToObject(obj, "icon_3d_layout", app->icon_3d_layout);
		cJSON_AddItemToArray(arr, obj);
	}
	char *json_str = cJSON_Print(arr);
	cJSON_Delete(arr);

	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(json_str, 1, strlen(json_str), f);
		fclose(f);
	}
	free(json_str);
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
	// Phase 6.1 (#140): start the app with its window hidden. In shell
	// mode, the app's HWND appearing on the 3D display disrupts the SR
	// SDK's weaver and causes a multi-second stretch artifact. The app's
	// content is captured via shared handles into the multi-compositor
	// atlas, so its own window doesn't need to be visible.
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
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
	P("Usage: displayxr-shell [options] [--pose x,y,z,w,h] app1.exe ...\n");
	P("\n");
	P("Options:\n");
	P("  --pose x,y,z,w,h       Set window pose for the next app argument\n");
	P("                          x,y,z = position (meters from display center)\n");
	P("                          w,h = window width and height (meters)\n");
	P("  --capture-hwnd <hwnd>   Capture a 2D window by its HWND (decimal or 0x hex)\n");
	P("                          Can be specified multiple times\n");
	P("  --help                  Show this help\n");
	P("\n");
	P("If no apps are specified, runs in monitor-only mode.\n");
	P("The service (displayxr-service --shell) is auto-started if needed.\n");
}

static int
parse_args(int argc, char *argv[], struct app_entry *apps, int *app_count,
           struct capture_entry *captures, int *capture_count)
{
	*app_count = 0;
	*capture_count = 0;
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
		if (strcmp(argv[i], "--capture-hwnd") == 0) {
			if (i + 1 >= argc) {
				PE("Error: --capture-hwnd requires HWND value\n");
				return -1;
			}
			i++;
			if (*capture_count >= MAX_CAPTURES) {
				PE("Warning: max %d captures, ignoring %s\n", MAX_CAPTURES, argv[i]);
				continue;
			}
			struct capture_entry *ce = &captures[*capture_count];
			memset(ce, 0, sizeof(*ce));
			ce->hwnd = (uint64_t)strtoull(argv[i], NULL, 0); // supports decimal and 0x hex
#ifdef _WIN32
			{
				char title[128] = "Captured Window";
				GetWindowTextA((HWND)(uintptr_t)ce->hwnd, title, sizeof(title));
				snprintf(ce->name, sizeof(ce->name), "%s", title);
			}
#else
			snprintf(ce->name, sizeof(ce->name), "Capture HWND %llu", (unsigned long long)ce->hwnd);
#endif
			(*capture_count)++;
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

#ifdef _WIN32
/*
 * Window enumeration for auto-adoption of desktop windows.
 */

/*!
 * Find the PID of displayxr-service.exe (to exclude its windows from adoption).
 */
static DWORD
find_service_pid(void)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return 0;

	PROCESSENTRY32 pe = {0};
	pe.dwSize = sizeof(pe);
	DWORD pid = 0;
	if (Process32First(snap, &pe)) {
		do {
			if (_stricmp(pe.szExeFile, "displayxr-service.exe") == 0) {
				pid = pe.th32ProcessID;
				break;
			}
		} while (Process32Next(snap, &pe));
	}
	CloseHandle(snap);
	return pid;
}

struct enum_ctx
{
	HWND found[MAX_CAPTURES];
	int count;
	DWORD shell_pid;   // our own process
	DWORD service_pid; // displayxr-service process
};

static BOOL CALLBACK
enum_windows_cb(HWND hwnd, LPARAM lParam)
{
	struct enum_ctx *ctx = (struct enum_ctx *)lParam;
	if (ctx->count >= MAX_CAPTURES) {
		return FALSE;
	}

	if (!IsWindowVisible(hwnd)) {
		return TRUE;
	}

	// Skip owned popups
	if (GetWindow(hwnd, GW_OWNER) != NULL) {
		return TRUE;
	}

	// Skip our own process and service process windows
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == ctx->shell_pid || pid == ctx->service_pid) {
		return TRUE;
	}

	// Skip known system window classes
	char class_name[256] = {0};
	GetClassNameA(hwnd, class_name, sizeof(class_name));
	if (strcmp(class_name, "Shell_TrayWnd") == 0 ||
	    strcmp(class_name, "Progman") == 0 ||
	    strcmp(class_name, "WorkerW") == 0 ||
	    strcmp(class_name, "Button") == 0 ||
	    strcmp(class_name, "NotifyIconOverflowWindow") == 0 ||
	    strcmp(class_name, "Windows.UI.Core.CoreWindow") == 0 ||
	    strcmp(class_name, "Shell_SecondaryTrayWnd") == 0 ||
	    strcmp(class_name, "XamlExplorerHostIslandWindow") == 0 ||
	    strcmp(class_name, "TopLevelWindowForOverflowXamlIsland") == 0) {
		return TRUE;
	}

	// Skip tiny windows (tooltips, helpers, hidden overlays)
	RECT rect;
	GetClientRect(hwnd, &rect);
	if ((rect.right - rect.left) < 100 || (rect.bottom - rect.top) < 100) {
		return TRUE;
	}

	// Skip windows with WS_EX_TOOLWINDOW (already hidden from taskbar)
	LONG_PTR exstyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
	if (exstyle & WS_EX_TOOLWINDOW) {
		return TRUE;
	}

	ctx->found[ctx->count++] = hwnd;
	return TRUE;
}

/*!
 * Enumerate visible desktop windows and adopt new ones as capture clients.
 * Skips windows already tracked in the captures array and IPC clients.
 */
static void
enumerate_and_adopt_windows(struct ipc_connection *ipc_c,
                            struct capture_entry *captures,
                            int *capture_count,
                            DWORD service_pid)
{
	// Enumerate visible top-level windows
	struct enum_ctx ctx = {0};
	ctx.shell_pid = GetCurrentProcessId();
	ctx.service_pid = service_pid;
	EnumWindows(enum_windows_cb, (LPARAM)&ctx);

	// Get current IPC client PIDs to skip OpenXR 3D apps
	DWORD ipc_pids[IPC_MAX_CLIENTS] = {0};
	int ipc_pid_count = 0;
	{
		struct ipc_client_list clients;
		if (ipc_call_system_get_clients(ipc_c, &clients) == XRT_SUCCESS) {
			for (uint32_t c = 0; c < clients.id_count; c++) {
				if (clients.ids[c] == 0) continue;
				struct ipc_app_state ias;
				if (ipc_call_system_get_client_info(ipc_c, clients.ids[c], &ias) == XRT_SUCCESS) {
					ipc_pids[ipc_pid_count++] = ias.pid;
					if (ipc_pid_count >= IPC_MAX_CLIENTS) break;
				}
			}
		}
	}

	for (int i = 0; i < ctx.count; i++) {
		HWND hwnd = ctx.found[i];
		uint64_t hwnd_val = (uint64_t)(uintptr_t)hwnd;

		// Skip if already tracked
		bool already_tracked = false;
		for (int j = 0; j < *capture_count; j++) {
			if (captures[j].hwnd == hwnd_val && captures[j].added) {
				already_tracked = true;
				break;
			}
		}
		if (already_tracked) continue;

		// Skip if this HWND belongs to an IPC client process
		DWORD hwnd_pid = 0;
		GetWindowThreadProcessId(hwnd, &hwnd_pid);
		bool is_ipc = false;
		for (int j = 0; j < ipc_pid_count; j++) {
			if (ipc_pids[j] == hwnd_pid) {
				is_ipc = true;
				break;
			}
		}
		if (is_ipc) continue;

		// Adopt this window
		if (*capture_count >= MAX_CAPTURES) {
			P("  Auto-adopt: max captures (%d) reached\n", MAX_CAPTURES);
			break;
		}

		struct capture_entry *ce = &captures[*capture_count];
		memset(ce, 0, sizeof(*ce));
		ce->hwnd = hwnd_val;
		{
			char title[128] = "Captured Window";
			GetWindowTextA(hwnd, title, sizeof(title));
			snprintf(ce->name, sizeof(ce->name), "%s", title);
		}

		uint32_t cid = 0;
		xrt_result_t r = ipc_call_shell_add_capture_client(ipc_c, ce->hwnd, &cid);
		if (r == XRT_SUCCESS) {
			ce->client_id = cid;
			ce->added = true;
			(*capture_count)++;
			P("  Auto-adopt: HWND=0x%llx '%s' → client_id=%u\n",
			  (unsigned long long)ce->hwnd, ce->name, cid);
		} else {
			P("  Auto-adopt: failed for HWND=0x%llx '%s'\n",
			  (unsigned long long)ce->hwnd, ce->name);
		}
	}
}

/*!
 * Check for closed capture windows and remove their capture clients.
 */
static void
cleanup_closed_captures(struct ipc_connection *ipc_c,
                        struct capture_entry *captures,
                        int *capture_count)
{
	for (int i = 0; i < *capture_count; i++) {
		if (!captures[i].added) continue;
		if (!IsWindow((HWND)(uintptr_t)captures[i].hwnd)) {
			P("  Window closed: '%s' (client_id=%u)\n", captures[i].name, captures[i].client_id);
			ipc_call_shell_remove_capture_client(ipc_c, captures[i].client_id);
			// Shift remaining entries
			for (int j = i; j < *capture_count - 1; j++) {
				captures[j] = captures[j + 1];
			}
			(*capture_count)--;
			i--; // Re-check this index
		}
	}
}

// --- 5.6: Running-app set (for launcher "running" tag) ---

#define SHELL_RUNNING_NAMES_MAX 32

struct shell_running_set
{
	int count;
	char names[SHELL_RUNNING_NAMES_MAX][128];
};

/*!
 * Snapshot the set of application_name strings for every IPC client currently
 * connected to the service. Used by the launcher to highlight tiles whose app
 * is already running.
 *
 * Wraps ipc_call_system_get_clients + ipc_call_system_get_client_info so the
 * launcher UI doesn't have to speak IPC directly. Cheap enough to call every
 * time the launcher opens; not intended for per-frame use.
 *
 * On IPC failure, returns an empty set — the launcher should degrade to "no
 * running apps" rather than failing to open.
 */
static void
shell_get_running_app_set(struct ipc_connection *ipc_c, struct shell_running_set *out)
{
	out->count = 0;

	struct ipc_client_list clients;
	xrt_result_t r = ipc_call_system_get_clients(ipc_c, &clients);
	if (r != XRT_SUCCESS) {
		return;
	}

	for (uint32_t c = 0; c < clients.id_count; c++) {
		if (clients.ids[c] == 0) continue;
		if (out->count >= SHELL_RUNNING_NAMES_MAX) break;

		struct ipc_app_state ias;
		xrt_result_t ir = ipc_call_system_get_client_info(ipc_c, clients.ids[c], &ias);
		if (ir != XRT_SUCCESS) continue;
		if (ias.info.application_name[0] == '\0') continue;

		// Deduplicate — two instances of the same app are one tile in the
		// launcher (for highlight purposes).
		bool already = false;
		for (int i = 0; i < out->count; i++) {
			if (strcmp(out->names[i], ias.info.application_name) == 0) {
				already = true;
				break;
			}
		}
		if (already) continue;

		snprintf(out->names[out->count], sizeof(out->names[0]), "%s",
		         ias.info.application_name);
		out->count++;
	}
}

/*!
 * True if @p app_name matches any currently-running client in @p set. The
 * comparison is case-sensitive and expects the application_name as passed to
 * xrCreateInstance.
 */
static bool
shell_running_set_contains(const struct shell_running_set *set, const char *app_name)
{
	if (set == NULL || app_name == NULL || !*app_name) return false;
	for (int i = 0; i < set->count; i++) {
		if (strcmp(set->names[i], app_name) == 0) return true;
	}
	return false;
}

/*!
 * Phase 5.11: case-insensitive normalized exe-path equality. Treats
 * forward slashes as backslashes so registry-stored paths match what
 * QueryFullProcessImageNameW returns.
 */
static bool
shell_exe_paths_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL) return a == b;
	while (*a && *b) {
		int ca = (unsigned char)*a;
		int cb = (unsigned char)*b;
		if (ca == '/') ca = '\\';
		if (cb == '/') cb = '\\';
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb) return false;
		a++; b++;
	}
	return *a == '\0' && *b == '\0';
}

#ifdef _WIN32
/*!
 * Phase 5.11: resolve a PID to its absolute exe path via
 * QueryFullProcessImageNameW. Writes a UTF-8 path into @p out, returns true
 * on success.
 */
static bool
shell_pid_to_exe_path(DWORD pid, char *out, size_t out_size)
{
	if (pid == 0 || out == NULL || out_size == 0) return false;

	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h == NULL) return false;

	wchar_t wpath[MAX_PATH] = {0};
	DWORD wlen = MAX_PATH;
	BOOL ok = QueryFullProcessImageNameW(h, 0, wpath, &wlen);
	CloseHandle(h);
	if (!ok || wlen == 0) return false;

	int n = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)wlen, out, (int)out_size - 1, NULL, NULL);
	if (n <= 0) return false;
	out[n] = '\0';
	return true;
}
#endif

/*!
 * Phase 5.11: build a bitmask of registered apps that have at least one
 * matching IPC client currently connected. Bit @c i set means
 * g_registered_apps[i].exe_path equals (case-insensitive, slash-normalized)
 * the exe path of some IPC client's PID.
 *
 * Returns 0 on Windows if no clients are connected; non-Windows always 0.
 */
static uint64_t
shell_compute_running_tile_mask(struct ipc_connection *ipc_c)
{
#ifdef _WIN32
	uint64_t mask = 0;

	struct ipc_client_list clients;
	if (ipc_call_system_get_clients(ipc_c, &clients) != XRT_SUCCESS) {
		return 0;
	}

	// Collect each connected client's exe path once.
	char client_exes[IPC_MAX_CLIENTS][MAX_PATH];
	int client_exe_count = 0;
	for (uint32_t c = 0; c < clients.id_count; c++) {
		if (clients.ids[c] == 0) continue;
		struct ipc_app_state ias;
		if (ipc_call_system_get_client_info(ipc_c, clients.ids[c], &ias) != XRT_SUCCESS)
			continue;
		if (ias.pid == 0) continue;
		if (shell_pid_to_exe_path((DWORD)ias.pid, client_exes[client_exe_count],
		                          sizeof(client_exes[0]))) {
			client_exe_count++;
			if (client_exe_count >= IPC_MAX_CLIENTS) break;
		}
	}

	// Match each registered app against the connected exe set.
	int cap = g_registered_app_count;
	if (cap > 64) cap = 64; // mask is uint64_t
	for (int i = 0; i < cap; i++) {
		const char *reg_exe = g_registered_apps[i].exe_path;
		for (int j = 0; j < client_exe_count; j++) {
			if (shell_exe_paths_equal(reg_exe, client_exes[j])) {
				mask |= (1ULL << i);
				break;
			}
		}
	}

	return mask;
#else
	(void)ipc_c;
	return 0;
#endif
}

/*!
 * Phase 5.8: ship the current g_registered_apps[] array to the service so
 * the spatial launcher panel can render its tile grid. Uses a clear+add
 * pattern (one IPC call per app) so each message stays under IPC_BUF_SIZE.
 * The shell must re-call this whenever the registry changes (browse-for-app,
 * remove, refresh).
 */
static void
shell_push_registered_apps_to_service(struct ipc_connection *ipc_c)
{
	xrt_result_t r = ipc_call_shell_clear_launcher_apps(ipc_c);
	if (r != XRT_SUCCESS) {
		PE("ipc_call_shell_clear_launcher_apps failed: %d\n", r);
		return;
	}

	int pushed = 0;
	int cap = g_registered_app_count;
	if (cap > IPC_LAUNCHER_MAX_APPS) {
		cap = IPC_LAUNCHER_MAX_APPS;
	}
	for (int i = 0; i < cap; i++) {
		const struct registered_app *src = &g_registered_apps[i];

		struct ipc_launcher_app msg;
		memset(&msg, 0, sizeof(msg));
		snprintf(msg.name, sizeof(msg.name), "%s", src->name);
		snprintf(msg.exe_path, sizeof(msg.exe_path), "%s", src->exe_path);
		snprintf(msg.type, sizeof(msg.type), "%s", src->type);

		r = ipc_call_shell_add_launcher_app(ipc_c, &msg);
		if (r != XRT_SUCCESS) {
			PE("ipc_call_shell_add_launcher_app[%d] failed: %d\n", i, r);
			return;
		}
		pushed++;
	}
	P("Pushed %d app(s) to launcher.\n", pushed);
}

#ifdef _WIN32
/*!
 * Phase 5.14: prompt the user for an executable via GetOpenFileNameA,
 * append it to g_registered_apps as a user entry, persist to JSON, and
 * re-push the registry to the service. Mirrors the OPENFILENAMEA idiom
 * used by comp_d3d11_window.cpp's Ctrl+O launch flow.
 *
 * Called from the launcher click-poll branch when the service signals a
 * Browse-tile hit (IPC_LAUNCHER_ACTION_BROWSE). The dialog is modal and
 * blocks the shell's message loop; on return the launcher is re-shown so
 * the user sees their new tile at the end of the grid.
 */
static void
shell_browse_and_add_app(struct ipc_connection *ipc_c)
{
	// Phase 5.14: open the file dialog as a top-level window
	// (hwndOwner=NULL). The service granted ASFW_ANY foreground permission
	// when it processed the Browse click, so Windows lets the modal dialog
	// activate normally. Using the hidden HWND_MESSAGE g_msg_hwnd as owner
	// doesn't help — message-only windows can't be focused, so the dialog
	// would still hide behind the compositor.
	char path[MAX_PATH] = {0};
	OPENFILENAMEA ofn = {0};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = NULL;
	ofn.lpstrFilter = "Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrTitle = "Add app to DisplayXR launcher";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

	if (!GetOpenFileNameA(&ofn)) {
		P("Browse: canceled\n");
		return;
	}

	if (g_registered_app_count >= MAX_REGISTERED_APPS) {
		PE("Registry full (%d) — cannot add '%s'\n",
		   g_registered_app_count, path);
		return;
	}

	// Derive a display name from the exe basename (strip path and .exe).
	const char *base = strrchr(path, '\\');
	if (base == NULL) base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char name[128];
	snprintf(name, sizeof(name), "%s", base);
	size_t nlen = strlen(name);
	if (nlen >= 4 && _stricmp(name + nlen - 4, ".exe") == 0) {
		name[nlen - 4] = '\0';
	}

	struct registered_app *app = &g_registered_apps[g_registered_app_count++];
	registered_app_zero(app);
	snprintf(app->name, sizeof(app->name), "%s", name);
	snprintf(app->exe_path, sizeof(app->exe_path), "%s", path);
	snprintf(app->type, sizeof(app->type), "3d");   // assume 3d; user can edit JSON
	snprintf(app->source, sizeof(app->source), "user");
	snprintf(app->category, sizeof(app->category), "app");

	registered_apps_save();
	shell_push_registered_apps_to_service(ipc_c);
	P("Browse: added '%s' from %s\n", name, path);
}
#else
static void
shell_browse_and_add_app(struct ipc_connection *ipc_c)
{
	(void)ipc_c;
}
#endif

// --- 4C.10+4C.11: App launch from shell + auto-detect type ---

/*!
 * Launch a registered app from the shell.
 * For "3d" type: uses launch_app() with DISPLAYXR_SHELL_SESSION env.
 * For "2d" type: launches without shell env, then captures via IPC.
 * For unknown type: launches as 3d, polls for IPC connect to auto-detect.
 */
static void
shell_launch_registered_app(struct ipc_connection *ipc_c,
                            struct registered_app *rapp,
                            const char *runtime_json,
                            struct app_entry *apps, int *app_count,
                            struct capture_entry *captures, int *capture_count)
{
	P("Launching: '%s' (%s) — %s\n", rapp->name, rapp->exe_path, rapp->type);

	if (strcmp(rapp->type, "2d") == 0) {
		// 2D app: launch without shell env, then poll for HWND and capture
		STARTUPINFOA si = {0};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi = {0};

		char abs_path[MAX_PATH];
		char cmd[MAX_PATH + 16];
		if (_fullpath(abs_path, rapp->exe_path, MAX_PATH) == NULL) {
			snprintf(abs_path, MAX_PATH, "%s", rapp->exe_path);
		}
		snprintf(cmd, sizeof(cmd), "\"%s\"", abs_path);

		// Clear shell env vars so 2D app doesn't accidentally pick them up
		SetEnvironmentVariableA("DISPLAYXR_SHELL_SESSION", NULL);

		BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
		    CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);
		if (!ok) {
			PE("  Failed to launch 2D app: %lu\n", GetLastError());
			return;
		}
		P("  Launched 2D app PID=%lu, waiting for HWND...\n", pi.dwProcessId);
		CloseHandle(pi.hThread);

		// Poll for HWND owned by this PID (up to 5 seconds)
		HWND found_hwnd = NULL;
		for (int poll = 0; poll < 10 && found_hwnd == NULL; poll++) {
			Sleep(500);
			struct enum_ctx ectx = {0};
			ectx.shell_pid = GetCurrentProcessId();
			ectx.service_pid = 0;
			EnumWindows(enum_windows_cb, (LPARAM)&ectx);
			for (int w = 0; w < ectx.count; w++) {
				DWORD wnd_pid = 0;
				GetWindowThreadProcessId(ectx.found[w], &wnd_pid);
				if (wnd_pid == pi.dwProcessId) {
					found_hwnd = ectx.found[w];
					break;
				}
			}
		}

		if (found_hwnd != NULL && *capture_count < MAX_CAPTURES) {
			struct capture_entry *ce = &captures[*capture_count];
			memset(ce, 0, sizeof(*ce));
			ce->hwnd = (uint64_t)(uintptr_t)found_hwnd;
			GetWindowTextA(found_hwnd, ce->name, sizeof(ce->name));

			uint32_t cid = 0;
			xrt_result_t r = ipc_call_shell_add_capture_client(ipc_c, ce->hwnd, &cid);
			if (r == XRT_SUCCESS) {
				ce->client_id = cid;
				ce->added = true;
				(*capture_count)++;
				P("  Captured 2D window HWND=%p '%s' → client_id=%u\n",
				  (void *)found_hwnd, ce->name, cid);
			}
		} else if (found_hwnd == NULL) {
			PE("  2D app: no visible HWND found after 5s\n");
		}
		CloseHandle(pi.hProcess);
		return;
	}

	// "3d" or unknown type: launch as shell app
	if (*app_count >= MAX_APPS) {
		PE("  Max apps (%d) reached\n", MAX_APPS);
		return;
	}
	struct app_entry *a = &apps[*app_count];
	memset(a, 0, sizeof(*a));
	a->exe_path = rapp->exe_path;
	launch_app(a, runtime_json);
	(*app_count)++;

	// 4C.11: Auto-detect type if unknown
	if (rapp->type[0] == '\0' && a->pid != 0) {
		P("  Unknown type — detecting (polling IPC for 5s)...\n");
		bool found_ipc = false;
		for (int poll = 0; poll < 10 && !found_ipc; poll++) {
			Sleep(500);
			struct ipc_client_list clients;
			if (ipc_call_system_get_clients(ipc_c, &clients) == XRT_SUCCESS) {
				for (uint32_t c = 0; c < clients.id_count; c++) {
					struct ipc_app_state ias;
					if (ipc_call_system_get_client_info(ipc_c, clients.ids[c], &ias) == XRT_SUCCESS) {
						if ((DWORD)ias.pid == a->pid) {
							found_ipc = true;
							break;
						}
					}
				}
			}
		}
		if (found_ipc) {
			snprintf(rapp->type, sizeof(rapp->type), "3d");
			P("  Auto-detected: 3D app (IPC client connected)\n");
		} else {
			snprintf(rapp->type, sizeof(rapp->type), "2d");
			P("  Auto-detected: 2D app (no IPC client after 5s)\n");
			// TODO: find HWND by PID and capture as 2D
		}
		registered_apps_save();
	}
}

/*!
 * Show a simple app launcher dialog listing registered apps.
 * Returns the index of the selected app, or -1 if cancelled.
 */
static int
shell_show_launcher_dialog(void)
{
	if (g_registered_app_count == 0) {
		P("No registered apps.\n");
		return -1;
	}

	// Build a numbered list string for the MessageBox
	char list[2048] = "Select an app to launch:\n\n";
	for (int i = 0; i < g_registered_app_count; i++) {
		char line[256];
		snprintf(line, sizeof(line), "  %d. %s [%s]\n",
		         i + 1, g_registered_apps[i].name,
		         g_registered_apps[i].type[0] ? g_registered_apps[i].type : "auto");
		strncat(list, line, sizeof(list) - strlen(list) - 1);
	}
	strncat(list, "\nEnter the number in the input box below:",
	        sizeof(list) - strlen(list) - 1);

	// Use a simple input loop: cycle through apps with repeated Ctrl+L
	// For now, just launch the first app. A proper dialog is Phase 5.
	// MessageBox to show list, then launch first unlaunched app.
	// TODO(Phase 5): Replace with spatial launcher panel.

	// Find first registered app not currently running
	// For simplicity, just return the first one.
	P("%s\n", list);
	return 0; // Launch first app
}

// --- System tray icon ---

static NOTIFYICONDATAA g_nid = {0};

static void
tray_create(HWND hwnd)
{
	g_nid.cbSize = sizeof(NOTIFYICONDATAA);
	g_nid.hWnd = hwnd;
	g_nid.uID = 1;
	g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	g_nid.uCallbackMessage = WM_TRAYICON;
	g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	strncpy(g_nid.szTip, "DisplayXR Shell (inactive)", sizeof(g_nid.szTip) - 1);
	Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void
tray_update_tooltip(bool active)
{
	strncpy(g_nid.szTip,
	        active ? "DisplayXR Shell (active)" : "DisplayXR Shell (inactive)",
	        sizeof(g_nid.szTip) - 1);
	Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

static void
tray_destroy(void)
{
	Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

static void
tray_show_context_menu(HWND hwnd, bool shell_active)
{
	HMENU menu = CreatePopupMenu();
	AppendMenuA(menu, MF_STRING, TRAY_CMD_ACTIVATE,
	            shell_active ? "Deactivate" : "Activate");
	AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuA(menu, MF_STRING, TRAY_CMD_EXIT, "Exit");

	POINT pt;
	GetCursorPos(&pt);
	SetForegroundWindow(hwnd); // Required for TrackPopupMenu to dismiss properly
	UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
	                          pt.x, pt.y, 0, hwnd, NULL);
	DestroyMenu(menu);
	PostMessage(hwnd, WM_NULL, 0, 0); // Force dismiss

	if (cmd == TRAY_CMD_ACTIVATE) {
		// Handled by caller via return value
		PostMessage(hwnd, WM_HOTKEY, HOTKEY_TOGGLE, 0);
	} else if (cmd == TRAY_CMD_EXIT) {
		g_running = 0;
	}
}

#endif // _WIN32

#ifdef _WIN32
// WIN32 subsystem entry point — no console window.
// Delegates to main() with the command line split into argc/argv.
int WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	(void)hInstance;
	(void)hPrevInstance;
	(void)lpCmdLine;
	(void)nCmdShow;
	return main(__argc, __argv);
}
#endif

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
	struct capture_entry captures[MAX_CAPTURES];
	int capture_count = 0;
	if (parse_args(argc, argv, apps, &app_count, captures, &capture_count) < 0) {
		return 1;
	}

	P("DisplayXR Shell\n");
	if (app_count > 0) {
		P("Will launch %d app(s)\n", app_count);
	}
	if (capture_count > 0) {
		P("Will capture %d window(s)\n", capture_count);
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

	P("Connected to service.\n");

	// Load registered apps config (Phase 4C.9 + Phase 5.5 scanner merge)
	registered_apps_load();

	// Phase 5.8: push the merged registry to the service so the spatial
	// launcher panel can render its tile grid.
	shell_push_registered_apps_to_service(&ipc_c);

#ifdef _WIN32
	// --- Resolve runtime JSON path (needed for app launches) ---
	char runtime_json[MAX_PATH] = {0};
	bool have_json = get_runtime_json_path(runtime_json, sizeof(runtime_json));
	DWORD service_pid = find_service_pid();

	// --- Create message-only window for hotkey + tray ---
	g_msg_hwnd = CreateWindowExA(0, "STATIC", "DisplayXR Shell Msg", 0,
	    0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
	if (g_msg_hwnd == NULL) {
		PE("Warning: failed to create message window (hotkey/tray unavailable)\n");
	}

	// Register system-wide Ctrl+Space hotkey
	if (g_msg_hwnd != NULL) {
		if (!RegisterHotKey(g_msg_hwnd, HOTKEY_TOGGLE, MOD_CONTROL, VK_SPACE)) {
			PE("Warning: RegisterHotKey(Ctrl+Space) failed — hotkey unavailable\n");
		}
		if (!RegisterHotKey(g_msg_hwnd, HOTKEY_LAUNCH, MOD_CONTROL, 'L')) {
			PE("Warning: RegisterHotKey(Ctrl+L) failed — launcher hotkey unavailable\n");
		}
	}

	// Create system tray icon
	if (g_msg_hwnd != NULL) {
		tray_create(g_msg_hwnd);
	}

	// --- Decide startup mode ---
	// If launched with apps or captures: activate immediately (current behavior).
	// If launched with no args: start deactivated in tray, wait for Ctrl+Space.
	bool start_active = (app_count > 0 || capture_count > 0);

	if (start_active) {
		P("Activating shell mode...\n");
		xret = ipc_call_shell_activate(&ipc_c);
		if (xret != XRT_SUCCESS) {
			PE("Warning: shell_activate failed\n");
		}
		g_shell_active = true;
		tray_update_tooltip(true);

		// Add capture clients
		for (int i = 0; i < capture_count; i++) {
			uint32_t cid = 0;
			xrt_result_t r = ipc_call_shell_add_capture_client(&ipc_c, captures[i].hwnd, &cid);
			if (r == XRT_SUCCESS) {
				captures[i].client_id = cid;
				captures[i].added = true;
				P("  Capture: HWND=0x%llx '%s' → client_id=%u\n",
				  (unsigned long long)captures[i].hwnd, captures[i].name, cid);
			} else {
				PE("  Capture: failed for HWND=0x%llx '%s'\n",
				   (unsigned long long)captures[i].hwnd, captures[i].name);
			}
		}

		// Launch apps
		if (app_count > 0) {

			P("XR_RUNTIME_JSON = %s\n", have_json ? runtime_json : "(not set)");
			for (int i = 0; i < app_count; i++) {
				launch_app(&apps[i], have_json ? runtime_json : NULL);
				if (i + 1 < app_count) {
					Sleep(100);
				}
			}
		}
	} else {
		P("Starting in system tray — press Ctrl+Space to activate.\n");
	}

	// Auto-adopt is disabled for now — use --capture-hwnd for explicit 2D windows.
	// TODO(4C.6): re-enable on re-activate to auto-adopt desktop windows.
	bool auto_adopt = false;
#else
	// Non-Windows: simple activate + poll (no hotkey/tray)
	P("Activating shell mode...\n");
	xret = ipc_call_shell_activate(&ipc_c);
	if (xret != XRT_SUCCESS) {
		PE("Warning: shell_activate failed\n");
	}
	bool auto_adopt = false;
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

	int adopt_counter = 0;

	// Client ID → numbered name mapping (for persistence with duplicate app names)
	static struct { uint32_t id; char name[128]; } client_names[MAX_SAVED_WINDOWS];
	static int client_name_count = 0;

	// --- Main loop: MsgWait on Windows, Sleep on other platforms ---
	while (g_running) {
#ifdef _WIN32
		// Wait for either a message or the poll timeout
		DWORD wait_result = MsgWaitForMultipleObjects(
		    0, NULL, FALSE, POLL_INTERVAL_MS, QS_ALLINPUT);

		if (wait_result == WAIT_OBJECT_0) {
			MSG msg;
			while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
				if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_TOGGLE) {
					// --- Toggle shell active/inactive ---
					if (g_shell_active) {
						P("Deactivating shell...\n");
						ipc_call_shell_deactivate(&ipc_c);

						// Clear local capture tracking
						for (int i = 0; i < capture_count; i++) {
							captures[i].added = false;
						}
						capture_count = 0;

						// Reset pose tracking so re-activate can re-apply
						for (int i = 0; i < app_count; i++) {
							apps[i].pose_applied = false;
						}
						poses_pending = false;
						for (int i = 0; i < app_count; i++) {
							if (apps[i].has_pose) poses_pending = true;
						}

						// Clear client name tracking
						client_name_count = 0;

						g_shell_active = false;
						g_launcher_visible = false;
						tray_update_tooltip(false);
						P("Shell deactivated — waiting in tray.\n");
					} else {
						P("Activating shell...\n");
						xret = ipc_call_shell_activate(&ipc_c);

						// If IPC pipe is dead (service exited), reconnect.
						// The IPC client lib auto-starts the service.
						if (xret != XRT_SUCCESS) {
							P("Reconnecting to service...\n");
							ipc_client_connection_fini(&ipc_c);
							memset(&ipc_c, 0, sizeof(ipc_c));
							for (int attempt = 0; attempt < 10; attempt++) {
								xret = ipc_client_connection_init(
								    &ipc_c, U_LOGGING_WARN, &info);
								if (xret == XRT_SUCCESS) break;
								Sleep(1000);
							}
							if (xret == XRT_SUCCESS) {
								xret = ipc_call_shell_activate(&ipc_c);
								service_pid = find_service_pid();
							}
							if (xret != XRT_SUCCESS) {
								PE("Failed to reconnect to service.\n");
								continue;
							}
						}

						// Already-running IPC apps (OpenXR handle apps) are
						// automatically picked up by the multi-comp on their
						// next layer_commit. No need to enumerate 2D windows.

						g_shell_active = true;
						tray_update_tooltip(true);
						P("Shell activated.\n");
					}
				} else if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_LAUNCH) {
					// --- Ctrl+L: Toggle spatial launcher panel (Phase 5.7) ---
					// The Phase 4C MessageBox path is gone — the service-side
					// multi-compositor renders the panel in its own window when
					// launcher_visible is true. Tile grid + launch dispatch
					// come in later Phase 5 tasks.
					if (g_shell_active) {
						g_launcher_visible = !g_launcher_visible;

						// Phase 5.12: when opening, hand the service process
						// foreground-activation permission so it can pull its
						// compositor window into focus. Otherwise keys like
						// Esc stay routed to whichever app previously had
						// focus and never reach the compositor WndProc that
						// the launcher keyboard handler depends on.
						if (g_launcher_visible && service_pid != 0) {
							AllowSetForegroundWindow(service_pid);
						}

						xrt_result_t lret = ipc_call_shell_set_launcher_visible(
						    &ipc_c, g_launcher_visible);
						if (lret != XRT_SUCCESS) {
							PE("ipc_call_shell_set_launcher_visible failed: %d\n", lret);
							g_launcher_visible = !g_launcher_visible; // roll back
						} else {
							P("Launcher %s\n", g_launcher_visible ? "shown" : "hidden");
						}
					}
				} else if (msg.message == WM_TRAYICON) {
					if (LOWORD(msg.lParam) == WM_LBUTTONUP) {
						// Left-click tray: activate if inactive
						if (!g_shell_active) {
							PostMessage(g_msg_hwnd, WM_HOTKEY, HOTKEY_TOGGLE, 0);
						}
					} else if (LOWORD(msg.lParam) == WM_RBUTTONUP) {
						// Right-click tray: context menu
						tray_show_context_menu(g_msg_hwnd, g_shell_active);
					}
				}
				TranslateMessage(&msg);
				DispatchMessageA(&msg);
			}
		}
		// Fall through to poll work regardless of wait result
#else
		usleep(500000);
#endif

		// --- Poll work (only meaningful when shell is active) ---
#ifdef _WIN32
		if (!g_shell_active) {
			continue;
		}
#endif

		// Apply pending poses when new clients appear
		if (poses_pending) {
			try_apply_poses(&ipc_c, apps, app_count, prev_ids, prev_count);

			poses_pending = false;
			for (int i = 0; i < app_count; i++) {
				if (apps[i].has_pose && !apps[i].pose_applied) {
					poses_pending = true;
				}
			}
		}

		// Detect new clients and track names
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
							int instance = 1;
							for (int cn = 0; cn < client_name_count; cn++) {
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

							if (client_name_count < MAX_SAVED_WINDOWS) {
								client_names[client_name_count].id = clients.ids[c];
								snprintf(client_names[client_name_count].name, 128, "%s", numbered_name);
								client_name_count++;
							}
						}
					}
				}
			}
		}

		print_clients(&ipc_c, prev_ids, &prev_count);

#ifdef _WIN32
		// Detect server-side deactivation (ESC closed the compositor window).
		// The compositor sets shell_mode=false — sync our state.
		if (g_shell_active) {
			bool server_active = false;
			if (ipc_call_shell_get_state(&ipc_c, &server_active) == XRT_SUCCESS) {
				if (!server_active) {
					P("Shell deactivated by compositor (ESC) — returning to tray.\n");
					capture_count = 0;
					client_name_count = 0;
					g_shell_active = false;
					g_launcher_visible = false;
					tray_update_tooltip(false);
				}
			}
		}

		// Phase 5.11: refresh the running-tile mask and push to service if
		// it has changed. Cheap to compute (one IPC list call + per-PID
		// QueryFullProcessImageNameW), and we only push on diff.
		if (g_shell_active) {
			static uint64_t s_last_running_mask = (uint64_t)-1;
			uint64_t mask = shell_compute_running_tile_mask(&ipc_c);
			if (mask != s_last_running_mask) {
				ipc_call_shell_set_running_tile_mask(&ipc_c, mask);
				s_last_running_mask = mask;
			}
		}

		// Phase 5.10: poll for launcher tile clicks. The service-side
		// WM_LBUTTONDOWN handler stores a tile index when the user clicks
		// inside the launcher; we look up the registered app and dispatch
		// the launch on this side because we own the env vars + CreateProcess
		// path. The launcher panel was already hidden by the service when
		// the click landed, so we just need to sync our local state and run
		// the launch. Only poll while the launcher is actually visible to
		// keep IPC traffic minimal.
		// Poll for launcher tile clicks. Only while the launcher is visible
		// — when hidden there can't be any clicks. This is a performance
		// optimization (avoids 500ms-cadence IPC traffic when the launcher
		// isn't open), not a workaround for #144 which turned out to be a
		// stale-binary issue resolved by the Phase 6 rebuild.
		if (g_shell_active && g_launcher_visible) {
			int64_t tile_index = -1;
			if (ipc_call_shell_poll_launcher_click(&ipc_c, &tile_index) == XRT_SUCCESS &&
			    tile_index != -1) {
				// Service already hid the launcher when the click registered.
				g_launcher_visible = false;

				if (tile_index == IPC_LAUNCHER_ACTION_BROWSE) {
					// Phase 5.14: Browse tile → open file dialog, add to
					// registry, re-push, re-show launcher so the user sees
					// their new tile.
					shell_browse_and_add_app(&ipc_c);
#ifdef _WIN32
					if (service_pid != 0) {
						AllowSetForegroundWindow(service_pid);
					}
#endif
					if (ipc_call_shell_set_launcher_visible(&ipc_c, true) == XRT_SUCCESS) {
						g_launcher_visible = true;
					}
				} else if (tile_index >= 0 && tile_index < (int64_t)g_registered_app_count) {
					struct registered_app *rapp = &g_registered_apps[tile_index];

					// Phase 5.11: if a matching client is already running,
					// focus it instead of spawning a second instance.
					bool focused_existing = false;
#ifdef _WIN32
					struct ipc_client_list clist;
					if (ipc_call_system_get_clients(&ipc_c, &clist) == XRT_SUCCESS) {
						for (uint32_t c = 0; c < clist.id_count && !focused_existing; c++) {
							if (clist.ids[c] == 0) continue;
							struct ipc_app_state ias;
							if (ipc_call_system_get_client_info(&ipc_c, clist.ids[c], &ias) != XRT_SUCCESS)
								continue;
							char client_exe[MAX_PATH];
							if (!shell_pid_to_exe_path((DWORD)ias.pid, client_exe, sizeof(client_exe)))
								continue;
							if (shell_exe_paths_equal(client_exe, rapp->exe_path)) {
								if (ipc_call_system_set_focused_client(&ipc_c, clist.ids[c]) == XRT_SUCCESS) {
									P("Launcher: focused running client %u → '%s'\n",
									  clist.ids[c], rapp->name);
									focused_existing = true;
								}
							}
						}
					}
#endif
					if (!focused_existing) {
						P("Launcher: launching tile %lld → '%s'\n",
						  (long long)tile_index, rapp->name);
						shell_launch_registered_app(
						    &ipc_c, rapp,
						    have_json ? runtime_json : NULL,
						    apps, &app_count,
						    captures, &capture_count);
					}
				} else {
					PE("Launcher click: tile %lld out of range (count=%d)\n",
					   (long long)tile_index, g_registered_app_count);
				}
			}
		}

		// Dynamic window tracking: detect new/closed windows every ~1 second
		if (auto_adopt) {
			adopt_counter++;
			if (adopt_counter >= 2) {
				adopt_counter = 0;
				cleanup_closed_captures(&ipc_c, captures, &capture_count);
				enumerate_and_adopt_windows(&ipc_c, captures, &capture_count, service_pid);
			}
		}
#endif
	}

	P("\nShell exiting.\n");

#ifdef _WIN32
	// Deactivate shell if still active
	if (g_shell_active) {
		ipc_call_shell_deactivate(&ipc_c);
	}

	// Cleanup hotkey and tray
	if (g_msg_hwnd != NULL) {
		UnregisterHotKey(g_msg_hwnd, HOTKEY_TOGGLE);
		UnregisterHotKey(g_msg_hwnd, HOTKEY_LAUNCH);
		tray_destroy();
		DestroyWindow(g_msg_hwnd);
	}

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
