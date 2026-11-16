// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671, #2689): the native-Wayland
 *         backend, read from GNOME Shell's window snapshot.
 * @ingroup aux_util
 *
 * ## Why a compositor-side source
 *
 * A Wayland client never learns the global stacking order, the active
 * workspace or whether its window is minimized by someone else, so it cannot
 * tell whether its 3D window can be seen. GNOME Shell can. DisplayXR's own
 * extension, window-geometry@displayxr.org (contrib/gnome-shell/), already
 * publishes a per-window snapshot over the session bus for the vk_native
 * geometry consumer (comp_vk_native_wl_geom.c); from extension version 20
 * every entry also carries `hidden` ("" or `minimized` | `workspace` |
 * `locked` | `covered`), computed in gnome-shell with the X11 tracker's
 * occluder rules (docs/specs/runtime/wayland-window-geometry.md §4.3). This
 * backend reads that field for this process's window, picked exactly as the
 * geometry consumer picks it (u_wv_wl_parse_snapshot).
 *
 * ## Its own connection, on the tracker thread
 *
 * The geometry consumer is pumped by the frame loop, and a hidden Wayland
 * surface may stop getting frame callbacks, so its loop can stall exactly
 * while the window is hidden. This backend therefore opens its own private
 * session-bus connection and uses it from the tracker thread only: the
 * connection's fd wakes the tracker on every WindowsChanged, and only the
 * newest payload of a burst is parsed. The first GetWindows, and a lazy retry
 * every @ref WVL_RETRY_NS while there is no snapshot, are bounded calls on
 * that thread, never on a render thread. NameOwnerChanged drops the state when
 * the publisher goes away and re-takes it when it comes back.
 *
 * ## Limits (all on the "visible" side, which keeps the lens in 3D)
 *
 * - Not GNOME, no session bus: no backend (the tracker is not created).
 * - No extension, or one older than version 20 (no `hidden`): visible, with
 *   one INFO line saying which.
 * - The extension judges the window on its own monitor, not on the 3D screen
 *   (the snapshot does not name monitors): `panel` is not used.
 */

#include "util/u_window_visibility.h"
#include "util/u_window_visibility_posix.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <dbus/dbus.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WVL_BUS_NAME "org.displayxr.WindowGeometry"
#define WVL_OBJ_PATH "/org/displayxr/WindowGeometry"
#define WVL_IFACE "org.displayxr.WindowGeometry1"
#define WVL_MATCH_RULE "type='signal',interface='" WVL_IFACE "',member='WindowsChanged'"
#define WVL_OWNER_MATCH_RULE                                                                                           \
	"type='signal',sender='" DBUS_SERVICE_DBUS "',interface='" DBUS_INTERFACE_DBUS                                 \
	"',member='NameOwnerChanged',arg0='" WVL_BUS_NAME "'"

//! Bound of a GetWindows call (tracker thread only).
#define WVL_CALL_TIMEOUT_MS 100
//! Earliest re-ask while there is no snapshot.
#define WVL_RETRY_NS (5ull * 1000ull * 1000ull * 1000ull)

//! What has been said in the one INFO line about the source (logged on change).
enum wvl_said
{
	WVL_SAID_NOTHING = 0,
	WVL_SAID_NO_PUBLISHER,
	WVL_SAID_OLD_PUBLISHER,
	WVL_SAID_TRACKING,
};

struct u_wv_wl
{
	DBusConnection *conn;
	int fd;
	int32_t pid;
	bool have_target;

	//! The newest snapshot's answer for this process's window.
	enum u_wv_wl_state state;
	const char *reason;
	bool have_snapshot;
	uint64_t next_retry_ns;

	enum wvl_said said;
};

static void
wvl_apply(struct u_wv_wl *b, const char *json)
{
	const char *reason = NULL;
	const enum u_wv_wl_state st = u_wv_wl_parse_snapshot(json, b->pid, &reason);
	if (st == U_WV_WL_UNKNOWN) {
		return; // not a payload this reader understands: keep the last one
	}
	b->state = st;
	b->reason = reason;
	b->have_snapshot = true;
}

static void
wvl_forget(struct u_wv_wl *b)
{
	b->state = U_WV_WL_UNKNOWN;
	b->reason = "uncovered";
	b->have_snapshot = false;
	b->next_retry_ns = 0;
}

//! Bounded GetWindows. Signals read meanwhile stay queued for the next pump.
static bool
wvl_request_snapshot(struct u_wv_wl *b)
{
	DBusMessage *call = dbus_message_new_method_call(WVL_BUS_NAME, WVL_OBJ_PATH, WVL_IFACE, "GetWindows");
	if (call == NULL) {
		return false;
	}
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(b->conn, call, WVL_CALL_TIMEOUT_MS, NULL);
	dbus_message_unref(call);
	if (reply == NULL) {
		return false;
	}
	const char *json = NULL;
	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) && json != NULL) {
		wvl_apply(b, json);
	}
	dbus_message_unref(reply);
	return b->have_snapshot;
}

/*!
 * Read what the socket has, then handle everything queued: only the newest
 * WindowsChanged of the batch is parsed (a drag sends one per redraw).
 */
static void
wvl_pump(struct u_wv_wl *b)
{
	(void)dbus_connection_read_write(b->conn, 0);

	DBusMessage *newest = NULL;
	bool owner_appeared = false;
	DBusMessage *msg = NULL;
	while ((msg = dbus_connection_pop_message(b->conn)) != NULL) {
		if (dbus_message_is_signal(msg, WVL_IFACE, "WindowsChanged")) {
			if (newest != NULL) {
				dbus_message_unref(newest);
			}
			newest = msg;
			continue;
		}
		if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
			const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
			                          DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
			    name != NULL && strcmp(name, WVL_BUS_NAME) == 0) {
				// Gone or replaced: what we had is no longer vouched for.
				if (newest != NULL) {
					dbus_message_unref(newest);
					newest = NULL;
				}
				wvl_forget(b);
				owner_appeared = new_owner != NULL && new_owner[0] != '\0';
			}
		}
		dbus_message_unref(msg);
	}

	if (newest != NULL) {
		const char *json = NULL;
		if (dbus_message_get_args(newest, NULL, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) && json != NULL) {
			wvl_apply(b, json);
		}
		dbus_message_unref(newest);
	}
	if (owner_appeared && !b->have_snapshot) {
		// It only emits on the next change, so ask now.
		(void)wvl_request_snapshot(b);
		b->next_retry_ns = os_monotonic_get_ns() + WVL_RETRY_NS;
	}
}

//! The one INFO line about where visibility comes from, on change only.
static void
wvl_say(struct u_wv_wl *b)
{
	enum wvl_said now = WVL_SAID_NOTHING;
	switch (b->state) {
	case U_WV_WL_UNKNOWN: now = WVL_SAID_NO_PUBLISHER; break;
	case U_WV_WL_NO_FIELD: now = WVL_SAID_OLD_PUBLISHER; break;
	case U_WV_WL_VISIBLE:
	case U_WV_WL_HIDDEN: now = WVL_SAID_TRACKING; break;
	case U_WV_WL_NO_WINDOW: return; // not mapped yet, or gone: nothing to say
	}
	if (now == b->said) {
		return;
	}
	b->said = now;
	switch (now) {
	case WVL_SAID_NO_PUBLISHER:
		U_LOG_I("#2689 wl visibility: no answer from " WVL_BUS_NAME
		        " (the window-geometry@displayxr.org GNOME Shell extension) — the native Wayland 3D window "
		        "counts as always visible until it appears");
		break;
	case WVL_SAID_OLD_PUBLISHER:
		U_LOG_I(
		    "#2689 wl visibility: the window-geometry@displayxr.org extension publishes no \"hidden\" "
		    "(older than version 20; log out and back in after an update) — the native Wayland 3D "
		    "window counts as always visible");
		break;
	case WVL_SAID_TRACKING:
		U_LOG_I("#2689 wl visibility: reading the native Wayland 3D window's visibility from GNOME Shell");
		break;
	default: break;
	}
}

/*
 *
 * Backend interface (through u_window_visibility_linux.c).
 *
 */

struct u_wv_wl *
u_wv_wl_create(void)
{
	// Only GNOME Shell has the publisher. Ubuntu says "ubuntu:GNOME".
	const char *desktop = getenv("XDG_CURRENT_DESKTOP");
	if (desktop == NULL || strstr(desktop, "GNOME") == NULL) {
		return NULL;
	}

	struct u_wv_wl *b = (struct u_wv_wl *)calloc(1, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	DBusError err;
	dbus_error_init(&err);
	// Private: a shared connection must never be closed, and this one lives
	// and dies with the tracker (and is used from its thread only).
	b->conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (b->conn == NULL) {
		dbus_error_free(&err);
		free(b);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(b->conn, FALSE);
	dbus_bus_add_match(b->conn, WVL_MATCH_RULE, &err);
	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
	}
	dbus_bus_add_match(b->conn, WVL_OWNER_MATCH_RULE, &err);
	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
	}
	dbus_connection_flush(b->conn);

	b->fd = -1;
	if (!dbus_connection_get_unix_fd(b->conn, &b->fd)) {
		b->fd = -1;
	}
	b->pid = (int32_t)getpid();
	wvl_forget(b);
	return b;
}

void
u_wv_wl_set_target(struct u_wv_wl *b, void *native_window)
{
	// The surface only names the target; the window is this process's.
	b->have_target = native_window != NULL;
}

bool
u_wv_wl_check(struct u_wv_wl *b, const char *panel, const char **out_reason)
{
	(void)panel; // judged on the window's own monitor by the extension
	*out_reason = "uncovered";
	if (!b->have_target) {
		return false;
	}

	wvl_pump(b);
	if (!b->have_snapshot) {
		const uint64_t now = os_monotonic_get_ns();
		if (now >= b->next_retry_ns) {
			b->next_retry_ns = now + WVL_RETRY_NS;
			(void)wvl_request_snapshot(b);
			// Signals the call read meanwhile.
			wvl_pump(b);
		}
	}
	wvl_say(b);

	if (b->state != U_WV_WL_HIDDEN) {
		return false;
	}
	*out_reason = b->reason != NULL ? b->reason : "hidden by the shell";
	return true;
}

int
u_wv_wl_fd(struct u_wv_wl *b)
{
	return b->fd;
}

void
u_wv_wl_drain(struct u_wv_wl *b)
{
	wvl_pump(b);
}

void
u_wv_wl_destroy(struct u_wv_wl *b)
{
	if (b == NULL) {
		return;
	}
	if (b->conn != NULL) {
		dbus_connection_close(b->conn);
		dbus_connection_unref(b->conn);
	}
	free(b);
}
