// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Wayland window-geometry provider implementation (#817).
 *
 * libdbus-1 client of the GNOME Shell extension `window-geometry@displayxr.org`
 * (contrib/gnome-shell/). One initial GetWindows snapshot at create, then a
 * non-blocking per-query pump of WindowsChanged signals. Single-threaded use
 * from the compositor frame loop — no locking.
 *
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_wl_geom.h"

#include "util/u_logging.h"
#include "util/u_json.h"
#include "util/u_misc.h"
#include "os/os_time.h"

#include <dbus/dbus.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WLG_BUS_NAME "org.displayxr.WindowGeometry"
#define WLG_OBJ_PATH "/org/displayxr/WindowGeometry"
#define WLG_IFACE "org.displayxr.WindowGeometry1"
#define WLG_MATCH_RULE "type='signal',interface='" WLG_IFACE "',member='WindowsChanged'"

//! Highest payload schema version this consumer understands. Additive changes
//! keep the version; anything that changes the MEANING of an existing field
//! bumps it (and older consumers then refuse the payload rather than misread it).
#define WLG_SCHEMA_VERSION_MAX 1

// One entry per published window we care about (own-PID filter is applied at
// query time, not parse time, so the cache mirrors the full snapshot).
struct wlg_window
{
	int32_t pid;
	bool focus;
	int32_t x, y;
	int32_t w, h;
	float scale;
};

#define WLG_MAX_WINDOWS 64

struct comp_vk_native_wl_geom
{
	DBusConnection *conn; //!< private session-bus connection, NULL when unavailable

	struct wlg_window windows[WLG_MAX_WINDOWS];
	uint32_t window_count;
	bool have_snapshot;      //!< at least one successfully parsed payload
	bool warned_unavailable; //!< one-shot WARN guard (extension missing)
	bool warned_scale;       //!< one-shot WARN guard (monitor scale != 1.0)
	bool warned_schema;      //!< one-shot WARN guard (publisher schema too new)
	int64_t next_retry_ns;   //!< earliest monotonic time for the next blocking GetWindows retry
};


/*
 *
 * Payload parsing.
 *
 */

static void
wlg_parse_snapshot(struct comp_vk_native_wl_geom *g, const char *json)
{
	cJSON *root = cJSON_Parse(json);
	if (root == NULL) {
		return;
	}

	// Schema gate. The publisher is a SHARED asset that a vendor runtime
	// package may ship (see the packaging contract in
	// docs/specs/runtime/wayland-window-geometry.md), so the publisher on a
	// given box is NOT necessarily the one this runtime shipped with. Within a
	// version the schema is additive-only, so a newer minor publisher is safe
	// to read (unknown fields are simply not looked up); a MAJOR bump means
	// changed semantics — e.g. physical instead of logical pixels — which would
	// weave at a silently wrong phase. Refuse it and stay display-scoped.
	int version = 0;
	if (!u_json_get_int(u_json_get(root, "version"), &version)) {
		version = 1; // pre-versioning publisher — treat as v1
	}
	if (version > WLG_SCHEMA_VERSION_MAX) {
		if (!g->warned_schema) {
			U_LOG_W("wl_geom: geometry service speaks schema v%d, this runtime understands up to v%d "
			        "— ignoring it and weaving display-scoped. Update the runtime.",
			        version, WLG_SCHEMA_VERSION_MAX);
			g->warned_schema = true;
		}
		cJSON_Delete(root);
		return;
	}

	const cJSON *windows = u_json_get(root, "windows");
	if (!cJSON_IsArray(windows)) {
		cJSON_Delete(root);
		return;
	}

	uint32_t count = 0;
	const cJSON *win = NULL;
	cJSON_ArrayForEach(win, windows)
	{
		if (count >= WLG_MAX_WINDOWS) {
			break;
		}

		int pid = 0;
		if (!u_json_get_int(u_json_get(win, "pid"), &pid) || pid <= 0) {
			continue;
		}

		const cJSON *frame = u_json_get(win, "frame");
		if (!cJSON_IsArray(frame) || cJSON_GetArraySize(frame) != 4) {
			continue;
		}

		struct wlg_window *out = &g->windows[count];
		out->pid = (int32_t)pid;
		out->x = (int32_t)cJSON_GetArrayItem(frame, 0)->valuedouble;
		out->y = (int32_t)cJSON_GetArrayItem(frame, 1)->valuedouble;
		out->w = (int32_t)cJSON_GetArrayItem(frame, 2)->valuedouble;
		out->h = (int32_t)cJSON_GetArrayItem(frame, 3)->valuedouble;

		bool focus = false;
		u_json_get_bool(u_json_get(win, "focus"), &focus);
		out->focus = focus;

		out->scale = 1.0f;
		const cJSON *monitor = u_json_get(win, "monitor");
		if (cJSON_IsObject(monitor)) {
			float scale = 1.0f;
			if (u_json_get_float(u_json_get(monitor, "scale"), &scale) && scale > 0.0f) {
				out->scale = scale;
			}
		}

		count++;
	}

	g->window_count = count;
	g->have_snapshot = true;
	cJSON_Delete(root);
}

//! Drain pending bus messages without blocking; keep the latest snapshot.
static void
wlg_pump(struct comp_vk_native_wl_geom *g)
{
	if (g->conn == NULL) {
		return;
	}

	dbus_connection_read_write(g->conn, 0);

	DBusMessage *msg = NULL;
	while ((msg = dbus_connection_pop_message(g->conn)) != NULL) {
		if (dbus_message_is_signal(msg, WLG_IFACE, "WindowsChanged")) {
			const char *json = NULL;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) &&
			    json != NULL) {
				wlg_parse_snapshot(g, json);
			}
		}
		dbus_message_unref(msg);
	}
}

//! Synchronous GetWindows — used once at create (and as a lazy retry when the
//! extension wasn't up yet), short timeout so a missing service can't stall a
//! frame for long.
static bool
wlg_request_snapshot(struct comp_vk_native_wl_geom *g, int timeout_ms)
{
	if (g->conn == NULL) {
		return false;
	}

	DBusMessage *call = dbus_message_new_method_call(WLG_BUS_NAME, WLG_OBJ_PATH, WLG_IFACE, "GetWindows");
	if (call == NULL) {
		return false;
	}

	DBusMessage *reply = dbus_connection_send_with_reply_and_block(g->conn, call, timeout_ms, NULL);
	dbus_message_unref(call);
	if (reply == NULL) {
		return false;
	}

	const char *json = NULL;
	bool ok = false;
	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) && json != NULL) {
		wlg_parse_snapshot(g, json);
		ok = g->have_snapshot;
	}
	dbus_message_unref(reply);
	return ok;
}


/*
 *
 * Public API.
 *
 */

struct comp_vk_native_wl_geom *
comp_vk_native_wl_geom_create(void)
{
	struct comp_vk_native_wl_geom *g = U_TYPED_CALLOC(struct comp_vk_native_wl_geom);
	if (g == NULL) {
		return NULL;
	}

	DBusError err;
	dbus_error_init(&err);
	// Private connection: shared connections must never be closed, and the
	// compositor owns this one's lifetime.
	g->conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (g->conn == NULL) {
		U_LOG_W("wl_geom: no session D-Bus (%s) — Wayland windowed weaving unavailable, staying display-scoped",
		        dbus_error_is_set(&err) ? err.message : "unknown");
		dbus_error_free(&err);
		free(g);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(g->conn, FALSE);

	dbus_bus_add_match(g->conn, WLG_MATCH_RULE, &err);
	if (dbus_error_is_set(&err)) {
		U_LOG_W("wl_geom: add_match failed (%s)", err.message);
		dbus_error_free(&err);
	}
	dbus_connection_flush(g->conn);

	if (!wlg_request_snapshot(g, 200)) {
		U_LOG_W("wl_geom: geometry service " WLG_BUS_NAME
		        " not answering — is the window-geometry@displayxr.org GNOME Shell extension "
		        "enabled? Weaving stays display-scoped until it appears.");
		g->warned_unavailable = true;
	} else {
		U_LOG_W("wl_geom: compositor geometry service connected (%u windows) — "
		        "Wayland windowed weaving active",
		        g->window_count);
	}

	return g;
}

bool
comp_vk_native_wl_geom_get_window_rect(struct comp_vk_native_wl_geom *g,
                                       int32_t *out_left_px,
                                       int32_t *out_top_px,
                                       uint32_t *out_width_px,
                                       uint32_t *out_height_px,
                                       float *out_scale)
{
	if (g == NULL || g->conn == NULL) {
		return false;
	}

	wlg_pump(g);

	if (!g->have_snapshot) {
		// Extension may have been enabled after we started. Retry the
		// blocking snapshot at most every 5 s so a missing service costs
		// one bounded call occasionally, never per frame (the common miss
		// path is the fast org.freedesktop.DBus error reply anyway, the
		// 25 ms cap is the worst case).
		const int64_t now_ns = os_monotonic_get_ns();
		if (now_ns < g->next_retry_ns) {
			return false;
		}
		g->next_retry_ns = now_ns + (int64_t)5 * 1000 * 1000 * 1000;
		wlg_request_snapshot(g, 25);
		if (!g->have_snapshot) {
			return false;
		}
	}

	const int32_t pid = (int32_t)getpid();
	const struct wlg_window *best = NULL;
	for (uint32_t i = 0; i < g->window_count; i++) {
		const struct wlg_window *w = &g->windows[i];
		if (w->pid != pid || w->w <= 0 || w->h <= 0) {
			continue;
		}
		if (best == NULL || (w->focus && !best->focus) ||
		    (w->focus == best->focus && (int64_t)w->w * w->h > (int64_t)best->w * best->h)) {
			best = w;
		}
	}
	if (best == NULL) {
		return false;
	}

	if (best->scale != 1.0f && !g->warned_scale) {
		U_LOG_W("wl_geom: monitor scale %.2f != 1.0 — logical != physical pixels, weave phase will "
		        "be wrong; set the 3D display to 100%% scale",
		        best->scale);
		g->warned_scale = true;
	}

	if (out_left_px != NULL) {
		*out_left_px = best->x;
	}
	if (out_top_px != NULL) {
		*out_top_px = best->y;
	}
	if (out_width_px != NULL) {
		*out_width_px = (uint32_t)best->w;
	}
	if (out_height_px != NULL) {
		*out_height_px = (uint32_t)best->h;
	}
	if (out_scale != NULL) {
		*out_scale = best->scale;
	}
	return true;
}

void
comp_vk_native_wl_geom_destroy(struct comp_vk_native_wl_geom **g_ptr)
{
	if (g_ptr == NULL || *g_ptr == NULL) {
		return;
	}
	struct comp_vk_native_wl_geom *g = *g_ptr;
	if (g->conn != NULL) {
		dbus_connection_close(g->conn);
		dbus_connection_unref(g->conn);
	}
	free(g);
	*g_ptr = NULL;
}
