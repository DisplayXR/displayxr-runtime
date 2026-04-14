// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MCP server thread, JSON-RPC dispatch, tool registry.
 * @ingroup aux_util
 */

#include "u_mcp_server.h"
#include "u_mcp_transport.h"
#include "u_mcp_log_ring.h"

#include "util/u_logging.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_PFX "[mcp] "
#define MAX_TOOLS 32
#define MAX_FRAME_BYTES (4 * 1024 * 1024)

struct u_mcp_server
{
	pthread_t thread;
	bool thread_started;
	struct u_mcp_listener *listener;

	pthread_mutex_t tools_mutex;
	const struct u_mcp_tool *tools[MAX_TOOLS];
	size_t tool_count;
};

static struct u_mcp_server g_server = {
    .tools_mutex = PTHREAD_MUTEX_INITIALIZER,
};

// ---------- Frame I/O (Content-Length framing, LSP/MCP style) ----------

static bool
read_line(struct u_mcp_conn *conn, char *buf, size_t cap)
{
	size_t n = 0;
	while (n + 1 < cap) {
		char c;
		if (!u_mcp_conn_read(conn, &c, 1)) {
			return false;
		}
		buf[n++] = c;
		if (c == '\n') {
			buf[n] = '\0';
			return true;
		}
	}
	return false;
}

static char *
read_frame(struct u_mcp_conn *conn, size_t *out_len)
{
	char line[256];
	size_t content_length = 0;
	bool have_length = false;

	while (read_line(conn, line, sizeof(line))) {
		// Header end: blank line (CRLF or LF).
		if (line[0] == '\r' || line[0] == '\n') {
			break;
		}
		size_t key_len = strlen("Content-Length:");
		if (strncasecmp(line, "Content-Length:", key_len) == 0) {
			content_length = (size_t)strtoul(line + key_len, NULL, 10);
			have_length = true;
		}
		// Ignore other headers (e.g. Content-Type).
	}

	if (!have_length || content_length == 0 || content_length > MAX_FRAME_BYTES) {
		return NULL;
	}

	char *body = malloc(content_length + 1);
	if (body == NULL) {
		return NULL;
	}
	if (!u_mcp_conn_read(conn, body, content_length)) {
		free(body);
		return NULL;
	}
	body[content_length] = '\0';
	*out_len = content_length;
	return body;
}

static bool
write_frame(struct u_mcp_conn *conn, const char *body)
{
	if (body == NULL) {
		return false;
	}
	size_t len = strlen(body);
	char header[64];
	int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
	if (hlen < 0) {
		return false;
	}
	if (!u_mcp_conn_write(conn, header, (size_t)hlen)) {
		return false;
	}
	return u_mcp_conn_write(conn, body, len);
}

// ---------- JSON-RPC helpers ----------

static cJSON *
jsonrpc_envelope(const cJSON *id)
{
	cJSON *env = cJSON_CreateObject();
	cJSON_AddStringToObject(env, "jsonrpc", "2.0");
	if (id != NULL) {
		cJSON_AddItemToObject(env, "id", cJSON_Duplicate(id, 1));
	}
	return env;
}

static char *
jsonrpc_result(const cJSON *id, cJSON *result)
{
	cJSON *env = jsonrpc_envelope(id);
	cJSON_AddItemToObject(env, "result", result != NULL ? result : cJSON_CreateObject());
	char *out = cJSON_PrintUnformatted(env);
	cJSON_Delete(env);
	return out;
}

static char *
jsonrpc_error(const cJSON *id, int code, const char *message)
{
	cJSON *env = jsonrpc_envelope(id);
	cJSON *err = cJSON_CreateObject();
	cJSON_AddNumberToObject(err, "code", code);
	cJSON_AddStringToObject(err, "message", message ? message : "unknown error");
	cJSON_AddItemToObject(env, "error", err);
	char *out = cJSON_PrintUnformatted(env);
	cJSON_Delete(env);
	return out;
}

// ---------- Tool registry ----------

static const struct u_mcp_tool *
find_tool(const char *name)
{
	pthread_mutex_lock(&g_server.tools_mutex);
	const struct u_mcp_tool *found = NULL;
	for (size_t i = 0; i < g_server.tool_count; i++) {
		if (strcmp(g_server.tools[i]->name, name) == 0) {
			found = g_server.tools[i];
			break;
		}
	}
	pthread_mutex_unlock(&g_server.tools_mutex);
	return found;
}

void
u_mcp_server_register_tool(const struct u_mcp_tool *tool)
{
	if (tool == NULL || tool->name == NULL || tool->fn == NULL) {
		return;
	}
	pthread_mutex_lock(&g_server.tools_mutex);
	if (g_server.tool_count < MAX_TOOLS) {
		g_server.tools[g_server.tool_count++] = tool;
	} else {
		U_LOG_W(LOG_PFX "tool registry full; dropping '%s'", tool->name);
	}
	pthread_mutex_unlock(&g_server.tools_mutex);
}

// ---------- Built-in echo tool (slice 1 handshake check) ----------

// ---------- Built-in tail_log tool ----------

static cJSON *
tool_tail_log(const cJSON *params, void *userdata)
{
	(void)userdata;
	uint64_t since = 0;
	size_t max_entries = 128;
	if (params != NULL) {
		const cJSON *s = cJSON_GetObjectItemCaseSensitive(params, "since");
		if (cJSON_IsNumber(s)) {
			since = (uint64_t)s->valuedouble;
		}
		const cJSON *m = cJSON_GetObjectItemCaseSensitive(params, "max");
		if (cJSON_IsNumber(m)) {
			double mv = m->valuedouble;
			if (mv > 0 && mv <= 1024) {
				max_entries = (size_t)mv;
			}
		}
	}

	struct u_mcp_log_entry *buf = calloc(max_entries, sizeof(*buf));
	if (buf == NULL) {
		return NULL;
	}
	size_t count = 0;
	uint64_t next_cursor = since, dropped = 0;
	u_mcp_log_ring_read(since, buf, max_entries, &count, &next_cursor, &dropped);

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "cursor", (double)next_cursor);
	cJSON_AddNumberToObject(r, "dropped", (double)dropped);
	cJSON *arr = cJSON_CreateArray();
	static const char *level_names[] = {"trace", "debug", "info", "warn", "error", "raw"};
	for (size_t i = 0; i < count; i++) {
		cJSON *e = cJSON_CreateObject();
		cJSON_AddNumberToObject(e, "seq", (double)buf[i].seq);
		cJSON_AddNumberToObject(e, "ts_ns", (double)buf[i].timestamp_ns);
		int lv = (int)buf[i].level;
		cJSON_AddStringToObject(e, "level",
		                        (lv >= 0 && lv < (int)(sizeof(level_names) / sizeof(level_names[0])))
		                            ? level_names[lv]
		                            : "unknown");
		cJSON_AddStringToObject(e, "text", buf[i].text);
		cJSON_AddItemToArray(arr, e);
	}
	cJSON_AddItemToObject(r, "entries", arr);
	free(buf);
	return r;
}

static const struct u_mcp_tool TAIL_LOG_TOOL = {
    .name = "tail_log",
    .description =
        "Return buffered U_LOG lines with seq > `since`. Pass the returned `cursor` as `since` "
        "on the next call to stream. `dropped` indicates how many entries were evicted before "
        "the caller read them (ring size is fixed).",
    .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"since\":{\"type\":\"integer\",\"default\":0},"
        "\"max\":{\"type\":\"integer\",\"default\":128,\"maximum\":1024}}}",
    .fn = tool_tail_log,
    .userdata = NULL,
};

static cJSON *
tool_echo(const cJSON *params, void *userdata)
{
	(void)userdata;
	cJSON *result = cJSON_CreateObject();
	cJSON_AddItemToObject(result, "echo", params != NULL ? cJSON_Duplicate(params, 1) : cJSON_CreateNull());
	return result;
}

static const struct u_mcp_tool ECHO_TOOL = {
    .name = "echo",
    .description = "Echoes the params back in {\"echo\": <params>}. Used for handshake testing.",
    .input_schema_json = "{\"type\":\"object\"}",
    .fn = tool_echo,
    .userdata = NULL,
};

// ---------- MCP protocol methods ----------

static cJSON *
build_tools_list(void)
{
	cJSON *arr = cJSON_CreateArray();
	pthread_mutex_lock(&g_server.tools_mutex);
	for (size_t i = 0; i < g_server.tool_count; i++) {
		const struct u_mcp_tool *t = g_server.tools[i];
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", t->name);
		if (t->description != NULL) {
			cJSON_AddStringToObject(e, "description", t->description);
		}
		if (t->input_schema_json != NULL) {
			cJSON *schema = cJSON_Parse(t->input_schema_json);
			if (schema != NULL) {
				cJSON_AddItemToObject(e, "inputSchema", schema);
			}
		}
		cJSON_AddItemToArray(arr, e);
	}
	pthread_mutex_unlock(&g_server.tools_mutex);
	return arr;
}

static char *
handle_request(const cJSON *req)
{
	const cJSON *method_node = cJSON_GetObjectItemCaseSensitive(req, "method");
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
	const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
	bool is_notification = (id == NULL);
	const char *method = cJSON_IsString(method_node) ? method_node->valuestring : NULL;

	if (method == NULL) {
		return is_notification ? NULL : jsonrpc_error(id, -32600, "invalid request: missing method");
	}

	// Notifications are fire-and-forget; MCP sends notifications/initialized.
	if (is_notification) {
		return NULL;
	}

	if (strcmp(method, "initialize") == 0) {
		cJSON *result = cJSON_CreateObject();
		cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
		cJSON *caps = cJSON_CreateObject();
		cJSON *tools_cap = cJSON_CreateObject();
		cJSON_AddBoolToObject(tools_cap, "listChanged", false);
		cJSON_AddItemToObject(caps, "tools", tools_cap);
		cJSON_AddItemToObject(result, "capabilities", caps);
		cJSON *info = cJSON_CreateObject();
		cJSON_AddStringToObject(info, "name", "displayxr-mcp");
		cJSON_AddStringToObject(info, "version", "0.2.0-phase-a");
		cJSON_AddItemToObject(result, "serverInfo", info);
		return jsonrpc_result(id, result);
	}

	if (strcmp(method, "ping") == 0) {
		return jsonrpc_result(id, cJSON_CreateObject());
	}

	if (strcmp(method, "tools/list") == 0) {
		cJSON *result = cJSON_CreateObject();
		cJSON_AddItemToObject(result, "tools", build_tools_list());
		return jsonrpc_result(id, result);
	}

	if (strcmp(method, "tools/call") == 0) {
		const cJSON *tool_name = cJSON_GetObjectItemCaseSensitive(params, "name");
		const cJSON *tool_args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
		if (!cJSON_IsString(tool_name)) {
			return jsonrpc_error(id, -32602, "tools/call: missing string 'name'");
		}
		const struct u_mcp_tool *tool = find_tool(tool_name->valuestring);
		if (tool == NULL) {
			return jsonrpc_error(id, -32601, "tool not found");
		}
		cJSON *inner = tool->fn(tool_args, tool->userdata);
		if (inner == NULL) {
			return jsonrpc_error(id, -32000, "tool handler failed");
		}
		// MCP wraps tool results in { content: [{type:'text', text: <json>}] }
		// so clients without per-tool schema rendering still see the payload.
		cJSON *result = cJSON_CreateObject();
		cJSON *content = cJSON_CreateArray();
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "type", "text");
		char *inner_str = cJSON_PrintUnformatted(inner);
		cJSON_AddStringToObject(item, "text", inner_str != NULL ? inner_str : "");
		free(inner_str);
		cJSON_AddItemToArray(content, item);
		cJSON_AddItemToObject(result, "content", content);
		// Also expose the structured result for programmatic consumers.
		cJSON_AddItemToObject(result, "structured", inner);
		return jsonrpc_result(id, result);
	}

	return jsonrpc_error(id, -32601, "method not found");
}

// ---------- Per-connection serve loop ----------

static void
serve(struct u_mcp_conn *conn)
{
	for (;;) {
		size_t len = 0;
		char *body = read_frame(conn, &len);
		if (body == NULL) {
			return; // EOF or framing error.
		}
		cJSON *req = cJSON_ParseWithLength(body, len);
		free(body);

		char *reply = NULL;
		if (req == NULL) {
			reply = jsonrpc_error(NULL, -32700, "parse error");
		} else {
			reply = handle_request(req);
			cJSON_Delete(req);
		}
		if (reply != NULL) {
			if (!write_frame(conn, reply)) {
				free(reply);
				return;
			}
			free(reply);
		}
	}
}

// ---------- Thread entry ----------

static void *
server_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct u_mcp_conn *c = u_mcp_listener_accept(g_server.listener);
		if (c == NULL) {
			return NULL; // listener closed.
		}
		serve(c);
		u_mcp_conn_close(c);
	}
}

// ---------- Public start/stop ----------

void
u_mcp_server_maybe_start(void)
{
	if (g_server.thread_started) {
		return;
	}
	const char *flag = getenv("DISPLAYXR_MCP");
	if (flag == NULL || flag[0] == '\0' || flag[0] == '0') {
		return;
	}

	// Start the log ring first so sink_cb captures bring-up messages,
	// then register built-in tools.
	u_mcp_log_ring_start();
	u_mcp_server_register_tool(&ECHO_TOOL);
	u_mcp_server_register_tool(&TAIL_LOG_TOOL);

	g_server.listener = u_mcp_listener_open(getpid());
	if (g_server.listener == NULL) {
		U_LOG_W(LOG_PFX "failed to open listener; MCP disabled");
		return;
	}
	int rc = pthread_create(&g_server.thread, NULL, server_thread, NULL);
	if (rc != 0) {
		U_LOG_W(LOG_PFX "pthread_create failed: %s", strerror(rc));
		u_mcp_listener_close(g_server.listener);
		g_server.listener = NULL;
		return;
	}
	g_server.thread_started = true;
	U_LOG_I(LOG_PFX "server started (pid=%ld)", (long)getpid());
}

void
u_mcp_server_stop(void)
{
	if (!g_server.thread_started) {
		return;
	}
	u_mcp_listener_close(g_server.listener);
	g_server.listener = NULL;
	pthread_join(g_server.thread, NULL);
	g_server.thread_started = false;
	u_mcp_log_ring_stop();
	U_LOG_I(LOG_PFX "server stopped");
}
