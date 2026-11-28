// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Serving org.displayxr.WindowGeometry1 from a stand-alone bridge.
 * @ingroup targets
 */

#include "wgeom_bus.h"

#include <dbus/dbus.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wgeom_bus
{
	DBusConnection *conn;
	char log_name[64];
	char *snapshot; //!< malloc'd; NULL = none yet
	char *introspect_xml;
	wgeom_bus_method_fn extra;
	void *user;
	bool lost;
};

static const char *INTROSPECT_HEAD =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"" WGEOM_BUS_IFACE
    "\">\n"
    "    <method name=\"GetWindows\"><arg type=\"s\" direction=\"out\"/></method>\n"
    "    <signal name=\"WindowsChanged\"><arg type=\"s\"/></signal>\n"
    "  </interface>\n";

static const char *INTROSPECT_TAIL =
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method>\n"
    "  </interface>\n"
    "</node>\n";

static void
reply_string(DBusConnection *conn, DBusMessage *msg, const char *s)
{
	DBusMessage *r = dbus_message_new_method_return(msg);
	if (r == NULL) {
		return;
	}
	dbus_message_append_args(r, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
	dbus_connection_send(conn, r, NULL);
	dbus_message_unref(r);
}

static DBusHandlerResult
on_object_message(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
	struct wgeom_bus *bus = user_data;
	if (dbus_message_is_method_call(msg, WGEOM_BUS_IFACE, "GetWindows")) {
		reply_string(conn, msg, bus->snapshot != NULL ? bus->snapshot : WGEOM_EMPTY_SNAPSHOT);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		reply_string(conn, msg, bus->introspect_xml);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (bus->extra != NULL && bus->extra(bus, conn, msg, bus->user)) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

//! NameLost: a real publisher replaced us.
static DBusHandlerResult
on_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
	(void)conn;
	struct wgeom_bus *bus = user_data;
	if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameLost")) {
		const char *name = NULL;
		if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID) && name != NULL &&
		    strcmp(name, WGEOM_BUS_NAME) == 0) {
			fprintf(stderr, "%s: another publisher took " WGEOM_BUS_NAME " — exiting\n", bus->log_name);
			bus->lost = true;
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

struct wgeom_bus *
wgeom_bus_open(const char *log_name, const char *extra_introspect_xml, wgeom_bus_method_fn extra, void *user)
{
	struct wgeom_bus *bus = calloc(1, sizeof(*bus));
	if (bus == NULL) {
		return NULL;
	}
	snprintf(bus->log_name, sizeof(bus->log_name), "%s", log_name != NULL ? log_name : "wgeom-bridge");
	bus->extra = extra;
	bus->user = user;

	const char *mid = extra_introspect_xml != NULL ? extra_introspect_xml : "";
	const size_t len = strlen(INTROSPECT_HEAD) + strlen(mid) + strlen(INTROSPECT_TAIL) + 1;
	bus->introspect_xml = malloc(len);
	if (bus->introspect_xml == NULL) {
		free(bus);
		return NULL;
	}
	snprintf(bus->introspect_xml, len, "%s%s%s", INTROSPECT_HEAD, mid, INTROSPECT_TAIL);

	DBusError err;
	dbus_error_init(&err);
	bus->conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (bus->conn == NULL) {
		fprintf(stderr, "%s: no session bus: %s\n", bus->log_name, err.message != NULL ? err.message : "?");
		dbus_error_free(&err);
		wgeom_bus_close(&bus);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(bus->conn, FALSE);

	const int rn = dbus_bus_request_name(bus->conn, WGEOM_BUS_NAME,
	                                     DBUS_NAME_FLAG_DO_NOT_QUEUE | DBUS_NAME_FLAG_ALLOW_REPLACEMENT, &err);
	if (rn != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr, "%s: " WGEOM_BUS_NAME " is %s — exiting\n", bus->log_name,
		        dbus_error_is_set(&err) ? err.message : "owned by another publisher");
		dbus_error_free(&err);
		wgeom_bus_close(&bus);
		return NULL;
	}

	const DBusObjectPathVTable vtable = {.message_function = on_object_message};
	if (!dbus_connection_register_object_path(bus->conn, WGEOM_BUS_PATH, &vtable, bus) ||
	    !dbus_connection_add_filter(bus->conn, on_filter, bus, NULL)) {
		fprintf(stderr, "%s: out of memory registering " WGEOM_BUS_PATH "\n", bus->log_name);
		wgeom_bus_close(&bus);
		return NULL;
	}
	return bus;
}

bool
wgeom_bus_publish(struct wgeom_bus *bus, const char *snapshot)
{
	if (bus == NULL || snapshot == NULL) {
		return false;
	}
	if (bus->snapshot != NULL && strcmp(bus->snapshot, snapshot) == 0) {
		return false;
	}
	char *copy = strdup(snapshot);
	if (copy == NULL) {
		return false;
	}
	free(bus->snapshot);
	bus->snapshot = copy;

	DBusMessage *sig = dbus_message_new_signal(WGEOM_BUS_PATH, WGEOM_BUS_IFACE, "WindowsChanged");
	if (sig != NULL) {
		const char *s = bus->snapshot;
		dbus_message_append_args(sig, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
		dbus_connection_send(bus->conn, sig, NULL);
		dbus_message_unref(sig);
		dbus_connection_flush(bus->conn);
	}
	return true;
}

int
wgeom_bus_fd(struct wgeom_bus *bus)
{
	int fd = -1;
	if (bus == NULL || bus->conn == NULL || !dbus_connection_get_unix_fd(bus->conn, &fd)) {
		return -1;
	}
	return fd;
}

bool
wgeom_bus_dispatch(struct wgeom_bus *bus, int timeout_ms)
{
	if (bus == NULL || bus->conn == NULL) {
		return false;
	}
	if (!dbus_connection_read_write_dispatch(bus->conn, timeout_ms)) {
		return false; // disconnected
	}
	while (!bus->lost && dbus_connection_get_dispatch_status(bus->conn) == DBUS_DISPATCH_DATA_REMAINS) {
		dbus_connection_dispatch(bus->conn);
	}
	return !bus->lost;
}

void
wgeom_bus_close(struct wgeom_bus **bus_ptr)
{
	if (bus_ptr == NULL || *bus_ptr == NULL) {
		return;
	}
	struct wgeom_bus *bus = *bus_ptr;
	if (bus->conn != NULL) {
		dbus_connection_unref(bus->conn); // shared connection: never close it
	}
	free(bus->snapshot);
	free(bus->introspect_xml);
	free(bus);
	*bus_ptr = NULL;
}
