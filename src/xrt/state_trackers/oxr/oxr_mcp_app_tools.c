// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_EXT_mcp_tools — app-defined MCP tools on the per-process server.
 * @ingroup oxr_main
 *
 * The runtime already hosts an MCP server inside every app process
 * (oxr_mcp_tools.c, Phase A). This TU lets the *app* add its own tools
 * to it ("play_pause", "load_model", …) without linking the MCP
 * framework: registration travels over the OpenXR API boundary, and
 * invocation rides the OpenXR event queue.
 *
 * Dispatch contract (design doc per-app-mcp-tools.md §4.2): the MCP
 * transport thread never calls app code. The trampoline registered as
 * each tool's handler parks the JSON-RPC request in a pending-call
 * slot, pushes an XrEventDataMCPToolCallEXT, and blocks on a condvar
 * until the app answers via xrSubmitMCPToolResultEXT — or until the
 * 5 s timeout, after which the agent gets a tool error and a late
 * result is silently dropped.
 *
 * Threading: one global lock (g_lock) guards both tables. The
 * trampoline blocks on g_cond while holding nothing the app's API
 * calls need for longer than table edits. Parking a request is free on
 * the MCP side — JSON-RPC is async and the v0.4.0 server serves each
 * client on its own thread.
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_handle.h"
#include "oxr_api_verify.h"
#include "oxr_mcp_tools.h"

#ifdef OXR_HAVE_EXT_mcp_tools

#include "displayxr_mcp/mcp_server.h"

#include <cjson/cJSON.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_APP_TOOLS 32
#define MAX_PENDING_CALLS 16
#define CALL_TIMEOUT_SEC 5
//! Abandoned (timed-out, never-answered) slots are reclaimed after this.
#define ABANDONED_REAP_SEC 60

struct app_tool
{
	bool used;
	struct oxr_session *sess;
	char name[XR_MAX_MCP_TOOL_NAME_SIZE_EXT];
	char *description;      //!< strdup'd; freed on unregister.
	char *schema;           //!< strdup'd or NULL.
	struct mcp_tool tool;   //!< Registered with the framework; stable storage.
};

struct pending_call
{
	bool used;
	bool completed; //!< App answered; result fields valid.
	bool abandoned; //!< Timed out or tool/session torn down; awaiting reap.
	uint64_t call_id;
	struct oxr_session *sess;
	char *args_json;   //!< Owned. Exposed via xrGetMCPToolCallArgsEXT.
	char *result_json; //!< Owned. Set by xrSubmitMCPToolResultEXT.
	bool success;
	time_t abandoned_at; //!< For the reaper.
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static struct app_tool g_tools[MAX_APP_TOOLS];
static struct pending_call g_pending[MAX_PENDING_CALLS];
static uint64_t g_next_call_id = 0;

/*
 *
 * Helpers.
 *
 */

static bool
mcp_active(void)
{
	// Same gate expression as the server start in oxr_instance.c — if
	// this is false the per-process MCP server was never started and
	// the extension's functions are inert.
	return mcp_check_env_or(oxr_mcp_capability_enabled());
}

/*!
 * Bare tool name: ^[a-z0-9][a-z0-9_-]{0,62}$ and no "__" (the
 * aggregator's namespace separator).
 */
static bool
tool_name_valid(const char *name)
{
	if (name == NULL || name[0] == '\0') {
		return false;
	}
	size_t len = strlen(name);
	if (len >= XR_MAX_MCP_TOOL_NAME_SIZE_EXT) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		char c = name[i];
		bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (i == 0 ? !alnum : !(alnum || c == '-' || c == '_')) {
			return false;
		}
	}
	return strstr(name, "__") == NULL;
}

/*!
 * App id: ^[a-z0-9][a-z0-9-]{0,31}$ — mirrors manifest spec §3.4 and
 * the framework-side validation in mcp_server_set_app_id().
 */
static bool
app_id_valid(const char *id)
{
	if (id == NULL || id[0] == '\0') {
		return false;
	}
	size_t len = strlen(id);
	if (len >= XR_MAX_MCP_APP_ID_SIZE_EXT) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		char c = id[i];
		bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (i == 0 ? !alnum : !(alnum || c == '-')) {
			return false;
		}
	}
	return true;
}

// Caller holds g_lock.
static void
pending_free_locked(struct pending_call *pc)
{
	free(pc->args_json);
	free(pc->result_json);
	memset(pc, 0, sizeof(*pc));
}

// Caller holds g_lock. Reclaim abandoned slots whose app never answered.
static void
pending_reap_locked(void)
{
	time_t now = time(NULL);
	for (size_t i = 0; i < MAX_PENDING_CALLS; i++) {
		struct pending_call *pc = &g_pending[i];
		if (pc->used && pc->abandoned && now - pc->abandoned_at > ABANDONED_REAP_SEC) {
			pending_free_locked(pc);
		}
	}
}

/*
 *
 * The trampoline — runs on an MCP server client thread.
 *
 */

static cJSON *
tool_trampoline(const cJSON *params, void *userdata)
{
	struct app_tool *tool = userdata;

	pthread_mutex_lock(&g_lock);
	pending_reap_locked();

	struct pending_call *pc = NULL;
	for (size_t i = 0; i < MAX_PENDING_CALLS; i++) {
		if (!g_pending[i].used) {
			pc = &g_pending[i];
			break;
		}
	}
	if (pc == NULL || !tool->used) {
		pthread_mutex_unlock(&g_lock);
		return NULL; // Saturated or tool raced away → JSON-RPC error.
	}
	pc->used = true;
	pc->call_id = ++g_next_call_id;
	pc->sess = tool->sess;
	pc->args_json = params != NULL ? cJSON_PrintUnformatted(params) : NULL;
	if (pc->args_json == NULL) {
		pc->args_json = strdup("{}");
	}
	uint64_t call_id = pc->call_id;
	uint32_t args_size = (uint32_t)strlen(pc->args_json) + 1;

	struct oxr_logger log;
	oxr_log_init(&log, "mcp_tool_call");
	XrResult res = oxr_event_push_XrEventDataMCPToolCall(&log, tool->sess, call_id, tool->name, args_size);
	if (res != XR_SUCCESS) {
		pending_free_locked(pc);
		pthread_mutex_unlock(&g_lock);
		return NULL;
	}

	// Park until the app answers or the deadline passes. The condvar is
	// broadcast on every submit/teardown; re-find our slot by call_id
	// each wake (the table may have been edited around us).
	struct timespec deadline;
	timespec_get(&deadline, TIME_UTC);
	deadline.tv_sec += CALL_TIMEOUT_SEC;

	cJSON *out = NULL;
	for (;;) {
		// Re-locate; teardown may have freed the slot entirely.
		pc = NULL;
		for (size_t i = 0; i < MAX_PENDING_CALLS; i++) {
			if (g_pending[i].used && g_pending[i].call_id == call_id) {
				pc = &g_pending[i];
				break;
			}
		}
		if (pc == NULL || pc->abandoned) {
			break; // Torn down (session destroy / unregister).
		}
		if (pc->completed) {
			if (pc->success) {
				out = pc->result_json != NULL ? cJSON_Parse(pc->result_json) : cJSON_CreateObject();
				if (out == NULL) {
					// App submitted unparseable JSON; surface raw.
					out = cJSON_CreateObject();
					cJSON_AddStringToObject(out, "raw",
					                        pc->result_json != NULL ? pc->result_json : "");
				}
			}
			pending_free_locked(pc);
			break;
		}
		int rc = pthread_cond_timedwait(&g_cond, &g_lock, &deadline);
		if (rc != 0) {
			// Timeout: the agent gets an error now; a late submit
			// finds `abandoned` and is dropped.
			pc->abandoned = true;
			pc->abandoned_at = time(NULL);
			break;
		}
	}
	pthread_mutex_unlock(&g_lock);
	return out; // NULL → framework replies with a JSON-RPC tool error.
}

/*
 *
 * Session teardown — from oxr_session_destroy.
 *
 */

void
oxr_mcp_app_tools_session_destroy(struct oxr_session *sess)
{
	// Collect names first; mcp_server_unregister_tool broadcasts a
	// list_changed per call and must not run under g_lock (it takes the
	// framework's own locks).
	char names[MAX_APP_TOOLS][XR_MAX_MCP_TOOL_NAME_SIZE_EXT];
	size_t name_count = 0;

	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < MAX_APP_TOOLS; i++) {
		struct app_tool *t = &g_tools[i];
		if (!t->used || t->sess != sess) {
			continue;
		}
		strncpy(names[name_count], t->name, sizeof(names[0]) - 1);
		names[name_count][sizeof(names[0]) - 1] = '\0';
		name_count++;
	}
	// Fail the session's pending calls; parked trampolines wake and
	// return errors to their agents.
	for (size_t i = 0; i < MAX_PENDING_CALLS; i++) {
		struct pending_call *pc = &g_pending[i];
		if (pc->used && pc->sess == sess && !pc->completed) {
			pc->abandoned = true;
			pc->abandoned_at = time(NULL);
		}
	}
	pthread_cond_broadcast(&g_cond);
	pthread_mutex_unlock(&g_lock);

	// Drop the tools from the framework registry BEFORE freeing the slots:
	// the registry stores &slot->tool, whose .name points into the slot, so
	// the registry scan strcmp's it — zeroing the slot first reads NULL
	// (#459).
	for (size_t i = 0; i < name_count; i++) {
		mcp_server_unregister_tool(names[i]);
	}

	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < MAX_APP_TOOLS; i++) {
		struct app_tool *t = &g_tools[i];
		if (!t->used || t->sess != sess) {
			continue;
		}
		free(t->description);
		free(t->schema);
		memset(t, 0, sizeof(*t));
	}
	pthread_mutex_unlock(&g_lock);
}

/*
 *
 * API functions.
 *
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetMCPAppInfoEXT(XrSession session, const XrMCPAppInfoEXT *info)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetMCPAppInfoEXT");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, info, XR_TYPE_MCP_APP_INFO_EXT);

	if (!mcp_active()) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "MCP capability is disabled (Capabilities\\MCP / DISPLAYXR_MCP)");
	}
	if (!app_id_valid(info->appId)) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "appId must match ^[a-z0-9][a-z0-9-]{0,31}$ (manifest spec 3.4)");
	}

	mcp_server_set_app_id(info->appId);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrRegisterMCPToolEXT(XrSession session, const XrMCPToolInfoEXT *tool)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrRegisterMCPToolEXT");
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, tool, XR_TYPE_MCP_TOOL_INFO_EXT);

	if (!mcp_active()) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "MCP capability is disabled (Capabilities\\MCP / DISPLAYXR_MCP)");
	}
	if (!tool_name_valid(tool->name)) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "tool name must match ^[a-z0-9][a-z0-9_-]{0,62}$ with no '__'");
	}
	if (tool->description == NULL || tool->description[0] == '\0') {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "description is required — it is what the agent reads");
	}

	pthread_mutex_lock(&g_lock);
	struct app_tool *slot = NULL;
	for (size_t i = 0; i < MAX_APP_TOOLS; i++) {
		if (g_tools[i].used && strcmp(g_tools[i].name, tool->name) == 0) {
			pthread_mutex_unlock(&g_lock);
			return oxr_error(&log, XR_ERROR_NAME_DUPLICATED, "tool '%s' is already registered",
			                 tool->name);
		}
		if (!g_tools[i].used && slot == NULL) {
			slot = &g_tools[i];
		}
	}
	if (slot == NULL) {
		pthread_mutex_unlock(&g_lock);
		return oxr_error(&log, XR_ERROR_LIMIT_REACHED, "app tool budget (%d) exhausted",
		                 MAX_APP_TOOLS);
	}

	slot->used = true;
	slot->sess = sess;
	snprintf(slot->name, sizeof(slot->name), "%s", tool->name);
	slot->description = strdup(tool->description);
	slot->schema = (tool->inputSchemaJson != NULL && tool->inputSchemaJson[0] != '\0')
	                   ? strdup(tool->inputSchemaJson)
	                   : NULL;
	slot->tool = (struct mcp_tool){
	    .name = slot->name,
	    .description = slot->description,
	    .input_schema_json = slot->schema != NULL ? slot->schema : "{\"type\":\"object\"}",
	    .fn = tool_trampoline,
	    .userdata = slot,
	    .group = MCP_TOOL_GROUP_APP,
	};
	pthread_mutex_unlock(&g_lock);

	// Outside g_lock: broadcasts tools/list_changed to connected agents.
	mcp_server_register_tool(&slot->tool);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrUnregisterMCPToolEXT(XrSession session, const char *name)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrUnregisterMCPToolEXT");

	if (!mcp_active()) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "MCP capability is disabled (Capabilities\\MCP / DISPLAYXR_MCP)");
	}
	if (name == NULL || name[0] == '\0') {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "name is required");
	}

	pthread_mutex_lock(&g_lock);
	struct app_tool *found = NULL;
	for (size_t i = 0; i < MAX_APP_TOOLS; i++) {
		if (g_tools[i].used && g_tools[i].sess == sess && strcmp(g_tools[i].name, name) == 0) {
			found = &g_tools[i];
			break;
		}
	}
	if (found == NULL) {
		pthread_mutex_unlock(&g_lock);
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "tool '%s' is not registered on this session",
		                 name);
	}
	// In-flight calls on the tool: their slots survive (keyed by
	// call_id, not tool pointer) and fail at timeout or session end —
	// wake them now so they fail promptly.
	pthread_cond_broadcast(&g_cond);
	pthread_mutex_unlock(&g_lock);

	// Drop the tool from the framework registry BEFORE freeing the slot:
	// the registry stores &found->tool, whose .name points at found->name,
	// so the registry scan strcmp's it — zeroing the slot first reads NULL
	// (#459). `found` stays valid across the unlock: the slot is still
	// marked used, so a concurrent register can't reuse it.
	mcp_server_unregister_tool(name);

	pthread_mutex_lock(&g_lock);
	free(found->description);
	free(found->schema);
	memset(found, 0, sizeof(*found));
	pthread_mutex_unlock(&g_lock);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetMCPToolCallArgsEXT(
    XrSession session, uint64_t callId, uint32_t capacity, uint32_t *countOutput, char *buffer)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetMCPToolCallArgsEXT");
	OXR_VERIFY_ARG_NOT_NULL(&log, countOutput);

	pthread_mutex_lock(&g_lock);
	struct pending_call *pc = NULL;
	for (size_t i = 0; i < MAX_PENDING_CALLS; i++) {
		if (g_pending[i].used && g_pending[i].call_id == callId && !g_pending[i].abandoned) {
			pc = &g_pending[i];
			break;
		}
	}
	if (pc == NULL || pc->sess != sess) {
		pthread_mutex_unlock(&g_lock);
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "unknown or expired callId %llu",
		                 (unsigned long long)callId);
	}
	uint32_t needed = (uint32_t)strlen(pc->args_json) + 1;
	*countOutput = needed;
	if (capacity == 0) {
		pthread_mutex_unlock(&g_lock);
		return XR_SUCCESS;
	}
	if (capacity < needed || buffer == NULL) {
		pthread_mutex_unlock(&g_lock);
		return oxr_error(&log, XR_ERROR_SIZE_INSUFFICIENT, "need %u bytes, got %u", needed, capacity);
	}
	memcpy(buffer, pc->args_json, needed);
	pthread_mutex_unlock(&g_lock);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSubmitMCPToolResultEXT(XrSession session, uint64_t callId, XrBool32 success, const char *resultJson)
{
	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSubmitMCPToolResultEXT");

	pthread_mutex_lock(&g_lock);
	struct pending_call *pc = NULL;
	for (size_t i = 0; i < MAX_PENDING_CALLS; i++) {
		if (g_pending[i].used && g_pending[i].call_id == callId) {
			pc = &g_pending[i];
			break;
		}
	}
	if (pc == NULL || pc->sess != sess) {
		pthread_mutex_unlock(&g_lock);
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "callId %llu never existed",
		                 (unsigned long long)callId);
	}
	if (pc->abandoned) {
		// Late result after timeout/teardown: drop silently per spec.
		pending_free_locked(pc);
		pthread_mutex_unlock(&g_lock);
		return XR_SUCCESS;
	}
	pc->result_json = strdup(resultJson != NULL && resultJson[0] != '\0' ? resultJson : "{}");
	pc->success = success == XR_TRUE;
	pc->completed = true;
	pthread_cond_broadcast(&g_cond);
	pthread_mutex_unlock(&g_lock);
	return XR_SUCCESS;
}

#endif // OXR_HAVE_EXT_mcp_tools
