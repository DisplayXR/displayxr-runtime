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
#include "wgeom_bus.h"

#include <dbus/dbus.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KWGB_LOG "displayxr-kwin-geometry-bridge"
#define KWGB_FEED_IFACE "org.displayxr.WindowGeometryFeed1"

static const char *FEED_INTROSPECT_XML =
    "  <interface name=\"" KWGB_FEED_IFACE
    "\">\n"
    "    <method name=\"Publish\"><arg type=\"s\" direction=\"in\"/></method>\n"
    "  </interface>\n";

static volatile sig_atomic_t g_quit = 0;
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
reply_error(DBusConnection *conn, DBusMessage *msg, const char *name, const char *text)
{
	DBusMessage *r = dbus_message_new_error(msg, name, text);
	if (r == NULL) {
		return;
	}
	dbus_connection_send(conn, r, NULL);
	dbus_message_unref(r);
}

//! org.displayxr.WindowGeometryFeed1.Publish(s), from the KWin script.
static bool
on_feed(struct wgeom_bus *bus, DBusConnection *conn, DBusMessage *msg, void *user)
{
	(void)user;
	if (!dbus_message_is_method_call(msg, KWGB_FEED_IFACE, "Publish")) {
		return false;
	}
	if (!sender_is_kwin(conn, msg)) {
		reply_error(conn, msg, DBUS_ERROR_ACCESS_DENIED, "only KWin may publish window geometry");
		return true;
	}
	const char *json = NULL;
	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) || json == NULL) {
		reply_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "expected one string");
		return true;
	}
	char *clean = kwgb_sanitize(json);
	if (clean == NULL) {
		if (!g_warned_reject) {
			g_warned_reject = true;
			fprintf(stderr, KWGB_LOG ": rejected a snapshot that is not schema version 1 "
			                         "(further rejections are not logged)\n");
		}
		reply_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "not a schema-1 window geometry snapshot");
		return true;
	}
	wgeom_bus_publish(bus, clean);
	kwgb_free(clean);

	DBusMessage *r = dbus_message_new_method_return(msg);
	if (r != NULL) {
		dbus_connection_send(conn, r, NULL);
		dbus_message_unref(r);
	}
	return true;
}

int
main(int argc, char **argv)
{
	if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
		printf(
		    "usage: %s\n"
		    "Serves " WGEOM_BUS_IFACE
		    " on a stock KWin, fed by the DisplayXR KWin script.\n"
		    "Started by D-Bus activation; KDE sessions only (DXR_KWGB_ANY_DESKTOP=1 overrides).\n",
		    argv[0]);
		return 0;
	}

	const char *any = getenv("DXR_KWGB_ANY_DESKTOP");
	if (!(any != NULL && strcmp(any, "1") == 0) && !kwgb_desktop_is_kde(getenv("XDG_CURRENT_DESKTOP"))) {
		fprintf(stderr,
		        KWGB_LOG ": not a KDE session (XDG_CURRENT_DESKTOP=%s) — not taking " WGEOM_BUS_NAME
		                 ", which this desktop's own publisher may need\n",
		        getenv("XDG_CURRENT_DESKTOP") != NULL ? getenv("XDG_CURRENT_DESKTOP") : "unset");
		return 0;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	struct wgeom_bus *bus = wgeom_bus_open(KWGB_LOG, FEED_INTROSPECT_XML, on_feed, NULL);
	if (bus == NULL) {
		return 0;
	}
	fprintf(stderr, KWGB_LOG ": serving " WGEOM_BUS_NAME " (geometry only)\n");

	while (!g_quit && wgeom_bus_dispatch(bus, 1000)) {
	}

	wgeom_bus_close(&bus);
	return 0;
}
