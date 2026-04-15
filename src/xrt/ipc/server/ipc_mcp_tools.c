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
	U_LOG_I(LOG_PFX "registered shell tools against ipc_server %p", (void *)s);
}
