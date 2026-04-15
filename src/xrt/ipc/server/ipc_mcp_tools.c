// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MCP shell-scope tool handlers (Phase B).
 * @ingroup ipc_server
 */

#include "ipc_mcp_tools.h"

#include "server/ipc_server.h"
#include "shared/ipc_protocol.h"

#include "util/u_mcp_server.h"
#include "util/u_logging.h"

#include "os/os_threading.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef XRT_OS_WINDOWS
#include <direct.h>
#include <shlobj.h>
#include <windows.h>
#define MKDIR(path) _mkdir(path)
#define PATH_SEP "\\"
#else
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(path) mkdir((path), 0700)
#define PATH_SEP "/"
#endif

#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
#include "d3d11_service/comp_d3d11_service.h"
#endif

#define LOG_PFX "[mcp-shell] "

// File-static server pointer — the Phase A server registry takes a raw
// userdata void* per tool, but for the Phase B shell tools all handlers
// share the same ipc_server anyway. Keeping it here avoids threading the
// pointer through each static tool descriptor.
static struct ipc_server *g_ipc_server = NULL;


// ---------- list_windows ----------

static cJSON *
tool_list_windows(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	struct ipc_server *s = g_ipc_server;
	if (s == NULL) {
		return NULL;
	}

	cJSON *arr = cJSON_CreateArray();
	if (arr == NULL) {
		return NULL;
	}

	os_mutex_lock(&s->global_state.lock);

	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		volatile struct ipc_client_state *ics = &s->threads[i].ics;
		// Skip empty slots and clients that haven't finished handshake.
		if (ics->server_thread_index < 0) {
			continue;
		}
		// Cast-drop volatile for read access — we hold the server lock
		// so the fields are stable for the duration of this call.
		const struct ipc_app_state *as =
		    (const struct ipc_app_state *)&ics->client_state;
		if (as->id == 0) {
			continue;
		}

		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "id", (double)as->id);
		cJSON_AddNumberToObject(o, "pid", (double)as->pid);
		cJSON_AddStringToObject(o, "name", as->info.application_name);
		cJSON_AddBoolToObject(o, "session_active", as->session_active);
		cJSON_AddBoolToObject(o, "session_visible", as->session_visible);
		cJSON_AddBoolToObject(o, "session_focused", as->session_focused);
		cJSON_AddNumberToObject(o, "z_order", (double)as->z_order);
		cJSON_AddBoolToObject(o, "primary", as->primary_application);

#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
		// Shell-mode pose + size. If the client hasn't produced its
		// compositor yet (or we're not in shell mode) the accessor
		// returns false and we emit nulls so the caller can tell.
		struct xrt_pose pose = {0};
		float width_m = 0.f, height_m = 0.f;
		bool have_pose = false;
		if (ics->xc != NULL && s->xsysc != NULL) {
			have_pose = comp_d3d11_service_get_client_window_pose(
			    s->xsysc, (struct xrt_compositor *)ics->xc, &pose, &width_m, &height_m);
		}
		if (have_pose) {
			cJSON *p = cJSON_CreateObject();
			cJSON_AddNumberToObject(p, "x", pose.position.x);
			cJSON_AddNumberToObject(p, "y", pose.position.y);
			cJSON_AddNumberToObject(p, "z", pose.position.z);
			cJSON_AddNumberToObject(p, "qx", pose.orientation.x);
			cJSON_AddNumberToObject(p, "qy", pose.orientation.y);
			cJSON_AddNumberToObject(p, "qz", pose.orientation.z);
			cJSON_AddNumberToObject(p, "qw", pose.orientation.w);
			cJSON_AddItemToObject(o, "pose", p);
			cJSON *d = cJSON_CreateObject();
			cJSON_AddNumberToObject(d, "width_m", width_m);
			cJSON_AddNumberToObject(d, "height_m", height_m);
			cJSON_AddItemToObject(o, "size", d);
		}
#endif
		cJSON_AddItemToArray(arr, o);
	}

	os_mutex_unlock(&s->global_state.lock);
	return arr;
}


// ---------- get/set_window_pose ----------

#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
// Look up an ipc_client_state by stable client_id. Caller must hold
// s->global_state.lock. Returns NULL if not found.
static volatile struct ipc_client_state *
find_client_locked(struct ipc_server *s, uint32_t client_id)
{
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		volatile struct ipc_client_state *ics = &s->threads[i].ics;
		if (ics->client_state.id == client_id && ics->server_thread_index >= 0) {
			return ics;
		}
	}
	return NULL;
}
#endif

static bool
extract_client_id(const cJSON *params, uint32_t *out)
{
	if (params == NULL) {
		return false;
	}
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "client_id");
	if (!cJSON_IsNumber(id)) {
		return false;
	}
	double v = id->valuedouble;
	if (v < 0.0 || v > (double)UINT32_MAX) {
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

static cJSON *
tool_get_window_pose(const cJSON *params, void *userdata)
{
	(void)userdata;
	struct ipc_server *s = g_ipc_server;
	uint32_t client_id = 0;
	if (s == NULL || !extract_client_id(params, &client_id)) {
		return NULL;
	}

#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
	struct xrt_pose pose = {0};
	float w = 0.f, h = 0.f;
	bool ok = false;

	if (s->xsysc != NULL && client_id >= 1000) {
		int slot = (int)(client_id - 1000);
		ok = comp_d3d11_service_get_capture_client_window_pose(
		    s->xsysc, slot, &pose, &w, &h);
	} else if (s->xsysc != NULL) {
		os_mutex_lock(&s->global_state.lock);
		volatile struct ipc_client_state *ics = find_client_locked(s, client_id);
		if (ics != NULL && ics->xc != NULL) {
			ok = comp_d3d11_service_get_client_window_pose(
			    s->xsysc, (struct xrt_compositor *)ics->xc, &pose, &w, &h);
		}
		os_mutex_unlock(&s->global_state.lock);
	}

	if (!ok) {
		return NULL;
	}

	cJSON *o = cJSON_CreateObject();
	cJSON *p = cJSON_CreateObject();
	cJSON_AddNumberToObject(p, "x", pose.position.x);
	cJSON_AddNumberToObject(p, "y", pose.position.y);
	cJSON_AddNumberToObject(p, "z", pose.position.z);
	cJSON_AddNumberToObject(p, "qx", pose.orientation.x);
	cJSON_AddNumberToObject(p, "qy", pose.orientation.y);
	cJSON_AddNumberToObject(p, "qz", pose.orientation.z);
	cJSON_AddNumberToObject(p, "qw", pose.orientation.w);
	cJSON_AddItemToObject(o, "pose", p);
	cJSON *d = cJSON_CreateObject();
	cJSON_AddNumberToObject(d, "width_m", w);
	cJSON_AddNumberToObject(d, "height_m", h);
	cJSON_AddItemToObject(o, "size", d);
	cJSON_AddNumberToObject(o, "client_id", (double)client_id);
	return o;
#else
	(void)client_id;
	return NULL;
#endif
}

static bool
extract_pose(const cJSON *params, struct xrt_pose *out_pose, float *out_w, float *out_h)
{
	if (params == NULL) {
		return false;
	}
	const cJSON *pose = cJSON_GetObjectItemCaseSensitive(params, "pose");
	const cJSON *size = cJSON_GetObjectItemCaseSensitive(params, "size");
	if (!cJSON_IsObject(pose) || !cJSON_IsObject(size)) {
		return false;
	}
	// Accept a compact representation: {pose:{x,y,z,qx,qy,qz,qw}, size:{width_m,height_m}}.
	// Orientation defaults to identity when omitted to allow position-only tweaks.
	struct xrt_pose p = {0};
	p.orientation.w = 1.f;
	const struct
	{
		const char *key;
		float *dst;
	} pfields[] = {
	    {"x", &p.position.x},    {"y", &p.position.y},    {"z", &p.position.z},
	    {"qx", &p.orientation.x}, {"qy", &p.orientation.y}, {"qz", &p.orientation.z},
	    {"qw", &p.orientation.w},
	};
	for (size_t i = 0; i < sizeof(pfields) / sizeof(pfields[0]); i++) {
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(pose, pfields[i].key);
		if (cJSON_IsNumber(v)) {
			*pfields[i].dst = (float)v->valuedouble;
		}
	}
	const cJSON *wjs = cJSON_GetObjectItemCaseSensitive(size, "width_m");
	const cJSON *hjs = cJSON_GetObjectItemCaseSensitive(size, "height_m");
	if (!cJSON_IsNumber(wjs) || !cJSON_IsNumber(hjs)) {
		return false;
	}
	*out_pose = p;
	*out_w = (float)wjs->valuedouble;
	*out_h = (float)hjs->valuedouble;
	return true;
}

static cJSON *
tool_set_window_pose(const cJSON *params, void *userdata)
{
	(void)userdata;
	struct ipc_server *s = g_ipc_server;
	uint32_t client_id = 0;
	if (s == NULL || !extract_client_id(params, &client_id)) {
		return NULL;
	}
	struct xrt_pose pose = {0};
	float w = 0.f, h = 0.f;
	if (!extract_pose(params, &pose, &w, &h)) {
		return NULL;
	}

#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
	if (s->xsysc == NULL) {
		return NULL;
	}

	bool ok = false;
	if (client_id >= 1000) {
		int slot = (int)(client_id - 1000);
		ok = comp_d3d11_service_set_capture_client_window_pose(
		    s->xsysc, slot, &pose, w, h);
	} else {
		os_mutex_lock(&s->global_state.lock);
		volatile struct ipc_client_state *ics = find_client_locked(s, client_id);
		if (ics != NULL && ics->xc != NULL) {
			ok = comp_d3d11_service_set_client_window_pose(
			    s->xsysc, (struct xrt_compositor *)ics->xc, &pose, w, h);
		}
		os_mutex_unlock(&s->global_state.lock);
	}

	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "ok", ok);
	cJSON_AddNumberToObject(r, "client_id", (double)client_id);
	return r;
#else
	(void)client_id;
	(void)pose;
	(void)w;
	(void)h;
	return NULL;
#endif
}


// ---------- set_focus ----------

static cJSON *
tool_set_focus(const cJSON *params, void *userdata)
{
	(void)userdata;
	struct ipc_server *s = g_ipc_server;
	uint32_t client_id = 0;
	if (s == NULL || !extract_client_id(params, &client_id)) {
		return NULL;
	}

	// Route through the IPC-layer focus path. This is the same function
	// the stub at ipc_server_handler.c:set_focused_client now delegates
	// to, so MCP and IPC stay consistent on semantics.
	xrt_result_t xret = ipc_server_set_active_client(s, client_id);

	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "ok", xret == XRT_SUCCESS);
	cJSON_AddNumberToObject(r, "client_id", (double)client_id);
	return r;
}


// ---------- apply_layout_preset ----------

static cJSON *
tool_apply_layout_preset(const cJSON *params, void *userdata)
{
	(void)userdata;
	struct ipc_server *s = g_ipc_server;
	if (s == NULL || params == NULL) {
		return NULL;
	}
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "preset");
	if (!cJSON_IsString(name) || name->valuestring == NULL) {
		return NULL;
	}

#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
	if (s->xsysc == NULL) {
		return NULL;
	}
	bool ok = comp_d3d11_service_apply_layout_preset(s->xsysc, name->valuestring);
	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "ok", ok);
	cJSON_AddStringToObject(r, "preset", name->valuestring);
	return r;
#else
	return NULL;
#endif
}


// ---------- workspace persistence ----------

#define WORKSPACE_SCHEMA_VERSION 1
#define WORKSPACE_NAME_MAX 64

// Resolve the directory that holds workspace JSON files. Creates it on
// demand (and any parent). Returns false if the path cannot be obtained
// or the directory cannot be created.
static bool
workspace_dir(char *out, size_t cap)
{
#ifdef XRT_OS_WINDOWS
	char appdata[MAX_PATH];
	if (!SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
		return false;
	}
	// %APPDATA%\DisplayXR\workspaces
	char parent[MAX_PATH];
	snprintf(parent, sizeof(parent), "%s\\DisplayXR", appdata);
	MKDIR(parent);
	snprintf(out, cap, "%s\\DisplayXR\\workspaces", appdata);
	MKDIR(out);
	return true;
#else
	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0') {
		return false;
	}
	char parent[512];
	snprintf(parent, sizeof(parent), "%s/.config", home);
	MKDIR(parent);
	snprintf(parent, sizeof(parent), "%s/.config/displayxr", home);
	MKDIR(parent);
	snprintf(out, cap, "%s/.config/displayxr/workspaces", home);
	MKDIR(out);
	return true;
#endif
}

// Validate a workspace name as a safe file-system fragment: letters,
// digits, spaces, dash, underscore, dot. Rejects slashes and backslashes
// so a rogue name can't escape the workspaces directory.
static bool
valid_workspace_name(const char *name)
{
	if (name == NULL || name[0] == '\0') {
		return false;
	}
	size_t n = strlen(name);
	if (n >= WORKSPACE_NAME_MAX) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)name[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ' || c == '.';
		if (!ok) {
			return false;
		}
	}
	// Don't allow a leading dot (hidden files) or "..".
	if (name[0] == '.' || strcmp(name, "..") == 0) {
		return false;
	}
	return true;
}

static bool
workspace_path(char *out, size_t cap, const char *name)
{
	char dir[512];
	if (!workspace_dir(dir, sizeof(dir))) {
		return false;
	}
	snprintf(out, cap, "%s" PATH_SEP "%s.json", dir, name);
	return true;
}

// save_workspace: snapshot current list_windows to {name}.json.
//
// Schema:
//   {
//     "version": 1,
//     "windows": [
//       {"id": N, "name": "...", "pose": {...}, "size": {...}},
//       ...
//     ]
//   }
//
// Windows without a pose (no shell-mode data) are skipped.
static cJSON *
tool_save_workspace(const cJSON *params, void *userdata)
{
	(void)userdata;
	if (params == NULL) {
		return NULL;
	}
	const cJSON *name_j = cJSON_GetObjectItemCaseSensitive(params, "name");
	if (!cJSON_IsString(name_j) || !valid_workspace_name(name_j->valuestring)) {
		return NULL;
	}

	// Reuse tool_list_windows to avoid duplicating the enumeration path.
	cJSON *windows = tool_list_windows(NULL, NULL);
	if (windows == NULL) {
		return NULL;
	}

	cJSON *doc = cJSON_CreateObject();
	cJSON_AddNumberToObject(doc, "version", WORKSPACE_SCHEMA_VERSION);
	cJSON *arr = cJSON_CreateArray();
	cJSON *w;
	cJSON_ArrayForEach(w, windows)
	{
		const cJSON *pose = cJSON_GetObjectItemCaseSensitive(w, "pose");
		const cJSON *size = cJSON_GetObjectItemCaseSensitive(w, "size");
		if (!cJSON_IsObject(pose) || !cJSON_IsObject(size)) {
			continue;
		}
		cJSON *entry = cJSON_CreateObject();
		cJSON_AddNumberToObject(
		    entry, "id",
		    cJSON_GetObjectItemCaseSensitive(w, "id")->valuedouble);
		const cJSON *app_name = cJSON_GetObjectItemCaseSensitive(w, "name");
		if (cJSON_IsString(app_name)) {
			cJSON_AddStringToObject(entry, "name", app_name->valuestring);
		}
		cJSON_AddItemToObject(entry, "pose", cJSON_Duplicate(pose, 1));
		cJSON_AddItemToObject(entry, "size", cJSON_Duplicate(size, 1));
		cJSON_AddItemToArray(arr, entry);
	}
	cJSON_AddItemToObject(doc, "windows", arr);
	cJSON_Delete(windows);

	char *text = cJSON_PrintUnformatted(doc);
	size_t len = text != NULL ? strlen(text) : 0;
	char path[1024];
	bool wrote = false;
	if (text != NULL && workspace_path(path, sizeof(path), name_j->valuestring)) {
		FILE *f = fopen(path, "wb");
		if (f != NULL) {
			wrote = (fwrite(text, 1, len, f) == len);
			fclose(f);
		}
	}
	free(text);
	cJSON_Delete(doc);

	if (!wrote) {
		U_LOG_W(LOG_PFX "save_workspace: failed to write %s", path);
		return NULL;
	}

	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "ok", true);
	cJSON_AddStringToObject(r, "name", name_j->valuestring);
	cJSON_AddStringToObject(r, "path", path);
	return r;
}

static char *
slurp_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0 || n > (long)(4 * 1024 * 1024)) {
		fclose(f);
		return NULL;
	}
	char *buf = (char *)malloc((size_t)n + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	if (got != (size_t)n) {
		free(buf);
		return NULL;
	}
	buf[n] = '\0';
	if (out_len != NULL) {
		*out_len = (size_t)n;
	}
	return buf;
}

// Apply one window entry's pose+size. Reuses the existing
// comp_d3d11_service accessor paths so semantics stay identical to
// tool_set_window_pose.
static bool
apply_workspace_entry(struct ipc_server *s, const cJSON *entry)
{
#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
	if (s->xsysc == NULL) {
		return false;
	}
	const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(entry, "id");
	if (!cJSON_IsNumber(id_j)) {
		return false;
	}
	uint32_t cid = (uint32_t)id_j->valuedouble;

	struct xrt_pose pose = {0};
	float w = 0.f, h = 0.f;
	if (!extract_pose(entry, &pose, &w, &h)) {
		return false;
	}

	if (cid >= 1000) {
		return comp_d3d11_service_set_capture_client_window_pose(
		    s->xsysc, (int)(cid - 1000), &pose, w, h);
	}
	bool ok = false;
	os_mutex_lock(&s->global_state.lock);
	volatile struct ipc_client_state *ics = find_client_locked(s, cid);
	if (ics != NULL && ics->xc != NULL) {
		ok = comp_d3d11_service_set_client_window_pose(
		    s->xsysc, (struct xrt_compositor *)ics->xc, &pose, w, h);
	}
	os_mutex_unlock(&s->global_state.lock);
	return ok;
#else
	(void)s;
	(void)entry;
	return false;
#endif
}

static cJSON *
tool_load_workspace(const cJSON *params, void *userdata)
{
	(void)userdata;
	struct ipc_server *s = g_ipc_server;
	if (s == NULL || params == NULL) {
		return NULL;
	}
	const cJSON *name_j = cJSON_GetObjectItemCaseSensitive(params, "name");
	if (!cJSON_IsString(name_j) || !valid_workspace_name(name_j->valuestring)) {
		return NULL;
	}

	char path[1024];
	if (!workspace_path(path, sizeof(path), name_j->valuestring)) {
		return NULL;
	}
	char *text = slurp_file(path, NULL);
	if (text == NULL) {
		return NULL;
	}
	cJSON *doc = cJSON_Parse(text);
	free(text);
	if (doc == NULL) {
		return NULL;
	}

	const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "version");
	if (!cJSON_IsNumber(version) || (int)version->valuedouble != WORKSPACE_SCHEMA_VERSION) {
		cJSON_Delete(doc);
		return NULL;
	}

	int applied = 0, missed = 0;
	const cJSON *windows = cJSON_GetObjectItemCaseSensitive(doc, "windows");
	const cJSON *entry;
	cJSON_ArrayForEach(entry, windows)
	{
		if (apply_workspace_entry(s, entry)) {
			applied++;
		} else {
			missed++;
		}
	}
	cJSON_Delete(doc);

	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "ok", applied > 0 || missed == 0);
	cJSON_AddStringToObject(r, "name", name_j->valuestring);
	cJSON_AddNumberToObject(r, "applied", applied);
	cJSON_AddNumberToObject(r, "missed", missed);
	return r;
}


// ---------- registry ----------

static const struct u_mcp_tool TOOL_LIST_WINDOWS = {
    .name = "list_windows",
    .description =
        "List all OpenXR clients attached to the service, including shell "
        "window pose and size in meters when shell mode is active.",
    .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    .fn = tool_list_windows,
    .userdata = NULL,
};

static const struct u_mcp_tool TOOL_GET_WINDOW_POSE = {
    .name = "get_window_pose",
    .description = "Query 6-DOF pose and size (meters) of one shell window.",
    .input_schema_json =
        "{\"type\":\"object\",\"required\":[\"client_id\"],"
        "\"properties\":{\"client_id\":{\"type\":\"integer\"}},"
        "\"additionalProperties\":false}",
    .fn = tool_get_window_pose,
    .userdata = NULL,
};

static const struct u_mcp_tool TOOL_SET_FOCUS = {
    .name = "set_focus",
    .description =
        "Focus the specified OpenXR client (promotes to active slot).",
    .input_schema_json =
        "{\"type\":\"object\",\"required\":[\"client_id\"],"
        "\"properties\":{\"client_id\":{\"type\":\"integer\"}},"
        "\"additionalProperties\":false}",
    .fn = tool_set_focus,
    .userdata = NULL,
};

static const struct u_mcp_tool TOOL_SAVE_WORKSPACE = {
    .name = "save_workspace",
    .description =
        "Snapshot current window poses to a named JSON file on disk. "
        "Stored in %APPDATA%\\DisplayXR\\workspaces (Windows) or "
        "~/.config/displayxr/workspaces (POSIX).",
    .input_schema_json =
        "{\"type\":\"object\",\"required\":[\"name\"],"
        "\"properties\":{\"name\":{\"type\":\"string\","
        "\"pattern\":\"^[A-Za-z0-9 ._-]+$\",\"maxLength\":63}},"
        "\"additionalProperties\":false}",
    .fn = tool_save_workspace,
    .userdata = NULL,
};

static const struct u_mcp_tool TOOL_LOAD_WORKSPACE = {
    .name = "load_workspace",
    .description =
        "Load a named workspace and reapply every window pose via "
        "set_window_pose. Missing clients are reported as 'missed'.",
    .input_schema_json =
        "{\"type\":\"object\",\"required\":[\"name\"],"
        "\"properties\":{\"name\":{\"type\":\"string\","
        "\"pattern\":\"^[A-Za-z0-9 ._-]+$\",\"maxLength\":63}},"
        "\"additionalProperties\":false}",
    .fn = tool_load_workspace,
    .userdata = NULL,
};

static const struct u_mcp_tool TOOL_APPLY_LAYOUT_PRESET = {
    .name = "apply_layout_preset",
    .description =
        "Apply a named shell layout preset. Valid names: grid, immersive, carousel.",
    .input_schema_json =
        "{\"type\":\"object\",\"required\":[\"preset\"],"
        "\"properties\":{\"preset\":{\"type\":\"string\","
        "\"enum\":[\"grid\",\"immersive\",\"carousel\"]}},"
        "\"additionalProperties\":false}",
    .fn = tool_apply_layout_preset,
    .userdata = NULL,
};

static const struct u_mcp_tool TOOL_SET_WINDOW_POSE = {
    .name = "set_window_pose",
    .description =
        "Move / resize one shell window. pose.qw defaults to 1 if the "
        "orientation quaternion is omitted.",
    .input_schema_json =
        "{\"type\":\"object\",\"required\":[\"client_id\",\"pose\",\"size\"],"
        "\"properties\":{"
        "\"client_id\":{\"type\":\"integer\"},"
        "\"pose\":{\"type\":\"object\"},"
        "\"size\":{\"type\":\"object\"}},"
        "\"additionalProperties\":false}",
    .fn = tool_set_window_pose,
    .userdata = NULL,
};

void
ipc_mcp_tools_register(struct ipc_server *s)
{
	if (s == NULL) {
		return;
	}
	g_ipc_server = s;
	u_mcp_server_register_tool(&TOOL_LIST_WINDOWS);
	u_mcp_server_register_tool(&TOOL_GET_WINDOW_POSE);
	u_mcp_server_register_tool(&TOOL_SET_WINDOW_POSE);
	u_mcp_server_register_tool(&TOOL_SET_FOCUS);
	u_mcp_server_register_tool(&TOOL_APPLY_LAYOUT_PRESET);
	u_mcp_server_register_tool(&TOOL_SAVE_WORKSPACE);
	u_mcp_server_register_tool(&TOOL_LOAD_WORKSPACE);
	U_LOG_I(LOG_PFX "registered shell tools against ipc_server %p", (void *)s);
}
