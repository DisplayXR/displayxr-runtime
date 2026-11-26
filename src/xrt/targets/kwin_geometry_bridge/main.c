// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-kwin-geometry-bridge: org.displayxr.WindowGeometry1 on a
 *         stock KWin, fed by the DisplayXR KWin script.
 *
 * docs/specs/runtime/wayland-window-geometry.md, "KWin script bridge". A KWin
 * script (contrib/kwin/displayxr-window-geometry) can call D-Bus but not
 * serve it, so it pushes each geometry snapshot here
 * (`org.displayxr.WindowGeometryFeed1.Publish(s)`), and this process serves
 * it under the canonical name exactly as the GNOME Shell extension does:
 *
 * - `org.displayxr.WindowGeometry1.GetWindows() -> s` (the latest snapshot;
 *   no windows until KWin has sent one);
 * - `org.displayxr.WindowGeometry1.WindowsChanged(s)` on every new snapshot.
 *
 * It is the SteamOS stopgap: stock KWin, read-only OS image, installed per
 * user. It steps aside for a real publisher: it never queues for the name,
 * allows replacement (a native KWin plug-in that claims the name with
 * REPLACE_EXISTING takes it, and this process exits on NameLost), and refuses
 * to start on any desktop but KDE, where the GNOME extension may want the
 * name. Started on demand by D-Bus activation (the per-user service file the
 * tarball's install.sh writes on KDE).
 *
 * @ingroup targets
 */

#include "kwgb_payload.h"

#include <dbus/dbus.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KWGB_BUS_NAME "org.displayxr.WindowGeometry"
#define KWGB_PATH "/org/displayxr/WindowGeometry"
#define KWGB_GEOM_IFACE "org.displayxr.WindowGeometry1"
#define KWGB_FEED_IFACE "org.displayxr.WindowGeometryFeed1"

static const char *INTROSPECT_XML =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"" KWGB_GEOM_IFACE
    "\">\n"
    "    <method name=\"GetWindows\"><arg type=\"s\" direction=\"out\"/></method>\n"
    "    <signal name=\"WindowsChanged\"><arg type=\"s\"/></signal>\n"
    "  </interface>\n"
    "  <interface name=\"" KWGB_FEED_IFACE
    "\">\n"
    "    <method name=\"Publish\"><arg type=\"s\" direction=\"in\"/></method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method>\n"
    "  </interface>\n"
    "</node>\n";

static volatile sig_atomic_t g_quit = 0;
static char *g_snapshot = NULL; //!< from kwgb_sanitize; NULL = none yet
static bool g_warned_reject = false;

static void
on_signal(int sig)
{
	(void)sig;
	g_quit = 1;
}

//! Is the caller KWin itself? Only it may feed the snapshot.
static bool
sender_is_kwin(DBusConnection *conn, DBusMessage *msg)
{
	const char *sender = dbus_message_get_sender(msg);
	if (sender == NULL) {
		return false;
	}
	DBusMessage *q = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
	                                              "org.freedesktop.DBus", "GetConnectionUnixProcessID");
	if (q == NULL) {
		return false;
	}
	dbus_message_append_args(q, DBUS_TYPE_STRING, &sender, DBUS_TYPE_INVALID);
	DBusError err;
	dbus_error_init(&err);
	DBusMessage *r = dbus_connection_send_with_reply_and_block(conn, q, 500, &err);
	dbus_message_unref(q);
	if (r == NULL) {
		dbus_error_free(&err);
		return false;
	}
	dbus_uint32_t pid = 0;
	const bool ok = dbus_message_get_args(r, &err, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);
	dbus_message_unref(r);
	dbus_error_free(&err);
	if (!ok || pid == 0) {
		return false;
	}
	char path[64];
	char comm[64] = "";
	snprintf(path, sizeof(path), "/proc/%u/comm", (unsigned)pid);
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return false;
	}
	if (fgets(comm, sizeof(comm), f) == NULL) {
		comm[0] = '\0';
	}
	fclose(f);
	return kwgb_comm_is_kwin(comm);
}

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

static void
reply_error(DBusConnection *conn, DBusMessage *msg, const char *name, const char *text)
{
	DBusMessage *r = dbus_message_new_error(msg, name, text);
	if (r == NULL) {
		return;
	}
	dbus_connection_send(conn, r, NULL);
	dbus_message_unref(r);
}

static void
handle_publish(DBusConnection *conn, DBusMessage *msg)
{
	if (!sender_is_kwin(conn, msg)) {
		reply_error(conn, msg, DBUS_ERROR_ACCESS_DENIED, "only KWin may publish window geometry");
		return;
	}
	const char *json = NULL;
	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) || json == NULL) {
		reply_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "expected one string");
		return;
	}
	char *clean = kwgb_sanitize(json);
	if (clean == NULL) {
		if (!g_warned_reject) {
			g_warned_reject = true;
			fprintf(stderr,
			        "displayxr-kwin-geometry-bridge: rejected a snapshot that is not schema version 1 "
			        "(further rejections are not logged)\n");
		}
		reply_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "not a schema-1 window geometry snapshot");
		return;
	}

	const bool changed = g_snapshot == NULL || strcmp(g_snapshot, clean) != 0;
	if (changed) {
		kwgb_free(g_snapshot);
		g_snapshot = clean;
		DBusMessage *sig = dbus_message_new_signal(KWGB_PATH, KWGB_GEOM_IFACE, "WindowsChanged");
		if (sig != NULL) {
			const char *s = g_snapshot;
			dbus_message_append_args(sig, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
			dbus_connection_send(conn, sig, NULL);
			dbus_message_unref(sig);
		}
	} else {
		kwgb_free(clean);
	}

	DBusMessage *r = dbus_message_new_method_return(msg);
	if (r != NULL) {
		dbus_connection_send(conn, r, NULL);
		dbus_message_unref(r);
	}
}

static DBusHandlerResult
on_object_message(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
	(void)user_data;
	if (dbus_message_is_method_call(msg, KWGB_GEOM_IFACE, "GetWindows")) {
		reply_string(conn, msg, g_snapshot != NULL ? g_snapshot : KWGB_EMPTY_SNAPSHOT);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_message_is_method_call(msg, KWGB_FEED_IFACE, "Publish")) {
		handle_publish(conn, msg);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		reply_string(conn, msg, INTROSPECT_XML);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

//! NameLost: a real publisher replaced us. Leave.
static DBusHandlerResult
on_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
	(void)conn;
	(void)user_data;
	if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameLost")) {
		const char *name = NULL;
		if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID) && name != NULL &&
		    strcmp(name, KWGB_BUS_NAME) == 0) {
			fprintf(stderr,
			        "displayxr-kwin-geometry-bridge: another publisher took " KWGB_BUS_NAME " — exiting\n");
			g_quit = 1;
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int
main(int argc, char **argv)
{
	if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
		printf(
		    "usage: %s\n"
		    "Serves " KWGB_GEOM_IFACE
		    " on a stock KWin, fed by the DisplayXR KWin script.\n"
		    "Started by D-Bus activation; KDE sessions only (DXR_KWGB_ANY_DESKTOP=1 overrides).\n",
		    argv[0]);
		return 0;
	}

	const char *any = getenv("DXR_KWGB_ANY_DESKTOP");
	if (!(any != NULL && strcmp(any, "1") == 0) && !kwgb_desktop_is_kde(getenv("XDG_CURRENT_DESKTOP"))) {
		fprintf(stderr,
		        "displayxr-kwin-geometry-bridge: not a KDE session (XDG_CURRENT_DESKTOP=%s) — not "
		        "taking " KWGB_BUS_NAME ", which this desktop's own publisher may need\n",
		        getenv("XDG_CURRENT_DESKTOP") != NULL ? getenv("XDG_CURRENT_DESKTOP") : "unset");
		return 0;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	DBusError err;
	dbus_error_init(&err);
	DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (conn == NULL) {
		fprintf(stderr, "displayxr-kwin-geometry-bridge: no session bus: %s\n",
		        err.message != NULL ? err.message : "?");
		dbus_error_free(&err);
		return 1;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	const int rn = dbus_bus_request_name(conn, KWGB_BUS_NAME,
	                                     DBUS_NAME_FLAG_DO_NOT_QUEUE | DBUS_NAME_FLAG_ALLOW_REPLACEMENT, &err);
	if (rn != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr, "displayxr-kwin-geometry-bridge: " KWGB_BUS_NAME " is %s — exiting\n",
		        dbus_error_is_set(&err) ? err.message : "owned by another publisher");
		dbus_error_free(&err);
		return 0;
	}

	const DBusObjectPathVTable vtable = {.message_function = on_object_message};
	if (!dbus_connection_register_object_path(conn, KWGB_PATH, &vtable, NULL) ||
	    !dbus_connection_add_filter(conn, on_filter, NULL, NULL)) {
		fprintf(stderr, "displayxr-kwin-geometry-bridge: out of memory registering " KWGB_PATH "\n");
		return 1;
	}
	fprintf(stderr, "displayxr-kwin-geometry-bridge: serving " KWGB_BUS_NAME " (geometry only)\n");

	while (!g_quit && dbus_connection_read_write_dispatch(conn, 1000)) {
	}

	kwgb_free(g_snapshot);
	g_snapshot = NULL;
	return 0;
}
