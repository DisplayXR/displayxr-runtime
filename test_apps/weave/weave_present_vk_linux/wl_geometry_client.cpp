// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  See wl_geometry_client.h. Payload parsing mirrors the runtime's
 *         comp_vk_native_wl_geom.c (schema v1, additive fields).
 */

#include "wl_geometry_client.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <unistd.h>

#ifdef DXR_PRESENT_HAVE_DBUS
#include "cJSON.h"
#include <dbus/dbus.h>

#define WLG_BUS_NAME "org.displayxr.WindowGeometry"
#define WLG_OBJ_PATH "/org/displayxr/WindowGeometry"
#define WLG_IFACE "org.displayxr.WindowGeometry1"
#define WLG_PLACEMENT_PATH "/org/displayxr/WindowPlacement"
#define WLG_PLACEMENT_IFACE "org.displayxr.WindowPlacement1"
#define WLG_MATCH_RULE "type='signal',interface='" WLG_IFACE "',member='WindowsChanged'"
#endif

namespace {

struct Win
{
	int32_t pid = 0;
	WlOwnWindow w;
};

struct State
{
	std::vector<Win> windows;
	bool have_snapshot = false;
};

int64_t
now_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

} // namespace

WlGeometryClient::~WlGeometryClient()
{
	disconnect();
}

void
WlGeometryClient::disconnect()
{
#ifdef DXR_PRESENT_HAVE_DBUS
	if (m_conn != nullptr) {
		dbus_connection_close((DBusConnection *)m_conn);
		dbus_connection_unref((DBusConnection *)m_conn);
		m_conn = nullptr;
	}
#endif
	delete (State *)m_state;
	m_state = nullptr;
}

#ifdef DXR_PRESENT_HAVE_DBUS

static bool
json_int(const cJSON *o, const char *k, int32_t *out)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	if (!cJSON_IsNumber(v)) {
		return false;
	}
	*out = (int32_t)v->valuedouble;
	return true;
}

static bool
json_rect(const cJSON *o, const char *k, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
	const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
	if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 4) {
		return false;
	}
	*x = (int32_t)cJSON_GetArrayItem(a, 0)->valuedouble;
	*y = (int32_t)cJSON_GetArrayItem(a, 1)->valuedouble;
	*w = (int32_t)cJSON_GetArrayItem(a, 2)->valuedouble;
	*h = (int32_t)cJSON_GetArrayItem(a, 3)->valuedouble;
	return true;
}

void
WlGeometryClient::parse(const char *json)
{
	cJSON *root = cJSON_Parse(json);
	if (root == nullptr) {
		return;
	}
	int32_t version = 1;
	json_int(root, "version", &version);
	const cJSON *windows = cJSON_GetObjectItemCaseSensitive(root, "windows");
	if (version > 1 || !cJSON_IsArray(windows)) {
		// A major bump changes field meanings: refuse rather than misread.
		cJSON_Delete(root);
		return;
	}
	State *st = (State *)m_state;
	st->windows.clear();
	const cJSON *win = nullptr;
	cJSON_ArrayForEach(win, windows)
	{
		Win e;
		if (!json_int(win, "pid", &e.pid) || e.pid <= 0 ||
		    !json_rect(win, "frame", &e.w.frame_x, &e.w.frame_y, &e.w.frame_w, &e.w.frame_h)) {
			continue;
		}
		if (!json_rect(win, "buffer", &e.w.buffer_x, &e.w.buffer_y, &e.w.buffer_w, &e.w.buffer_h)) {
			e.w.buffer_w = e.w.buffer_h = 0;
		}
		e.w.focus = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(win, "focus"));
		const cJSON *moving = cJSON_GetObjectItemCaseSensitive(win, "moving");
		if (cJSON_IsBool(moving)) {
			e.w.have_moving = true;
			e.w.moving = cJSON_IsTrue(moving);
		}
		e.w.lattice_drop = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(win, "lattice_drop"));
		const cJSON *mon = cJSON_GetObjectItemCaseSensitive(win, "monitor");
		if (cJSON_IsObject(mon) && json_int(mon, "x", &e.w.mon_x) && json_int(mon, "y", &e.w.mon_y) &&
		    json_int(mon, "w", &e.w.mon_w) && json_int(mon, "h", &e.w.mon_h) && e.w.mon_w > 0 &&
		    e.w.mon_h > 0) {
			e.w.have_monitor = true;
			const cJSON *s = cJSON_GetObjectItemCaseSensitive(mon, "scale");
			if (cJSON_IsNumber(s) && s->valuedouble > 0.0) {
				e.w.mon_scale = s->valuedouble;
			}
		}
		st->windows.push_back(e);
	}
	st->have_snapshot = true;
	cJSON_Delete(root);
}

bool
WlGeometryClient::request_snapshot(int timeout_ms)
{
	DBusConnection *c = (DBusConnection *)m_conn;
	DBusMessage *call = dbus_message_new_method_call(WLG_BUS_NAME, WLG_OBJ_PATH, WLG_IFACE, "GetWindows");
	if (call == nullptr) {
		return false;
	}
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(c, call, timeout_ms, nullptr);
	dbus_message_unref(call);
	if (reply == nullptr) {
		return false;
	}
	const char *json = nullptr;
	bool ok = false;
	if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) && json != nullptr) {
		parse(json);
		ok = ((State *)m_state)->have_snapshot;
	}
	dbus_message_unref(reply);
	return ok;
}

bool
WlGeometryClient::connect()
{
	disconnect();
	m_state = new State();
	DBusError err;
	dbus_error_init(&err);
	DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (c == nullptr) {
		fprintf(stderr, "[weave_present] wl_geom: no session bus (%s)\n", err.message ? err.message : "?");
		dbus_error_free(&err);
		return false;
	}
	dbus_connection_set_exit_on_disconnect(c, FALSE);
	dbus_bus_add_match(c, WLG_MATCH_RULE, nullptr);
	m_conn = c;
	if (!request_snapshot(200)) {
		fprintf(stderr, "[weave_present] wl_geom: " WLG_BUS_NAME
		                " did not answer GetWindows — is the window-geometry@displayxr.org "
		                "GNOME Shell extension enabled (log out/in after installing)? "
		                "Retrying every 5 s.\n");
		m_next_retry_ns = now_ns() + 5000000000LL;
	}
	return true;
}

bool
WlGeometryClient::own_window(WlOwnWindow *out)
{
	if (m_conn == nullptr) {
		return false;
	}
	DBusConnection *c = (DBusConnection *)m_conn;
	dbus_connection_read_write(c, 0);
	DBusMessage *msg = nullptr;
	while ((msg = dbus_connection_pop_message(c)) != nullptr) {
		if (dbus_message_is_signal(msg, WLG_IFACE, "WindowsChanged")) {
			const char *json = nullptr;
			if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) &&
			    json != nullptr) {
				parse(json);
			}
		}
		dbus_message_unref(msg);
	}
	State *st = (State *)m_state;
	if (!st->have_snapshot) {
		const int64_t now = now_ns();
		if (now < m_next_retry_ns) {
			return false;
		}
		m_next_retry_ns = now + 5000000000LL;
		if (!request_snapshot(25)) {
			return false;
		}
	}
	const int32_t pid = (int32_t)getpid();
	const Win *best = nullptr;
	for (const Win &e : st->windows) {
		if (e.pid != pid || e.w.frame_w <= 0 || e.w.frame_h <= 0) {
			continue;
		}
		if (best == nullptr || (e.w.focus && !best->w.focus) ||
		    (e.w.focus == best->w.focus &&
		     (int64_t)e.w.frame_w * e.w.frame_h > (int64_t)best->w.frame_w * best->w.frame_h)) {
			best = &e;
		}
	}
	if (best == nullptr) {
		return false;
	}
	*out = best->w;
	return true;
}

bool
WlGeometryClient::move_window(int32_t frame_logical_x, int32_t frame_logical_y)
{
	if (m_conn == nullptr) {
		return false;
	}
	DBusMessage *call =
	    dbus_message_new_method_call(WLG_BUS_NAME, WLG_PLACEMENT_PATH, WLG_PLACEMENT_IFACE, "MoveWindow");
	if (call == nullptr) {
		return false;
	}
	dbus_uint32_t pid = 0; // "me": the publisher takes the pid from the bus connection anyway
	dbus_int32_t x = frame_logical_x, y = frame_logical_y;
	dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y,
	                         DBUS_TYPE_INVALID);
	DBusError err;
	dbus_error_init(&err);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block((DBusConnection *)m_conn, call, 60, &err);
	dbus_message_unref(call);
	if (reply == nullptr) {
		if (!m_warned_placement) {
			m_warned_placement = true;
			fprintf(stderr,
			        "[weave_present] wl_geom: no " WLG_PLACEMENT_IFACE
			        " (%s) — a drop cannot be snapped; update the GNOME Shell "
			        "extension\n",
			        dbus_error_is_set(&err) ? err.message : "no reply");
		}
		dbus_error_free(&err);
		return false;
	}
	dbus_bool_t moved = FALSE;
	if (!dbus_message_get_args(reply, nullptr, DBUS_TYPE_BOOLEAN, &moved, DBUS_TYPE_INVALID)) {
		moved = FALSE;
	}
	dbus_message_unref(reply);
	return moved == TRUE;
}

#else // !DXR_PRESENT_HAVE_DBUS

void
WlGeometryClient::parse(const char *)
{}

bool
WlGeometryClient::request_snapshot(int)
{
	return false;
}

bool
WlGeometryClient::connect()
{
	fprintf(stderr,
	        "[weave_present] wl_geom: built without libdbus-1 — no "
	        "window geometry on Wayland\n");
	return false;
}

bool
WlGeometryClient::own_window(WlOwnWindow *)
{
	return false;
}

bool
WlGeometryClient::move_window(int32_t, int32_t)
{
	return false;
}

#endif
