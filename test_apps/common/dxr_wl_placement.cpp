// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  libdbus client of org.displayxr.WindowPlacement1. See the header.
 */

#include "dxr_wl_placement.h"

#include <cstdio>
#include <unistd.h> // getpid, to filter WindowMoved to our own window

#ifdef DXR_APP_HAVE_DBUS
#include <dbus/dbus.h>
#endif

#define PL_INFO(fmt, ...)                                                                                              \
	do {                                                                                                           \
		fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__);                                                   \
		fflush(stdout);                                                                                        \
	} while (0)

#define WLP_BUS "org.displayxr.WindowGeometry"
#define WLP_PATH "/org/displayxr/WindowPlacement"
#define WLP_IFACE "org.displayxr.WindowPlacement1"
#define WLP_MOVED_MATCH "type='signal',interface='" WLP_IFACE "',member='WindowMoved'"

DxrWlPlacement::~DxrWlPlacement()
{
	disconnect();
}

#ifdef DXR_APP_HAVE_DBUS

bool
DxrWlPlacement::connect()
{
	if (m_conn != nullptr) {
		return m_available;
	}
	DBusError err;
	dbus_error_init(&err);
	// Private connection: the app owns its lifetime, and a shared one must
	// never be closed.
	DBusConnection *conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (conn == nullptr) {
		m_why = "no session bus";
		dbus_error_free(&err);
		return false;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE);
	m_conn = conn;

	// WindowMoved carries the ACHIEVED position after every move. Subscribed
	// before the probe below, so no report can be missed.
	dbus_bus_add_match(conn, WLP_MOVED_MATCH, &err);
	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
	}
	dbus_connection_flush(conn);

	/*
	 * One bounded probe at start-up, so the drag path never has to discover
	 * mid-gesture what the publisher can do. GetWindowOrigin is the probe on
	 * purpose: it exists only alongside the WindowMoved signal, and WITHOUT
	 * that signal a client cannot observe whether its moves are being applied
	 * — which is the difference between a drag and a runaway. (Measured: an
	 * open loop asked for +1278 logical px while the window sat clamped.)
	 */
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "GetWindowOrigin");
	if (call == nullptr) {
		m_why = "out of memory";
		return false;
	}
	dbus_uint32_t pid = 0; // 0 = "me"; the publisher takes the PID from the bus
	dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 200, &err);
	dbus_message_unref(call);
	if (reply == nullptr) {
		m_why = "the geometry extension is older than version 6 (no WindowPlacement1.GetWindowOrigin / "
		        "WindowMoved), so a client cannot see whether its moves land — the title bar keeps the "
		        "compositor's own drag";
		dbus_error_free(&err);
		return false;
	}
	dbus_message_unref(reply);
	m_available = true;
	m_why = "ready (version 6+: moves are reported back, so a drag can close its loop)";
	return true;
}

bool
DxrWlPlacement::fetch_origin(int32_t *x, int32_t *y)
{
	if (m_conn == nullptr || !m_available) {
		return false;
	}
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "GetWindowOrigin");
	if (call == nullptr) {
		return false;
	}
	dbus_uint32_t pid = 0;
	dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block((DBusConnection *)m_conn, call, 100, nullptr);
	dbus_message_unref(call);
	if (reply == nullptr) {
		return false;
	}
	dbus_int32_t rx = 0, ry = 0;
	dbus_bool_t ok = FALSE;
	const bool got = dbus_message_get_args(reply, nullptr, DBUS_TYPE_INT32, &rx, DBUS_TYPE_INT32, &ry,
	                                       DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID) == TRUE &&
	                 ok == TRUE;
	dbus_message_unref(reply);
	if (!got) {
		return false;
	}
	m_obs_x = (int32_t)rx;
	m_obs_y = (int32_t)ry;
	m_have_obs = true;
	m_seq++;
	if (x != nullptr) {
		*x = m_obs_x;
	}
	if (y != nullptr) {
		*y = m_obs_y;
	}
	return true;
}

void
DxrWlPlacement::poll()
{
	if (m_conn == nullptr) {
		return;
	}
	DBusConnection *conn = (DBusConnection *)m_conn;
	dbus_connection_read_write(conn, 0);
	DBusMessage *msg = NULL;
	while ((msg = dbus_connection_pop_message(conn)) != NULL) {
		if (dbus_message_is_signal(msg, WLP_IFACE, "WindowMoved")) {
			dbus_uint32_t pid = 0;
			dbus_int32_t x = 0, y = 0;
			dbus_bool_t applied = FALSE;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INT32, &x,
			                          DBUS_TYPE_INT32, &y, DBUS_TYPE_BOOLEAN, &applied,
			                          DBUS_TYPE_INVALID) &&
			    (int32_t)pid == (int32_t)getpid()) {
				m_reports++;
				if (applied == TRUE) {
					m_obs_x = (int32_t)x;
					m_obs_y = (int32_t)y;
					m_have_obs = true;
					m_seq++;
				} else {
					// The publisher declined this move. Counted separately so
					// "the compositor refused" is never mistaken for "no report
					// arrived" — they call for different answers.
					m_refusals++;
				}
			}
		}
		dbus_message_unref(msg);
	}
}

bool
DxrWlPlacement::observed(int32_t *x, int32_t *y) const
{
	if (!m_have_obs || x == NULL || y == NULL) {
		return false;
	}
	*x = m_obs_x;
	*y = m_obs_y;
	return true;
}

bool
DxrWlPlacement::move_by(int32_t dx, int32_t dy)
{
	if (!m_available || m_conn == nullptr) {
		return false;
	}
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "MoveWindowBy");
	if (call == nullptr) {
		return false;
	}
	// No reply: this runs once per pointer motion, and a round trip in the
	// middle of the motion path is exactly the latency this design is trying
	// not to add. The window's real position is read back from the geometry
	// service anyway — the runtime feeds the weaver the LANDED position, never
	// the one we asked for.
	dbus_message_set_no_reply(call, TRUE);
	dbus_uint32_t pid = 0;
	dbus_int32_t ddx = (dbus_int32_t)dx, ddy = (dbus_int32_t)dy;
	dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INT32, &ddx, DBUS_TYPE_INT32, &ddy,
	                         DBUS_TYPE_INVALID);
	const bool sent = dbus_connection_send((DBusConnection *)m_conn, call, nullptr) == TRUE;
	dbus_message_unref(call);
	dbus_connection_flush((DBusConnection *)m_conn);
	return sent;
}

bool
DxrWlPlacement::move_to(int32_t x, int32_t y)
{
	if (!m_available || m_conn == nullptr) {
		return false;
	}
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "MoveWindow");
	if (call == nullptr) {
		return false;
	}
	// ABSOLUTE and fire-and-forget. Re-sending an absolute target is
	// idempotent: there is nothing to accumulate, so a request that raced a
	// report cannot compound the way a relative step did (measured: relative
	// steps against asynchronous reports oscillated to 1.4k px off the
	// pointer on hardware).
	dbus_message_set_no_reply(call, TRUE);
	dbus_uint32_t pid = 0;
	dbus_int32_t ax = (dbus_int32_t)x, ay = (dbus_int32_t)y;
	dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INT32, &ax, DBUS_TYPE_INT32, &ay,
	                         DBUS_TYPE_INVALID);
	const bool sent = dbus_connection_send((DBusConnection *)m_conn, call, nullptr) == TRUE;
	dbus_message_unref(call);
	dbus_connection_flush((DBusConnection *)m_conn);
	return sent;
}

void
DxrWlPlacement::disconnect()
{
	if (m_conn != nullptr) {
		dbus_connection_close((DBusConnection *)m_conn);
		dbus_connection_unref((DBusConnection *)m_conn);
		m_conn = nullptr;
	}
	m_available = false;
}

#else // !DXR_APP_HAVE_DBUS

bool
DxrWlPlacement::connect()
{
	m_why = "this build has no libdbus (install libdbus-1-dev and reconfigure)";
	return false;
}

bool
DxrWlPlacement::move_by(int32_t dx, int32_t dy)
{
	(void)dx;
	(void)dy;
	return false;
}

void
DxrWlPlacement::poll()
{
}

bool
DxrWlPlacement::move_to(int32_t x, int32_t y)
{
	(void)x;
	(void)y;
	return false;
}

bool
DxrWlPlacement::fetch_origin(int32_t *x, int32_t *y)
{
	(void)x;
	(void)y;
	return false;
}

bool
DxrWlPlacement::observed(int32_t *x, int32_t *y) const
{
	(void)x;
	(void)y;
	return false;
}

void
DxrWlPlacement::disconnect()
{
}

#endif // DXR_APP_HAVE_DBUS

const char *
DxrWlPlacement::describe() const
{
	return m_why;
}
