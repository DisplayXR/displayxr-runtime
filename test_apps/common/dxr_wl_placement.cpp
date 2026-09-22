// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  libdbus client of org.displayxr.WindowPlacement1. See the header.
 */

#include "dxr_wl_placement.h"

#include <cstdio>

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

	// One bounded probe at start-up, so the drag path never has to discover
	// mid-gesture that the service is missing. A relative move of (0,0) is a
	// no-op the publisher answers exactly as it would answer a real one.
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "MoveWindowBy");
	if (call == nullptr) {
		m_why = "out of memory";
		return false;
	}
	dbus_uint32_t pid = 0; // 0 = "me"; the publisher takes the PID from the bus
	dbus_int32_t zero = 0;
	dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INT32, &zero, DBUS_TYPE_INT32, &zero,
	                         DBUS_TYPE_INVALID);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 200, &err);
	dbus_message_unref(call);
	if (reply == nullptr) {
		m_why = "the geometry extension has no WindowPlacement1.MoveWindowBy (version 4 or newer needed)";
		dbus_error_free(&err);
		return false;
	}
	dbus_message_unref(reply);
	m_available = true;
	m_why = "ready";
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
DxrWlPlacement::disconnect()
{
}

#endif // DXR_APP_HAVE_DBUS

const char *
DxrWlPlacement::describe() const
{
	return m_why;
}
