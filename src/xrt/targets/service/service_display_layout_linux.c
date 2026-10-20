// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux display layout feeder — see service_display_layout_linux.h.
 * @ingroup ipc
 */

#include "service_display_layout_linux.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#ifdef SERVICE_HAVE_DBUS
#include <dbus/dbus.h>
#endif

#define MUTTER_DC_IFACE "org.gnome.Mutter.DisplayConfig"
#define MUTTER_DC_PATH "/org/gnome/Mutter/DisplayConfig"

bool
service_display_layout_is_change_signal(const char *interface, const char *member)
{
	return interface != NULL && member != NULL && strcmp(interface, MUTTER_DC_IFACE) == 0 &&
	       strcmp(member, "MonitorsChanged") == 0;
}

#ifdef SERVICE_HAVE_DBUS

//! How long one bus wait blocks; bounds how long stop() waits for the join.
#define LAYOUT_WAIT_MS 250
//! The follow-up re-probe after the last MonitorsChanged.
#define LAYOUT_FOLLOWUP_NS ((uint64_t)3 * 1000 * 1000 * 1000)

static pthread_t s_thread;
static bool s_running = false;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_stop = false; //!< guarded by s_lock
static service_display_layout_fn s_fn = NULL;
static DBusConnection *s_bus = NULL;

static bool
stop_requested(void)
{
	pthread_mutex_lock(&s_lock);
	const bool stop = s_stop;
	pthread_mutex_unlock(&s_lock);
	return stop;
}

static void *
layout_thread(void *ptr)
{
	(void)ptr;
	uint64_t count = 0;
	uint64_t followup_ns = 0; // 0 = none pending
	while (!stop_requested()) {
		// One more re-probe a few seconds after the last signal: XWayland
		// resizes its root (the X11 rects the resolver reads) asynchronously
		// to Mutter's signal, and the worker's 1 s debounce could otherwise
		// read the old rects. A re-probe that finds nothing new publishes
		// nothing (the generation moves only on a real change).
		if (followup_ns != 0 && os_monotonic_get_ns() >= followup_ns) {
			followup_ns = 0;
			if (s_fn != NULL) {
				s_fn("display layout changed (MonitorsChanged follow-up)");
			}
		}
		if (!dbus_connection_read_write(s_bus, LAYOUT_WAIT_MS)) {
			U_LOG_W(
			    "display layout feeder: session bus disconnected — live display-scale changes are no "
			    "longer followed (a service restart re-reads the layout)");
			break;
		}
		DBusMessage *msg = NULL;
		while ((msg = dbus_connection_pop_message(s_bus)) != NULL) {
			if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL &&
			    service_display_layout_is_change_signal(dbus_message_get_interface(msg),
			                                            dbus_message_get_member(msg))) {
				// One line per Mutter configuration change (a human action, a
				// few per session) — the re-probe it posts is debounced and
				// logs its own outcome.
				count++;
				U_LOG_W(
				    "display layout feeder: MonitorsChanged #%llu from %s — re-probing the displays "
				    "(#2251)",
				    (unsigned long long)count,
				    dbus_message_get_sender(msg) != NULL ? dbus_message_get_sender(msg) : "?");
				if (s_fn != NULL) {
					s_fn("display layout changed (MonitorsChanged)");
				}
				followup_ns = os_monotonic_get_ns() + LAYOUT_FOLLOWUP_NS;
			}
			dbus_message_unref(msg);
		}
	}
	return NULL;
}

bool
service_display_layout_start(service_display_layout_fn fn)
{
	if (fn == NULL || s_running) {
		return false;
	}
	DBusError err;
	dbus_error_init(&err);
	// Private connection: the shared one belongs to whoever else in the
	// process uses libdbus (the hotkey backend, the compositor's providers).
	DBusConnection *bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (bus == NULL) {
		U_LOG_W(
		    "display layout feeder: no session bus (%s) — a live display-scale change needs a service "
		    "restart to be seen",
		    dbus_error_is_set(&err) ? err.message : "?");
		dbus_error_free(&err);
		return false;
	}
	dbus_connection_set_exit_on_disconnect(bus, FALSE);
	// No sender filter: Mutter's well-known name resolves to its unique name
	// at match time, and a re-probe on a stray signal is harmless (the worker
	// publishes only a real change).
	dbus_bus_add_match(
	    bus, "type='signal',path='" MUTTER_DC_PATH "',interface='" MUTTER_DC_IFACE "',member='MonitorsChanged'",
	    &err);
	if (dbus_error_is_set(&err)) {
		U_LOG_W("display layout feeder: MonitorsChanged add_match failed (%s)", err.message);
		dbus_error_free(&err);
		dbus_connection_close(bus);
		dbus_connection_unref(bus);
		return false;
	}
	dbus_connection_flush(bus);

	s_bus = bus;
	s_fn = fn;
	pthread_mutex_lock(&s_lock);
	s_stop = false;
	pthread_mutex_unlock(&s_lock);
	if (pthread_create(&s_thread, NULL, layout_thread, NULL) != 0) {
		U_LOG_W("display layout feeder: could not start its thread");
		s_bus = NULL;
		s_fn = NULL;
		dbus_connection_close(bus);
		dbus_connection_unref(bus);
		return false;
	}
	s_running = true;
	U_LOG_W("display layout feeder: watching " MUTTER_DC_IFACE
	        ".MonitorsChanged — a display-scale or "
	        "arrangement change re-resolves the panel geometry live (#2251)");
	return true;
}

void
service_display_layout_stop(void)
{
	if (!s_running) {
		return;
	}
	pthread_mutex_lock(&s_lock);
	s_stop = true;
	pthread_mutex_unlock(&s_lock);
	pthread_join(s_thread, NULL);
	s_running = false;
	dbus_connection_close(s_bus);
	dbus_connection_unref(s_bus);
	s_bus = NULL;
	s_fn = NULL;
}

#else // !SERVICE_HAVE_DBUS

bool
service_display_layout_start(service_display_layout_fn fn)
{
	(void)fn;
	U_LOG_W(
	    "display layout feeder: built without D-Bus — a live display-scale change needs a service restart "
	    "to be seen");
	return false;
}

void
service_display_layout_stop(void)
{}

#endif // SERVICE_HAVE_DBUS
