// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  libdbus client of the compositor's drag lattice. See the header.
 */

#include "dxr_wl_placement.h"

#ifdef DXR_APP_HAVE_DBUS
#include <dbus/dbus.h>
#include <unistd.h> // getpid, to pick our own DragLatticeNeeded out of the stream
#endif

#define WLP_BUS "org.displayxr.WindowGeometry"
#define WLP_PATH "/org/displayxr/WindowPlacement"
#define WLP_IFACE "org.displayxr.WindowPlacement1"
#define WLP_NEEDED_MATCH "type='signal',interface='" WLP_IFACE "',member='DragLatticeNeeded'"
//! GetPlacementCapabilities bit: the publisher can constrain a drag.
#define WLP_CAP_DRAG_LATTICE 1u

DxrWlPlacement::~DxrWlPlacement()
{
	disconnect();
}

#ifdef DXR_APP_HAVE_DBUS

bool
DxrWlPlacement::connect()
{
	if (m_conn != nullptr) {
		return m_lattice;
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

	dbus_bus_add_match(conn, WLP_NEEDED_MATCH, &err);
	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
	}
	dbus_connection_flush(conn);

	// One bounded probe at start-up, so a press never has to find out.
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "GetPlacementCapabilities");
	if (call == nullptr) {
		m_why = "out of memory";
		return false;
	}
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 200, &err);
	dbus_message_unref(call);
	if (reply == nullptr) {
		m_why = "the geometry extension is older than version 6 (no drag lattice) — the title bar drags "
		        "through the compositor unsnapped";
		dbus_error_free(&err);
		return false;
	}
	dbus_uint32_t caps = 0;
	dbus_message_get_args(reply, nullptr, DBUS_TYPE_UINT32, &caps, DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	m_lattice = (caps & WLP_CAP_DRAG_LATTICE) != 0;
	m_why = m_lattice ? "ready — the compositor can constrain a drag to the interlace lattice"
	                  : "the compositor has no Meta.ExternalConstraint, so a drag cannot be constrained — the "
	                    "title bar drags unsnapped";
	return m_lattice;
}

bool
DxrWlPlacement::set_drag_lattice(bool extend,
                                 int32_t cell,
                                 int32_t min_dx,
                                 int32_t min_dy,
                                 int32_t max_dx,
                                 int32_t max_dy,
                                 const std::vector<int32_t> &dx,
                                 const std::vector<int32_t> &dy,
                                 int32_t *start_x,
                                 int32_t *start_y)
{
	if (!m_lattice || m_conn == nullptr || dx.size() != dy.size()) {
		return false;
	}
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "SetDragLattice");
	if (call == nullptr) {
		return false;
	}
	dbus_uint32_t pid = 0; // 0 = "me"; the publisher takes the PID from the bus
	dbus_bool_t ext = extend ? TRUE : FALSE;
	dbus_int32_t c = cell, a = min_dx, b = min_dy, e = max_dx, f = max_dy;
	const dbus_int32_t *px = dx.data(), *py = dy.data();
	const int n = (int)dx.size();
	dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_BOOLEAN, &ext, DBUS_TYPE_INT32, &c,
	                         DBUS_TYPE_INT32, &a, DBUS_TYPE_INT32, &b, DBUS_TYPE_INT32, &e, DBUS_TYPE_INT32, &f,
	                         DBUS_TYPE_ARRAY, DBUS_TYPE_INT32, &px, n, DBUS_TYPE_ARRAY, DBUS_TYPE_INT32, &py, n,
	                         DBUS_TYPE_INVALID);
	// Bounded and blocking, on purpose: this runs at the press, BEFORE
	// xdg_toplevel.move starts the grab, and the grab must not begin before
	// the compositor holds the table. It never runs during a grab.
	DBusMessage *reply = dbus_connection_send_with_reply_and_block((DBusConnection *)m_conn, call, 100, nullptr);
	dbus_message_unref(call);
	if (reply == nullptr) {
		return false;
	}
	dbus_bool_t ok = FALSE;
	dbus_int32_t sx = 0, sy = 0;
	dbus_message_get_args(reply, nullptr, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INT32, &sx, DBUS_TYPE_INT32, &sy,
	                      DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	if (start_x != nullptr) {
		*start_x = (int32_t)sx;
	}
	if (start_y != nullptr) {
		*start_y = (int32_t)sy;
	}
	return ok == TRUE;
}

bool
DxrWlPlacement::test_move_to(int32_t x, int32_t y)
{
	if (m_conn == nullptr) {
		return false;
	}
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "MoveWindow");
	if (call == nullptr) {
		return false;
	}
	dbus_uint32_t pid = 0;
	dbus_int32_t ax = x, ay = y;
	dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INT32, &ax, DBUS_TYPE_INT32, &ay,
	                         DBUS_TYPE_INVALID);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block((DBusConnection *)m_conn, call, 200, nullptr);
	dbus_message_unref(call);
	if (reply == nullptr) {
		return false;
	}
	dbus_bool_t moved = FALSE;
	dbus_message_get_args(reply, nullptr, DBUS_TYPE_BOOLEAN, &moved, DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	return moved == TRUE;
}

bool
DxrWlPlacement::poll_needed(int32_t *dx, int32_t *dy)
{
	if (m_conn == nullptr) {
		return false;
	}
	DBusConnection *conn = (DBusConnection *)m_conn;
	dbus_connection_read_write(conn, 0);
	bool got = false;
	DBusMessage *msg = nullptr;
	while ((msg = dbus_connection_pop_message(conn)) != nullptr) {
		if (dbus_message_is_signal(msg, WLP_IFACE, "DragLatticeNeeded")) {
			dbus_uint32_t pid = 0;
			dbus_int32_t x = 0, y = 0;
			if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INT32, &x,
			                          DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID) &&
			    (int32_t)pid == (int32_t)getpid()) {
				*dx = (int32_t)x; // the newest request wins
				*dy = (int32_t)y;
				got = true;
			}
		}
		dbus_message_unref(msg);
	}
	return got;
}

void
DxrWlPlacement::disconnect()
{
	if (m_conn != nullptr) {
		dbus_connection_close((DBusConnection *)m_conn);
		dbus_connection_unref((DBusConnection *)m_conn);
		m_conn = nullptr;
	}
	m_lattice = false;
}

#else // !DXR_APP_HAVE_DBUS

bool
DxrWlPlacement::connect()
{
	m_why = "this build has no libdbus (install libdbus-1-dev and reconfigure)";
	return false;
}

bool
DxrWlPlacement::set_drag_lattice(bool extend,
                                 int32_t cell,
                                 int32_t min_dx,
                                 int32_t min_dy,
                                 int32_t max_dx,
                                 int32_t max_dy,
                                 const std::vector<int32_t> &dx,
                                 const std::vector<int32_t> &dy,
                                 int32_t *start_x,
                                 int32_t *start_y)
{
	(void)start_x;
	(void)start_y;
	(void)extend;
	(void)cell;
	(void)min_dx;
	(void)min_dy;
	(void)max_dx;
	(void)max_dy;
	(void)dx;
	(void)dy;
	return false;
}

bool
DxrWlPlacement::poll_needed(int32_t *dx, int32_t *dy)
{
	(void)dx;
	(void)dy;
	return false;
}

bool
DxrWlPlacement::test_move_to(int32_t x, int32_t y)
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
