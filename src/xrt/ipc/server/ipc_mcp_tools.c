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

void
ipc_mcp_tools_register(struct ipc_server *s)
{
	if (s == NULL) {
		return;
	}
	g_ipc_server = s;
	u_mcp_server_register_tool(&TOOL_LIST_WINDOWS);
	U_LOG_I(LOG_PFX "registered shell tools against ipc_server %p", (void *)s);
}
