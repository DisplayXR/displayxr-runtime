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
 * - Launches each app with DISPLAYXR_WORKSPACE_SESSION=1 and XR_RUNTIME_JSON set
 * - Optionally assigns per-app window pose via --pose x,y,z,width_m,height_m
 * - Monitors client connect/disconnect until Ctrl+C
 *
 * @ingroup ipc
 */

#include "xrt/xrt_results.h"
#include "xrt/xrt_defines.h"
#include "util/u_logging.h"

#include "shell_app_scan.h"
#include "shell_openxr.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <tlhelp32.h> // For process enumeration (service PID lookup)
#include <shellapi.h> // For Shell_NotifyIcon (system tray); PrivateExtractIconsA (Browse PE-icon extract)
#include <commdlg.h>  // For GetOpenFileNameA (Phase 5.14 Browse tile)

#include "displayxr_shell_resource.h" // IDI_DISPLAYXR_SHELL — embedded tray/PE icon

// PNG encoding for the Browse-for-app PE icon → tile-icon conversion. Defined
// here (not in shell_app_scan.c) because main.c owns the Browse flow and this
// keeps the dependency in one TU. STB_IMAGE_WRITE_IMPLEMENTATION must be
// defined in exactly one source file per binary; the displayxr-service binary
// has its own definition in comp_d3d11_service.cpp.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#else
#include <unistd.h>
#endif


#define P(...) fprintf(stdout, __VA_ARGS__)
#define PE(...) fprintf(stderr, __VA_ARGS__)

#ifdef _WIN32
#define HOTKEY_TOGGLE 1
#define HOTKEY_LAUNCH 2
#define HOTKEY_CAPTURE 3
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
static bool g_service_managed = false; // When true, skip Ctrl+Space hotkey (service owns it)
static bool g_launcher_visible = false; // Phase 5.7: spatial launcher panel toggle (Ctrl+L)
static HWND g_msg_hwnd = NULL;
#endif

// Phase 2.I: file-scope OpenXR state. Set in main() after shell_openxr_init
// succeeds. Helpers throughout the shell dispatch through this pointer; once
// non-NULL, the corresponding ipc_call_workspace_* / launcher_* / system_*
// sites have been migrated to the public extension surface.
static struct shell_openxr_state *g_xr = NULL;

// Phase 2.I: convert struct xrt_pose → XrPosef. The two layouts are
// field-compatible (quat + vec3) but live in different headers; the runtime
// already proves this with a field-by-field translation in oxr_workspace.c.
static inline void
xrt_pose_to_xr_posef(const struct xrt_pose *in, XrPosef *out)
{
	out->orientation.x = in->orientation.x;
	out->orientation.y = in->orientation.y;
	out->orientation.z = in->orientation.z;
	out->orientation.w = in->orientation.w;
	out->position.x = in->position.x;
	out->position.y = in->position.y;
	out->position.z = in->position.z;
}

// Phase 2.I C10: cap on locally-buffered enumerate. IPC_MAX_CLIENTS is 8 in
// the wire format (struct ipc_client_list); 16 keeps headroom for a future
// raise without touching the shell.
#define SHELL_MAX_CLIENTS 16

// Drain the workspace client id list from the runtime. Returns the number
// of ids written; 0 on error or empty.
static uint32_t
shell_enumerate_clients(XrWorkspaceClientId *out_ids, uint32_t cap)
{
	if (g_xr == NULL || out_ids == NULL || cap == 0) {
		return 0;
	}
	uint32_t count = 0;
	if (g_xr->enumerate_clients(g_xr->session, cap, &count, out_ids) != XR_SUCCESS) {
		return 0;
	}
	return count;
}

// Fetch metadata for one client. Returns true on success; @p info is
// initialized with the required type + next NULL.
static bool
shell_get_client_info(XrWorkspaceClientId id, XrWorkspaceClientInfoEXT *info)
{
	if (g_xr == NULL || info == NULL) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->type = XR_TYPE_WORKSPACE_CLIENT_INFO_EXT;
	return g_xr->get_client_info(g_xr->session, id, info) == XR_SUCCESS;
}

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
	char manifest_path[MAX_PATH];  // .displayxr.json path (so launcher remove can delete it); "" for legacy entries
	char type[8];          // "3d", "2d", or "" (unknown)
	char category[32];     // manifest "category", default "app"
	char description[256]; // manifest "description"
	char display_mode[16]; // manifest "display_mode", default "auto"
	char icon_path[MAX_PATH];     // resolved 2D icon (absolute) or ""
	char icon_3d_path[MAX_PATH];  // resolved 3D icon (absolute) or ""
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

// Append a scanned app into the in-memory registry. The scanner already
// dedups by exe_path, so we just append. Caller is responsible for clearing
// g_registered_apps[] before the scan run.
static void
append_scanned_app(const struct shell_scanned_app *s)
{
	if (g_registered_app_count >= MAX_REGISTERED_APPS) {
		PE("registry full — dropping scanned app '%s'\n", s->name);
		return;
	}
	struct registered_app *app = &g_registered_apps[g_registered_app_count++];
	registered_app_zero(app);
	snprintf(app->name, sizeof(app->name), "%s", s->name);
	snprintf(app->exe_path, sizeof(app->exe_path), "%s", s->exe_path);
	snprintf(app->manifest_path, sizeof(app->manifest_path), "%s", s->manifest_path);
	snprintf(app->type, sizeof(app->type), "%s", s->type);
	snprintf(app->category, sizeof(app->category), "%s", s->category);
	snprintf(app->description, sizeof(app->description), "%s", s->description);
	snprintf(app->display_mode, sizeof(app->display_mode), "%s", s->display_mode);
	snprintf(app->icon_path, sizeof(app->icon_path), "%s", s->icon_path);
	snprintf(app->icon_3d_path, sizeof(app->icon_3d_path), "%s", s->icon_3d_path);
	snprintf(app->icon_3d_layout, sizeof(app->icon_3d_layout), "%s", s->icon_3d_layout);
}

// Forward declarations — these are called from registered_apps_load but
// defined later in this file.
static void
registered_apps_save(void);
#ifdef _WIN32
static void
get_exe_dir(char *buf, size_t buf_size);
#endif

// Rebuild the in-memory registry from the manifest scanner. Manifests
// (sidecar + registered) are now the sole source of truth — registered_apps.json
// is just a debug snapshot of the last scan, not a separate input. See
// docs/specs/displayxr-app-manifest.md §11.
static void
registered_apps_load(void)
{
	g_registered_app_count = 0;

	char path[512];
	get_registered_apps_path(path, sizeof(path));

#ifdef _WIN32
	{
		// Make sure the parent directory exists before we try to write the
		// debug snapshot.
		char dir[512];
		snprintf(dir, sizeof(dir), "%s", path);
		char *last = strrchr(dir, '\\');
		if (last) { *last = '\0'; CreateDirectoryA(dir, NULL); }
	}

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
			append_scanned_app(&scanned[i]);
		}
	}
#endif

	P("Registry: %d app(s) after scan.\n", g_registered_app_count);
	for (int i = 0; i < g_registered_app_count; i++) {
		P("  %s  (%s)\n", g_registered_apps[i].name, g_registered_apps[i].exe_path);
	}

	// Persist a snapshot of the current scan for debugging / inspection.
	// Not a source of truth — the next load() rebuilds entirely from manifests.
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
		if (app->manifest_path[0])  cJSON_AddStringToObject(obj, "manifest_path", app->manifest_path);
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

// Write everything before the last path separator of @p path into @p out.
// If there's no separator, @p out is set to "." (current dir). Used to derive
// lpCurrentDirectory for CreateProcessA so launched apps land with their
// install dir as CWD, matching Explorer-launched behavior.
static void
dirname_of(const char *path, char *out, size_t out_size)
{
	if (out_size == 0) return;
	out[0] = '\0';
	if (path == NULL || !*path) {
		snprintf(out, out_size, ".");
		return;
	}

	const char *last_bs = strrchr(path, '\\');
	const char *last_fs = strrchr(path, '/');
	const char *last = last_bs > last_fs ? last_bs : last_fs;
	if (last == NULL) {
		snprintf(out, out_size, ".");
		return;
	}

	size_t len = (size_t)(last - path);
	if (len >= out_size) len = out_size - 1;
	memcpy(out, path, len);
	out[len] = '\0';
}

// Ensure the Windows Per-App Graphics Settings registry has a "high
// performance" preference for @p exe_path, matching what Settings → System →
// Display → Graphics writes. DXGI honours this at process start, which puts
// the launched app on the same adapter as the (dGPU-pinned) service. Without
// it, third-party apps that don't carry NvOptimusEnablement land on the iGPU
// on hybrid laptops, and the IPC shared swapchain texture renders black on
// the service side because shared textures can't bridge two adapters.
//
// Idempotent. Only writes if the entry is missing or empty so a user-set
// "Power saving" preference is respected.
//
// Reference: docs.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-1-6-improvements
static void
ensure_app_gpu_pref_high(const char *exe_path)
{
	const char *kSubkey = "Software\\Microsoft\\DirectX\\UserGpuPreferences";
	HKEY hkey = NULL;
	DWORD disposition = 0;
	LSTATUS rc = RegCreateKeyExA(HKEY_CURRENT_USER, kSubkey, 0, NULL,
	                             REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
	                             NULL, &hkey, &disposition);
	if (rc != ERROR_SUCCESS || hkey == NULL) {
		PE("ensure_app_gpu_pref_high: RegCreateKeyEx failed (%ld) for %s\n",
		   (long)rc, exe_path);
		return;
	}

	// Read existing value first — respect any pre-existing preference, even
	// "power saving". Only write when missing/empty.
	char existing[256] = {0};
	DWORD existing_size = sizeof(existing);
	DWORD existing_type = 0;
	rc = RegQueryValueExA(hkey, exe_path, NULL, &existing_type,
	                      (LPBYTE)existing, &existing_size);
	bool already_set = (rc == ERROR_SUCCESS && existing_type == REG_SZ &&
	                    existing_size > 1);
	if (!already_set) {
		const char *value = "GpuPreference=2;"; // 2 = high-performance / dGPU
		rc = RegSetValueExA(hkey, exe_path, 0, REG_SZ, (const BYTE *)value,
		                    (DWORD)strlen(value) + 1);
		if (rc == ERROR_SUCCESS) {
			P("Registered high-perf GPU preference for %s\n", exe_path);
		} else {
			PE("ensure_app_gpu_pref_high: RegSetValueEx failed (%ld) for %s\n",
			   (long)rc, exe_path);
		}
	}

	RegCloseKey(hkey);
}

/*!
 * Launch an app with DISPLAYXR_WORKSPACE_SESSION=1 and XR_RUNTIME_JSON set.
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
	SetEnvironmentVariableA("DISPLAYXR_WORKSPACE_SESSION", "1");

	// Resolve to absolute path (relative paths fail with CreateProcessA)
	char abs_path[MAX_PATH];
	if (_fullpath(abs_path, app->exe_path, MAX_PATH) == NULL) {
		PE("Failed to resolve path: %s\n", app->exe_path);
		return false;
	}

	// Force the launched app onto the same adapter as the dGPU-pinned
	// service so the IPC shared swapchain texture works on hybrid laptops.
	// See ensure_app_gpu_pref_high for why this matters.
	ensure_app_gpu_pref_high(abs_path);

	// CWD = exe's install dir (matches Explorer behavior). DisplayXRClient.dll
	// drops displayxr.log relative to CWD; without this it would land
	// wherever the shell happened to be launched from.
	char exe_dir[MAX_PATH];
	dirname_of(abs_path, exe_dir, sizeof(exe_dir));

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
	    exe_dir, &si, &pi);

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
		if (strcmp(argv[i], "--service-managed") == 0) {
			g_service_managed = true;
			continue;
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
try_apply_poses(struct app_entry *apps, int app_count,
                uint32_t *prev_ids, uint32_t prev_count)
{
	// Get current client list
	XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
	uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
	if (client_count == 0) {
		return;
	}

	// Find new clients (IDs not in prev_ids)
	for (uint32_t i = 0; i < client_count; i++) {
		uint32_t id = client_ids[i];
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

				XrPosef xrpose;
				xrt_pose_to_xr_posef(&pose, &xrpose);
				XrResult r = g_xr->set_pose(
				    g_xr->session, id, &xrpose,
				    apps[a].width_m, apps[a].height_m);
				if (r == XR_SUCCESS) {
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
print_clients(uint32_t *prev_ids, uint32_t *prev_count)
{
	XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
	uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
	if (client_count == 0 && *prev_count == 0) {
		return;
	}

	// Detect changes
	bool changed = (client_count != *prev_count);
	if (!changed) {
		for (uint32_t i = 0; i < client_count; i++) {
			if (client_ids[i] != prev_ids[i]) {
				changed = true;
				break;
			}
		}
	}

	if (!changed) {
		return;
	}

	// Print current state
	P("\n--- %u client(s) connected ---\n", client_count);
	for (uint32_t i = 0; i < client_count; i++) {
		XrWorkspaceClientInfoEXT cinfo;
		if (!shell_get_client_info(client_ids[i], &cinfo)) {
			P("  [%u] (failed to get info)\n", (unsigned)client_ids[i]);
			continue;
		}
		P("  [%u] %s (PID %llu)\n", (unsigned)client_ids[i], cinfo.name,
		  (unsigned long long)cinfo.pid);
	}

	// Update previous state
	*prev_count = client_count;
	for (uint32_t i = 0; i < client_count; i++) {
		prev_ids[i] = client_ids[i];
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
enumerate_and_adopt_windows(struct capture_entry *captures,
                            int *capture_count,
                            DWORD service_pid)
{
	// Enumerate visible top-level windows
	struct enum_ctx ctx = {0};
	ctx.shell_pid = GetCurrentProcessId();
	ctx.service_pid = service_pid;
	EnumWindows(enum_windows_cb, (LPARAM)&ctx);

	// Get current OpenXR client PIDs to skip already-attached 3D apps.
	DWORD ipc_pids[SHELL_MAX_CLIENTS] = {0};
	int ipc_pid_count = 0;
	{
		XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
		uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
		for (uint32_t c = 0; c < client_count; c++) {
			if (client_ids[c] == XR_NULL_WORKSPACE_CLIENT_ID) continue;
			XrWorkspaceClientInfoEXT cinfo;
			if (shell_get_client_info(client_ids[c], &cinfo)) {
				ipc_pids[ipc_pid_count++] = (DWORD)cinfo.pid;
				if (ipc_pid_count >= SHELL_MAX_CLIENTS) break;
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
		xrt_result_t r = (xrt_result_t)g_xr->add_capture(g_xr->session, ce->hwnd, NULL, &cid);
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
cleanup_closed_captures(struct capture_entry *captures,
                        int *capture_count)
{
	for (int i = 0; i < *capture_count; i++) {
		if (!captures[i].added) continue;
		if (!IsWindow((HWND)(uintptr_t)captures[i].hwnd)) {
			P("  Window closed: '%s' (client_id=%u)\n", captures[i].name, captures[i].client_id);
			(void)g_xr->remove_capture(g_xr->session, captures[i].client_id);
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
 * Snapshot the set of application_name strings for every workspace client
 * currently connected. Used by the launcher to highlight tiles whose app is
 * already running. Cheap enough to call every time the launcher opens; not
 * intended for per-frame use.
 *
 * On error, returns an empty set — the launcher should degrade to "no running
 * apps" rather than failing to open.
 */
static void
shell_get_running_app_set(struct shell_running_set *out)
{
	out->count = 0;

	XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
	uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
	if (client_count == 0) {
		return;
	}

	for (uint32_t c = 0; c < client_count; c++) {
		if (client_ids[c] == XR_NULL_WORKSPACE_CLIENT_ID) continue;
		if (out->count >= SHELL_RUNNING_NAMES_MAX) break;

		XrWorkspaceClientInfoEXT cinfo;
		if (!shell_get_client_info(client_ids[c], &cinfo)) continue;
		if (cinfo.name[0] == '\0') continue;

		// Deduplicate — two instances of the same app are one tile in the
		// launcher (for highlight purposes).
		bool already = false;
		for (int i = 0; i < out->count; i++) {
			if (strcmp(out->names[i], cinfo.name) == 0) {
				already = true;
				break;
			}
		}
		if (already) continue;

		snprintf(out->names[out->count], sizeof(out->names[0]), "%s",
		         cinfo.name);
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
shell_compute_running_tile_mask(void)
{
#ifdef _WIN32
	uint64_t mask = 0;

	XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
	uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
	if (client_count == 0) {
		return 0;
	}

	// Collect each connected client's exe path once.
	char client_exes[SHELL_MAX_CLIENTS][MAX_PATH];
	int client_exe_count = 0;
	for (uint32_t c = 0; c < client_count; c++) {
		if (client_ids[c] == XR_NULL_WORKSPACE_CLIENT_ID) continue;
		XrWorkspaceClientInfoEXT cinfo;
		if (!shell_get_client_info(client_ids[c], &cinfo)) continue;
		if (cinfo.pid == 0) continue;
		if (shell_pid_to_exe_path((DWORD)cinfo.pid, client_exes[client_exe_count],
		                          sizeof(client_exes[0]))) {
			client_exe_count++;
			if (client_exe_count >= SHELL_MAX_CLIENTS) break;
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
shell_push_registered_apps_to_service(void)
{
	XrResult r = g_xr->clear_launcher(g_xr->session);
	if (r != XR_SUCCESS) {
		PE("xrClearLauncherAppsEXT failed: %d\n", r);
		return;
	}

	int pushed = 0;
	int cap = g_registered_app_count;
	if (cap > XR_LAUNCHER_MAX_APPS_EXT) {
		cap = XR_LAUNCHER_MAX_APPS_EXT;
	}
	for (int i = 0; i < cap; i++) {
		const struct registered_app *src = &g_registered_apps[i];

		XrLauncherAppInfoEXT info;
		memset(&info, 0, sizeof(info));
		info.type = XR_TYPE_LAUNCHER_APP_INFO_EXT;
		snprintf(info.name, sizeof(info.name), "%s", src->name);
		snprintf(info.iconPath, sizeof(info.iconPath), "%s", src->icon_path);
		snprintf(info.appType, sizeof(info.appType), "%s", src->type);
		snprintf(info.iconPath3D, sizeof(info.iconPath3D), "%s", src->icon_3d_path);
		snprintf(info.iconLayout3D, sizeof(info.iconLayout3D), "%s", src->icon_3d_layout);
		info.appIndex = i;

		r = g_xr->add_launcher_app(g_xr->session, &info);
		if (r != XR_SUCCESS) {
			PE("xrAddLauncherAppEXT[%d] failed: %d\n", i, r);
			return;
		}
		pushed++;
	}
	P("Pushed %d app(s) to launcher.\n", pushed);
}

#ifdef _WIN32

// Get the per-user registered-mode discovery dir, e.g.
// "C:\Users\foo\AppData\Local\DisplayXR\apps". Creates intermediate dirs.
// Returns true on success.
static bool
get_registered_apps_dir(char *out, size_t out_size)
{
	const char *local_appdata = getenv("LOCALAPPDATA");
	if (local_appdata == NULL || !*local_appdata) return false;

	char base[MAX_PATH];
	snprintf(base, sizeof(base), "%s\\DisplayXR", local_appdata);
	CreateDirectoryA(base, NULL);

	snprintf(out, out_size, "%s\\apps", base);
	CreateDirectoryA(out, NULL);
	return true;
}

// Replace path separators and any non-portable characters with '_'. Used to
// derive a manifest filename from an exe basename.
static void
sanitize_basename(const char *src, char *dst, size_t dst_size)
{
	size_t i = 0;
	for (; src[i] && i + 1 < dst_size; i++) {
		char c = src[i];
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		dst[i] = ok ? c : '_';
	}
	dst[i] = '\0';
}

// Extract the largest available app icon from @p exe and write it as PNG to
// @p out_png. Uses PrivateExtractIconsA to request the largest standard size
// (256x256), then DrawIconEx onto a 32-bit DIB so PNG-compressed Vista-era
// icons render correctly. Returns true on success.
static bool
extract_pe_icon_to_png(const char *exe, const char *out_png)
{
	const int target = 256;
	HICON hicon = NULL;
	UINT extracted = PrivateExtractIconsA(exe, 0, target, target, &hicon, NULL, 1, 0);
	if (extracted == 0 || extracted == (UINT)-1 || hicon == NULL) {
		// Fall back to the system "large icon" size (32x32 on classic
		// Windows). Better than nothing.
		extracted = PrivateExtractIconsA(exe, 0, GetSystemMetrics(SM_CXICON),
		                                 GetSystemMetrics(SM_CYICON), &hicon,
		                                 NULL, 1, 0);
		if (extracted == 0 || extracted == (UINT)-1 || hicon == NULL) {
			return false;
		}
	}

	// Determine icon dimensions from the actual HICON (PrivateExtractIcons
	// may have given us the largest available size, not 256).
	ICONINFO ii = {0};
	if (!GetIconInfo(hicon, &ii)) {
		DestroyIcon(hicon);
		return false;
	}
	BITMAP bm = {0};
	int w = target, h = target;
	if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm)) {
		w = bm.bmWidth;
		h = bm.bmHeight;
	}
	if (ii.hbmMask) DeleteObject(ii.hbmMask);
	if (ii.hbmColor) DeleteObject(ii.hbmColor);

	// Render the icon onto a 32bpp top-down DIB. DrawIconEx handles alpha
	// for both Vista PNG icons and classic AND/XOR-mask icons.
	BITMAPINFO bi = {0};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;     // negative → top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	HDC screen_dc = GetDC(NULL);
	HDC mem_dc = CreateCompatibleDC(screen_dc);
	void *bits = NULL;
	HBITMAP dib = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	ReleaseDC(NULL, screen_dc);

	if (dib == NULL || bits == NULL) {
		if (dib) DeleteObject(dib);
		DeleteDC(mem_dc);
		DestroyIcon(hicon);
		return false;
	}

	HGDIOBJ prev = SelectObject(mem_dc, dib);
	memset(bits, 0, (size_t)w * (size_t)h * 4);  // transparent background
	BOOL drew = DrawIconEx(mem_dc, 0, 0, hicon, w, h, 0, NULL, DI_NORMAL);
	SelectObject(mem_dc, prev);
	DeleteDC(mem_dc);
	DestroyIcon(hicon);

	if (!drew) {
		DeleteObject(dib);
		return false;
	}

	// DIB layout is BGRA; stb expects RGBA, so swap channels in place.
	uint8_t *px = (uint8_t *)bits;
	for (int i = 0; i < w * h; i++) {
		uint8_t b = px[i * 4 + 0];
		px[i * 4 + 0] = px[i * 4 + 2];
		px[i * 4 + 2] = b;
	}

	int ok = stbi_write_png(out_png, w, h, 4, bits, w * 4);
	DeleteObject(dib);
	return ok != 0;
}

/*!
 * Browse-for-app: prompt for an executable, write a registered-mode manifest
 * (`<sanitized>.displayxr.json` with `exe_path`) into
 * `%LOCALAPPDATA%\DisplayXR\apps\`, extract the embedded PE icon as a sibling
 * `<sanitized>.png`, and re-trigger discovery so the new tile shows up. See
 * docs/specs/displayxr-app-manifest.md §11.
 *
 * Called from the launcher click-poll branch when the service signals a
 * Browse-tile hit (IPC_LAUNCHER_ACTION_BROWSE). The file dialog is modal and
 * blocks the shell's message loop; on return the launcher is re-shown so the
 * user sees their new tile.
 */
static void
shell_browse_and_add_app(void)
{
	// Open the file dialog as a top-level window (hwndOwner=NULL). The
	// service granted ASFW_ANY foreground permission when it processed the
	// Browse click, so Windows lets the modal dialog activate normally.
	char exe_path[MAX_PATH] = {0};
	OPENFILENAMEA ofn = {0};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = NULL;
	ofn.lpstrFilter = "Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
	ofn.lpstrFile = exe_path;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrTitle = "Add app to DisplayXR launcher";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

	if (!GetOpenFileNameA(&ofn)) {
		P("Browse: canceled\n");
		return;
	}

	// Derive a display name from the exe basename (strip path and .exe).
	const char *base = strrchr(exe_path, '\\');
	if (base == NULL) base = strrchr(exe_path, '/');
	base = base ? base + 1 : exe_path;
	char name[128];
	snprintf(name, sizeof(name), "%s", base);
	size_t nlen = strlen(name);
	if (nlen >= 4 && _stricmp(name + nlen - 4, ".exe") == 0) {
		name[nlen - 4] = '\0';
	}

	char sanitized[128];
	sanitize_basename(name, sanitized, sizeof(sanitized));

	char dir[MAX_PATH];
	if (!get_registered_apps_dir(dir, sizeof(dir))) {
		PE("Browse: cannot resolve %%LOCALAPPDATA%%\\DisplayXR\\apps\n");
		return;
	}

	char manifest_path[MAX_PATH];
	snprintf(manifest_path, sizeof(manifest_path), "%s\\%s.displayxr.json", dir,
	         sanitized);
	char icon_path[MAX_PATH];
	snprintf(icon_path, sizeof(icon_path), "%s\\%s.png", dir, sanitized);

	bool have_icon = extract_pe_icon_to_png(exe_path, icon_path);
	if (!have_icon) {
		P("Browse: PE icon extraction failed for %s — manifest will be iconless\n",
		  exe_path);
	}

	// Build the manifest JSON via cJSON for proper escaping of paths
	// containing backslashes, spaces, etc.
	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "schema_version", 1);
	cJSON_AddStringToObject(root, "name", name);
	cJSON_AddStringToObject(root, "type", "3d");
	cJSON_AddStringToObject(root, "category", "app");
	cJSON_AddStringToObject(root, "exe_path", exe_path);
	if (have_icon) {
		// Icon path is relative to the manifest, per §2.3.
		char icon_rel[256];
		snprintf(icon_rel, sizeof(icon_rel), "%s.png", sanitized);
		cJSON_AddStringToObject(root, "icon", icon_rel);
	}

	char *json_str = cJSON_Print(root);
	cJSON_Delete(root);
	if (json_str == NULL) {
		PE("Browse: cJSON_Print failed\n");
		return;
	}

	FILE *f = fopen(manifest_path, "wb");
	if (f == NULL) {
		PE("Browse: cannot write manifest %s\n", manifest_path);
		free(json_str);
		return;
	}
	fwrite(json_str, 1, strlen(json_str), f);
	fclose(f);
	free(json_str);

	P("Browse: wrote manifest %s\n", manifest_path);

	// Re-scan + re-push so the new tile appears.
	registered_apps_load();
	shell_push_registered_apps_to_service();
}
#else
static void
shell_browse_and_add_app(void)
{
}
#endif

// --- 4C.10+4C.11: App launch from shell + auto-detect type ---

/*!
 * Launch a registered app from the shell.
 * For "3d" type: uses launch_app() with DISPLAYXR_WORKSPACE_SESSION env.
 * For "2d" type: launches without shell env, then captures via IPC.
 * For unknown type: launches as 3d, polls for IPC connect to auto-detect.
 */
static void
shell_launch_registered_app(struct registered_app *rapp,
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

		// 2D apps don't share textures, but pin them to the same adapter
		// anyway for consistency with 3D apps and to keep DXGI happy.
		ensure_app_gpu_pref_high(abs_path);

		char exe_dir[MAX_PATH];
		dirname_of(abs_path, exe_dir, sizeof(exe_dir));

		// Clear shell env vars so 2D app doesn't accidentally pick them up
		SetEnvironmentVariableA("DISPLAYXR_WORKSPACE_SESSION", NULL);

		BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
		    CREATE_NEW_CONSOLE, NULL, exe_dir, &si, &pi);
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
			xrt_result_t r = (xrt_result_t)g_xr->add_capture(g_xr->session, ce->hwnd, NULL, &cid);
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
			XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
			uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
			for (uint32_t c = 0; c < client_count; c++) {
				XrWorkspaceClientInfoEXT cinfo;
				if (shell_get_client_info(client_ids[c], &cinfo)) {
					if ((DWORD)cinfo.pid == a->pid) {
						found_ipc = true;
						break;
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
	g_nid.hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDI_DISPLAYXR_SHELL));
	if (g_nid.hIcon == NULL) {
		g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	}
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
/*
 * Phase 8: 3D capture MVP.
 *
 * Builds an output prefix under %USERPROFILE%\Pictures\DisplayXR, asks the
 * runtime to write SBS / L / R PNGs of the current pre-weave atlas, then
 * writes a JSON sidecar with the metadata returned by the runtime.
 */
static void
capture_write_sidecar(const char *json_path, const XrWorkspaceCaptureResultEXT *r)
{
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}

	cJSON_AddNumberToObject(root, "schema_version", 1);
	cJSON_AddNumberToObject(root, "timestamp_ns", (double)r->timestampNs);

	cJSON *atlas = cJSON_AddObjectToObject(root, "atlas");
	cJSON_AddNumberToObject(atlas, "width", r->atlasWidth);
	cJSON_AddNumberToObject(atlas, "height", r->atlasHeight);

	cJSON *eye = cJSON_AddObjectToObject(root, "eye");
	cJSON_AddNumberToObject(eye, "width", r->eyeWidth);
	cJSON_AddNumberToObject(eye, "height", r->eyeHeight);

	cJSON *stereo = cJSON_AddObjectToObject(root, "stereo");
	cJSON_AddNumberToObject(stereo, "tile_columns", r->tileColumns);
	cJSON_AddNumberToObject(stereo, "tile_rows", r->tileRows);

	cJSON *disp = cJSON_AddObjectToObject(root, "display_m");
	cJSON_AddNumberToObject(disp, "width", r->displayWidthM);
	cJSON_AddNumberToObject(disp, "height", r->displayHeightM);

	cJSON *le = cJSON_AddArrayToObject(root, "eye_left_m");
	cJSON_AddItemToArray(le, cJSON_CreateNumber(r->eyeLeftM[0]));
	cJSON_AddItemToArray(le, cJSON_CreateNumber(r->eyeLeftM[1]));
	cJSON_AddItemToArray(le, cJSON_CreateNumber(r->eyeLeftM[2]));

	cJSON *re = cJSON_AddArrayToObject(root, "eye_right_m");
	cJSON_AddItemToArray(re, cJSON_CreateNumber(r->eyeRightM[0]));
	cJSON_AddItemToArray(re, cJSON_CreateNumber(r->eyeRightM[1]));
	cJSON_AddItemToArray(re, cJSON_CreateNumber(r->eyeRightM[2]));

	cJSON *views = cJSON_AddArrayToObject(root, "views_written");
	if (r->viewsWritten & XR_WORKSPACE_CAPTURE_FLAG_ATLAS_BIT_EXT) {
		cJSON_AddItemToArray(views, cJSON_CreateString("atlas"));
	}

	char *text = cJSON_Print(root);
	if (text != NULL) {
		FILE *f = fopen(json_path, "wb");
		if (f != NULL) {
			fwrite(text, 1, strlen(text), f);
			fclose(f);
		} else {
			PE("capture: failed to open sidecar %s\n", json_path);
		}
		free(text);
	}
	cJSON_Delete(root);
}

static void
capture_frame(void)
{
	const char *home = getenv("USERPROFILE");
	if (home == NULL || home[0] == '\0') {
		home = ".";
	}

	char dir[MAX_PATH];
	snprintf(dir, sizeof(dir), "%s\\Pictures\\DisplayXR", home);

	// Best-effort: Pictures should already exist; create the DisplayXR subdir.
	char pictures[MAX_PATH];
	snprintf(pictures, sizeof(pictures), "%s\\Pictures", home);
	CreateDirectoryA(pictures, NULL);
	if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
		PE("capture: CreateDirectory(%s) failed: %lu\n", dir, GetLastError());
		return;
	}

	SYSTEMTIME st;
	GetLocalTime(&st);

	XrWorkspaceCaptureRequestEXT req = {XR_TYPE_WORKSPACE_CAPTURE_REQUEST_EXT};
	snprintf(req.pathPrefix, sizeof(req.pathPrefix),
	         "%s\\capture_%04d-%02d-%02d_%02d-%02d-%02d",
	         dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	req.flags = XR_WORKSPACE_CAPTURE_FLAG_ATLAS_BIT_EXT;

	XrWorkspaceCaptureResultEXT result = {XR_TYPE_WORKSPACE_CAPTURE_RESULT_EXT};
	XrResult r = g_xr->capture_frame(g_xr->session, &req, &result);
	if (r != XR_SUCCESS) {
		PE("capture: xrCaptureWorkspaceFrameEXT failed: %d\n", r);
		return;
	}

	if (result.viewsWritten == 0) {
		PE("capture: runtime returned 0 views written\n");
		return;
	}

	char json_path[MAX_PATH];
	snprintf(json_path, sizeof(json_path), "%s.json", req.pathPrefix);
	capture_write_sidecar(json_path, &result);

	P("Capture saved: %s (views=0x%llx atlas=%ux%u eye=%ux%u)\n",
	  req.pathPrefix, (unsigned long long)result.viewsWritten,
	  result.atlasWidth, result.atlasHeight,
	  result.eyeWidth, result.eyeHeight);
}

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

	// Phase 2.I C10: shell connects to the service exclusively via the public
	// OpenXR extension surface. shell_openxr_init runs the runtime DLL which
	// internally opens the IPC connection and auto-starts displayxr-service
	// if it isn't already running. The shell never manages an IPC connection
	// directly anymore.
	P("Connecting to service via OpenXR runtime...\n");
	xrt_result_t xret = XRT_SUCCESS;
	struct shell_openxr_state *xr = shell_openxr_init();
	if (xr == NULL) {
		PE("shell_openxr_init failed — workspace + launcher extensions unavailable.\n");
		return 1;
	}
	g_xr = xr;
	P("Connected to service.\n");

	// Load registered apps config (Phase 4C.9 + Phase 5.5 scanner merge)
	registered_apps_load();

	// Phase 5.8: push the merged registry to the service so the spatial
	// launcher panel can render its tile grid.
	shell_push_registered_apps_to_service();

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

	// Register system-wide hotkeys. Ctrl+Space in service-managed mode exits
	// the shell so the orchestrator's keyboard hook (gated on !s_shell_running)
	// can re-spawn us on the next press.
	if (g_msg_hwnd != NULL) {
		if (!RegisterHotKey(g_msg_hwnd, HOTKEY_TOGGLE, MOD_CONTROL | MOD_NOREPEAT, VK_SPACE)) {
			PE("Warning: RegisterHotKey(Ctrl+Space) failed — hotkey unavailable\n");
		}
		if (!RegisterHotKey(g_msg_hwnd, HOTKEY_LAUNCH, MOD_CONTROL | MOD_NOREPEAT, 'L')) {
			PE("Warning: RegisterHotKey(Ctrl+L) failed — launcher hotkey unavailable\n");
		}
		if (!RegisterHotKey(g_msg_hwnd, HOTKEY_CAPTURE, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'C')) {
			PE("Warning: RegisterHotKey(Ctrl+Shift+C) failed — capture hotkey unavailable\n");
		}
	}

	// Create system tray icon
	if (g_msg_hwnd != NULL) {
		tray_create(g_msg_hwnd);
	}

	// --- Decide startup mode ---
	// If launched with apps or captures: activate immediately.
	// If launched via the service orchestrator (--service-managed): also
	// activate immediately. The orchestrator only spawns us in response to
	// Ctrl+Space being pressed (semantically "summon shell"), so starting
	// inactive-in-tray would need a second Ctrl+Space to activate — but
	// with --service-managed we skip our own hotkey registration, so no
	// one's listening for that second press.
	// Standalone no-args launches still start in tray and wait for the
	// shell's own Ctrl+Space to toggle active.
	bool start_active = (app_count > 0 || capture_count > 0 || g_service_managed);

	if (start_active) {
		P("Activating shell mode...\n");
		xret = (xrt_result_t)g_xr->activate(g_xr->session);
		if (xret != XRT_SUCCESS) {
			PE("Warning: workspace_activate failed\n");
		}
		g_shell_active = true;
		tray_update_tooltip(true);

		// Add capture clients
		for (int i = 0; i < capture_count; i++) {
			uint32_t cid = 0;
			xrt_result_t r = (xrt_result_t)g_xr->add_capture(g_xr->session, captures[i].hwnd, NULL, &cid);
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
	xret = (xrt_result_t)g_xr->activate(g_xr->session);
	if (xret != XRT_SUCCESS) {
		PE("Warning: workspace_activate failed\n");
	}
	bool auto_adopt = false;
#endif

	P("Monitoring clients (Ctrl+C to exit)...\n");

	uint32_t prev_ids[SHELL_MAX_CLIENTS] = {0};
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
					// Service-managed shell: Ctrl+Space exits instead of
					// toggling to tray. The orchestrator will re-spawn us
					// on the next press via its keyboard hook.
					if (g_service_managed) {
						P("Ctrl+Space pressed — exiting service-managed shell.\n");
						g_running = 0;
						break;
					}
					// --- Toggle shell active/inactive ---
					if (g_shell_active) {
						P("Deactivating shell...\n");
						(void)g_xr->deactivate(g_xr->session);

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
						xret = (xrt_result_t)g_xr->activate(g_xr->session);

						// Phase 2.I C10: the shell no longer owns its own IPC
						// connection — the runtime DLL does, and rebuilding
						// it requires xrDestroySession + xrCreateSession (a
						// future improvement). On activate failure today we
						// just bail and let the user re-trigger Ctrl+Space
						// once the service is healthy again.
						if (xret != XRT_SUCCESS) {
							PE("workspace_activate failed (%d) — service may be down. "
							   "Restart the service and press Ctrl+Space to retry.\n",
							   xret);
							continue;
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

						XrResult lret = g_xr->set_launcher_visible(
						    g_xr->session, g_launcher_visible ? XR_TRUE : XR_FALSE);
						if (lret != XR_SUCCESS) {
							PE("xrSetLauncherVisibleEXT failed: %d\n", lret);
							g_launcher_visible = !g_launcher_visible; // roll back
						} else {
							P("Launcher %s\n", g_launcher_visible ? "shown" : "hidden");
						}
					}
				} else if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_CAPTURE) {
					// --- Ctrl+Shift+C: capture pre-weave SBS frame (Phase 8) ---
					if (g_shell_active) {
						capture_frame();
					} else {
						P("Capture ignored: shell not active\n");
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
			try_apply_poses(apps, app_count, prev_ids, prev_count);

			poses_pending = false;
			for (int i = 0; i < app_count; i++) {
				if (apps[i].has_pose && !apps[i].pose_applied) {
					poses_pending = true;
				}
			}
		}

		// Detect new clients and track names
		{
			XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
			uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
			for (uint32_t c = 0; c < client_count; c++) {
				bool is_new = true;
				for (uint32_t p = 0; p < prev_count; p++) {
					if (client_ids[c] == prev_ids[p]) {
						is_new = false;
						break;
					}
				}
				if (is_new && client_ids[c] != XR_NULL_WORKSPACE_CLIENT_ID) {
					XrWorkspaceClientInfoEXT cinfo;
					if (shell_get_client_info(client_ids[c], &cinfo) && cinfo.name[0] != '\0') {
						int instance = 1;
						for (int cn = 0; cn < client_name_count; cn++) {
							char existing_base[128];
							snprintf(existing_base, sizeof(existing_base), "%s", client_names[cn].name);
							char *paren = strrchr(existing_base, '(');
							if (paren && paren > existing_base && *(paren - 1) == ' ')
								*(paren - 1) = '\0';
							if (strcmp(existing_base, cinfo.name) == 0)
								instance++;
						}

						char numbered_name[128];
						if (instance > 1)
							snprintf(numbered_name, sizeof(numbered_name), "%s (%d)",
							         cinfo.name, instance);
						else
							snprintf(numbered_name, sizeof(numbered_name), "%s",
							         cinfo.name);

						if (client_name_count < MAX_SAVED_WINDOWS) {
							client_names[client_name_count].id = client_ids[c];
							snprintf(client_names[client_name_count].name, 128, "%s", numbered_name);
							client_name_count++;
						}
					}
				}
			}
		}

		print_clients(prev_ids, &prev_count);

#ifdef _WIN32
		// Detect server-side deactivation (ESC closed the compositor window).
		// The compositor sets workspace_mode=false — sync our state.
		if (g_shell_active) {
			XrBool32 server_active_b = XR_FALSE;
			if (g_xr->get_state(g_xr->session, &server_active_b) == XR_SUCCESS) {
				bool server_active = (server_active_b == XR_TRUE);
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
			uint64_t mask = shell_compute_running_tile_mask();
			if (mask != s_last_running_mask) {
				(void)g_xr->set_running_tile_mask(g_xr->session, mask);
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
			int32_t tile_index = XR_LAUNCHER_INVALID_APPINDEX_EXT;
			if (g_xr->poll_launcher_click(g_xr->session, &tile_index) == XR_SUCCESS &&
			    tile_index != XR_LAUNCHER_INVALID_APPINDEX_EXT) {
				// Service already hid the launcher when the action registered.
				g_launcher_visible = false;

				// Phase 6.6: check refresh BEFORE remove — both use negative
				// sentinels and refresh (-300) would match the remove check
				// (<= -200) if checked second.
				if (tile_index == XR_LAUNCHER_APPINDEX_REFRESH_EXT) {
					P("Launcher: refreshing app list (before: %d apps)\n",
					  g_registered_app_count);
					registered_apps_load();
					P("Launcher: refreshed (after: %d apps)\n",
					  g_registered_app_count);
					shell_push_registered_apps_to_service();
#ifdef _WIN32
					if (service_pid != 0) {
						AllowSetForegroundWindow(service_pid);
					}
#endif
					if (g_xr->set_launcher_visible(g_xr->session, XR_TRUE) == XR_SUCCESS) {
						g_launcher_visible = true;
					}
				} else if (tile_index <= -XR_LAUNCHER_APPINDEX_REMOVE_BASE_EXT) {
					int full_idx = (int)(-(tile_index) - XR_LAUNCHER_APPINDEX_REMOVE_BASE_EXT);
					if (full_idx >= 0 && full_idx < g_registered_app_count) {
						struct registered_app *rm = &g_registered_apps[full_idx];
						P("Launcher: removing '%s' permanently\n", rm->name);

#ifdef _WIN32
						// Delete the manifest file (and sibling icon files)
						// when it's a registered-mode manifest the user owns.
						// Sidecar manifests in dev paths are developer-owned
						// and never deleted here.
						if (rm->manifest_path[0]) {
							const char *local = getenv("LOCALAPPDATA");
							const char *progdata = getenv("ProgramData");
							bool in_user_dir = (local && _strnicmp(rm->manifest_path,
							    local, strlen(local)) == 0);
							bool in_system_dir = (progdata && _strnicmp(rm->manifest_path,
							    progdata, strlen(progdata)) == 0);
							if (in_user_dir || in_system_dir) {
								if (DeleteFileA(rm->manifest_path)) {
									P("  deleted manifest: %s\n", rm->manifest_path);
								} else {
									DWORD err = GetLastError();
									PE("  could not delete manifest %s "
									   "(err=%lu) — tile will reappear next scan\n",
									   rm->manifest_path, err);
								}
								// Also delete the resolved icon files when
								// they live alongside the manifest. We
								// compare directories so we don't nuke a
								// shared icon in another location.
								char manifest_dir[MAX_PATH];
								snprintf(manifest_dir, sizeof(manifest_dir), "%s",
								         rm->manifest_path);
								char *sep = strrchr(manifest_dir, '\\');
								if (sep) *sep = '\0';
								if (rm->icon_path[0] &&
								    _strnicmp(rm->icon_path, manifest_dir,
								              strlen(manifest_dir)) == 0) {
									DeleteFileA(rm->icon_path);
								}
								if (rm->icon_3d_path[0] &&
								    _strnicmp(rm->icon_3d_path, manifest_dir,
								              strlen(manifest_dir)) == 0) {
									DeleteFileA(rm->icon_3d_path);
								}
							}
						}
#endif
						// Shift remaining entries down.
						for (int j = full_idx; j < g_registered_app_count - 1; j++) {
							g_registered_apps[j] = g_registered_apps[j + 1];
						}
						g_registered_app_count--;
						registered_apps_save();
						shell_push_registered_apps_to_service();
					}
					// Re-show launcher so user sees the updated grid.
#ifdef _WIN32
					if (service_pid != 0) {
						AllowSetForegroundWindow(service_pid);
					}
#endif
					if (g_xr->set_launcher_visible(g_xr->session, XR_TRUE) == XR_SUCCESS) {
						g_launcher_visible = true;
					}
				} else if (tile_index == XR_LAUNCHER_APPINDEX_BROWSE_EXT) {
					// Phase 5.14: Browse tile → open file dialog, add to
					// registry, re-push, re-show launcher so the user sees
					// their new tile.
					shell_browse_and_add_app();
#ifdef _WIN32
					if (service_pid != 0) {
						AllowSetForegroundWindow(service_pid);
					}
#endif
					if (g_xr->set_launcher_visible(g_xr->session, XR_TRUE) == XR_SUCCESS) {
						g_launcher_visible = true;
					}
				} else if (tile_index >= 0 && tile_index < (int32_t)g_registered_app_count) {
					struct registered_app *rapp = &g_registered_apps[tile_index];

					// Phase 5.11: if a matching client is already running,
					// focus it instead of spawning a second instance.
					bool focused_existing = false;
#ifdef _WIN32
					{
						XrWorkspaceClientId clist_ids[SHELL_MAX_CLIENTS];
						uint32_t clist_count = shell_enumerate_clients(clist_ids, SHELL_MAX_CLIENTS);
						for (uint32_t c = 0; c < clist_count && !focused_existing; c++) {
							if (clist_ids[c] == XR_NULL_WORKSPACE_CLIENT_ID) continue;
							XrWorkspaceClientInfoEXT cinfo;
							if (!shell_get_client_info(clist_ids[c], &cinfo))
								continue;
							char client_exe[MAX_PATH];
							if (!shell_pid_to_exe_path((DWORD)cinfo.pid, client_exe, sizeof(client_exe)))
								continue;
							if (shell_exe_paths_equal(client_exe, rapp->exe_path)) {
								if (g_xr->set_focused(g_xr->session, clist_ids[c]) == XR_SUCCESS) {
									P("Launcher: focused running client %u → '%s'\n",
									  (unsigned)clist_ids[c], rapp->name);
									focused_existing = true;
								}
							}
						}
					}
#endif
					if (!focused_existing) {
						P("Launcher: launching tile %d → '%s'\n",
						  (int)tile_index, rapp->name);
						shell_launch_registered_app(
						    rapp,
						    have_json ? runtime_json : NULL,
						    apps, &app_count,
						    captures, &capture_count);
					}
				} else {
					PE("Launcher click: tile %d out of range (count=%d)\n",
					   (int)tile_index, g_registered_app_count);
				}
			}
		}

		// Dynamic window tracking: detect new/closed windows every ~1 second
		if (auto_adopt) {
			adopt_counter++;
			if (adopt_counter >= 2) {
				adopt_counter = 0;
				cleanup_closed_captures(captures, &capture_count);
				enumerate_and_adopt_windows(captures, &capture_count, service_pid);
			}
		}
#endif
	}

	P("\nShell exiting.\n");

#ifdef _WIN32
	// Deactivate shell if still active
	if (g_shell_active) {
		(void)g_xr->deactivate(g_xr->session);
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

	shell_openxr_shutdown(xr);
	g_xr = NULL;
	return 0;
}
