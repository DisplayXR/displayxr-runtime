// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Linux workspace-controller launch hotkey (see service_hotkey_linux.h).
 * @ingroup ipc
 */

#include "service_hotkey_linux.h"
#include "service_hotkey.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SERVICE_HOTKEY_HAVE_DBUS
#include <dbus/dbus.h>
#endif
#ifdef SERVICE_HOTKEY_HAVE_XCB
#include <xcb/xcb.h>
#endif


/*
 *
 * Constants.
 *
 */

#define EXT_BUS_NAME "org.displayxr.WindowGeometry"
#define EXT_OBJECT_PATH "/org/displayxr/WorkspaceHotkey"
#define EXT_INTERFACE "org.displayxr.WorkspaceHotkey1"

//! The dashboard's suspend safety timeout (same as Windows).
#define SUSPEND_TIMEOUT_MS 60000
//! D-Bus method call timeout: the extension answers from the shell's main
//! loop, which is never blocked for long.
#define DBUS_CALL_TIMEOUT_MS 2000
//! Worker wake-up period when idle (only to notice requests; no logging).
#define WORKER_POLL_MS 250
//! X11 auto-repeat delivers KeyPress at the repeat rate while the chord is
//! held; one press is one activation.
#define X11_REPEAT_DEBOUNCE_NS (400ull * 1000ull * 1000ull)


/*
 *
 * Shared state (requests from any thread → worker).
 *
 */

enum backend
{
	BACKEND_NONE = 0,
	BACKEND_EXTENSION,
	BACKEND_X11,
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_thread;
static bool s_started = false;
static bool s_stop = false;
static bool s_dirty = false;             //!< combo or suspend changed
static char s_combo[SERVICE_HOTKEY_MAX]; //!< "" = disarmed
static bool s_suspend = false;
static service_hotkey_linux_activate_fn s_on_activate = NULL;


/*
 *
 * Worker-only state.
 *
 */

struct worker
{
	enum backend backend;
	char applied_combo[SERVICE_HOTKEY_MAX]; //!< what the backend currently holds
	bool applied_suspend;
	uint64_t suspend_deadline_ns; //!< X11 suspend timeout (the extension keeps its own)
	bool warned_none;
	bool warned_ext_old;
	char unit[256];

#ifdef SERVICE_HOTKEY_HAVE_DBUS
	DBusConnection *bus;
	bool ext_changed; //!< the extension's bus name changed owner
#endif

#ifdef SERVICE_HOTKEY_HAVE_XCB
	xcb_connection_t *xcb;
	xcb_window_t root;
	uint32_t x11_mods;
	xcb_keycode_t x11_keycodes[8];
	int x11_keycode_count;
	uint64_t last_press_ns;
	bool warned_x11_busy;
#endif
};


/*
 *
 * cgroup → systemd unit.
 *
 */

bool
service_hotkey_linux_unit_from_cgroup(const char *cgroup_text, char *out, unsigned out_size)
{
	if (out == NULL || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	if (cgroup_text == NULL) {
		return false;
	}
	// cgroup v2: one "0::<path>" line. v1 has several; the "name=systemd" or
	// "0::" line carries the systemd path. Take the first line with a path
	// under a user manager.
	const char *line = cgroup_text;
	while (*line != '\0') {
		const char *eol = strchr(line, '\n');
		size_t len = eol ? (size_t)(eol - line) : strlen(line);
		char buf[1024];
		if (len < sizeof(buf)) {
			memcpy(buf, line, len);
			buf[len] = '\0';
			const char *path = strstr(buf, "::");
			if (path == NULL) {
				path = strstr(buf, ":name=systemd:");
				path = path ? path + strlen(":name=systemd") : NULL;
			}
			if (path != NULL && strstr(path, "/user@") != NULL) {
				const char *last = strrchr(path, '/');
				last = last ? last + 1 : path;
				size_t n = strlen(last);
				const char *suffix = ".service";
				size_t sn = strlen(suffix);
				// The unit itself, never the user manager (user@1000.service:
				// a process directly in the manager's cgroup is not a unit
				// the extension could start).
				if (n > sn && strcmp(last + n - sn, suffix) == 0 && strncmp(last, "user@", 5) != 0 &&
				    n < out_size) {
					memcpy(out, last, n + 1);
					return true;
				}
			}
		}
		if (eol == NULL) {
			break;
		}
		line = eol + 1;
	}
	return false;
}

static void
read_own_unit(char *out, unsigned out_size)
{
	out[0] = '\0';
	FILE *f = fopen("/proc/self/cgroup", "r");
	if (f == NULL) {
		return;
	}
	char text[4096];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = '\0';
	(void)service_hotkey_linux_unit_from_cgroup(text, out, out_size);
}


/*
 *
 * Activation.
 *
 */

#if defined(SERVICE_HOTKEY_HAVE_DBUS) || defined(SERVICE_HOTKEY_HAVE_XCB)
static void
activate(const char *why)
{
	U_LOG_W("Workspace launch hotkey: %s.", why);
	service_hotkey_linux_activate_fn fn = s_on_activate;
	if (fn != NULL) {
		fn();
	}
}
#endif


/*
 *
 * GNOME Shell extension backend (D-Bus).
 *
 */

#ifdef SERVICE_HOTKEY_HAVE_DBUS

static void
ext_connect(struct worker *w)
{
	DBusError err;
	dbus_error_init(&err);
	// Private connection: libdbus' shared session connection is also used by
	// the compositor's window-geometry provider on other threads.
	w->bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (w->bus == NULL) {
		U_LOG_I("Workspace hotkey: no session bus (%s).", err.message ? err.message : "?");
		dbus_error_free(&err);
		return;
	}
	dbus_connection_set_exit_on_disconnect(w->bus, FALSE);

	// The extension's presses, and its comings and goings (lock screen,
	// shell restart, disable/enable), which re-push the accelerator.
	dbus_bus_add_match(w->bus,
	                   "type='signal',sender='" EXT_BUS_NAME "',path='" EXT_OBJECT_PATH
	                   "',interface='" EXT_INTERFACE "',member='Activated'",
	                   &err);
	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
	}
	dbus_bus_add_match(w->bus,
	                   "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
	                   "member='NameOwnerChanged',arg0='" EXT_BUS_NAME "'",
	                   &err);
	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
	}
	dbus_connection_flush(w->bus);
}

static bool
ext_present(struct worker *w)
{
	if (w->bus == NULL) {
		return false;
	}
	DBusError err;
	dbus_error_init(&err);
	bool has = dbus_bus_name_has_owner(w->bus, EXT_BUS_NAME, &err);
	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
		return false;
	}
	return has;
}

/*!
 * Configure(s accelerator, s unit) -> (b pending). Returns false when the
 * extension does not implement WorkspaceHotkey1 (version < 13) or the call
 * failed. @p out_pending: a press the extension saw while no service was
 * registered (it started our unit for it).
 */
static bool
ext_configure(struct worker *w, const char *accelerator, bool *out_pending)
{
	*out_pending = false;
	DBusMessage *msg = dbus_message_new_method_call(EXT_BUS_NAME, EXT_OBJECT_PATH, EXT_INTERFACE, "Configure");
	if (msg == NULL) {
		return false;
	}
	const char *unit = w->unit;
	dbus_message_append_args(msg, DBUS_TYPE_STRING, &accelerator, DBUS_TYPE_STRING, &unit, DBUS_TYPE_INVALID);
	DBusError err;
	dbus_error_init(&err);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(w->bus, msg, DBUS_CALL_TIMEOUT_MS, &err);
	dbus_message_unref(msg);
	if (reply == NULL) {
		// Disarming against an extension that predates WorkspaceHotkey1 is a
		// no-op, not news.
		if (accelerator[0] != '\0' && !w->warned_ext_old) {
			w->warned_ext_old = true;
			U_LOG_W(
			    "Workspace hotkey: the GNOME Shell extension does not take a launch hotkey (%s) — it needs "
			    "window-geometry@displayxr.org version 13 or later.",
			    err.message ? err.message : "no reply");
		}
		dbus_error_free(&err);
		return false;
	}
	dbus_bool_t pending = FALSE;
	if (!dbus_message_get_args(reply, &err, DBUS_TYPE_BOOLEAN, &pending, DBUS_TYPE_INVALID)) {
		dbus_error_free(&err);
		pending = FALSE;
	}
	dbus_message_unref(reply);
	*out_pending = pending != FALSE;
	return true;
}

static void
ext_suspend(struct worker *w, bool suspend)
{
	DBusMessage *msg = dbus_message_new_method_call(EXT_BUS_NAME, EXT_OBJECT_PATH, EXT_INTERFACE, "Suspend");
	if (msg == NULL) {
		return;
	}
	dbus_bool_t b = suspend ? TRUE : FALSE;
	dbus_uint32_t timeout = SUSPEND_TIMEOUT_MS;
	dbus_message_append_args(msg, DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_UINT32, &timeout, DBUS_TYPE_INVALID);
	DBusError err;
	dbus_error_init(&err);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(w->bus, msg, DBUS_CALL_TIMEOUT_MS, &err);
	dbus_message_unref(msg);
	if (reply != NULL) {
		dbus_message_unref(reply);
	}
	dbus_error_free(&err);
}

//! Drain incoming messages: Activated → activate; NameOwnerChanged → re-push.
static void
ext_dispatch(struct worker *w)
{
	if (w->bus == NULL) {
		return;
	}
	if (!dbus_connection_read_write(w->bus, 0)) {
		// Disconnected (session bus gone): drop the backend for good.
		dbus_connection_close(w->bus);
		dbus_connection_unref(w->bus);
		w->bus = NULL;
		if (w->backend == BACKEND_EXTENSION) {
			w->backend = BACKEND_NONE;
		}
		return;
	}
	DBusMessage *msg;
	while ((msg = dbus_connection_pop_message(w->bus)) != NULL) {
		if (dbus_message_is_signal(msg, EXT_INTERFACE, "Activated")) {
			if (w->backend == BACKEND_EXTENSION) {
				activate("pressed (GNOME Shell extension)");
			}
		} else if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
			w->ext_changed = true;
		}
		dbus_message_unref(msg);
	}
}

static void
ext_close(struct worker *w)
{
	if (w->bus != NULL) {
		dbus_connection_close(w->bus);
		dbus_connection_unref(w->bus);
		w->bus = NULL;
	}
}

#endif // SERVICE_HOTKEY_HAVE_DBUS


/*
 *
 * X11 backend (root-window key grab).
 *
 */

#ifdef SERVICE_HOTKEY_HAVE_XCB

static bool
x11_session(void)
{
	const char *display = getenv("DISPLAY");
	const char *wayland = getenv("WAYLAND_DISPLAY");
	const char *type = getenv("XDG_SESSION_TYPE");
	if (display == NULL || display[0] == '\0') {
		return false;
	}
	// Under Wayland DISPLAY is Xwayland's: a root grab there sees only X11
	// clients' input, never a global chord.
	if ((wayland != NULL && wayland[0] != '\0') || (type != NULL && strcmp(type, "wayland") == 0)) {
		return false;
	}
	return true;
}

//! Every keycode whose level 0 or 1 carries @p keysym.
static int
x11_keycodes_for(struct worker *w, uint32_t keysym, xcb_keycode_t *out, int max)
{
	const xcb_setup_t *setup = xcb_get_setup(w->xcb);
	xcb_keycode_t min = setup->min_keycode;
	xcb_keycode_t maxk = setup->max_keycode;
	xcb_get_keyboard_mapping_cookie_t ck = xcb_get_keyboard_mapping(w->xcb, min, (uint8_t)(maxk - min + 1));
	xcb_get_keyboard_mapping_reply_t *r = xcb_get_keyboard_mapping_reply(w->xcb, ck, NULL);
	if (r == NULL) {
		return 0;
	}
	const xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(r);
	const int per = r->keysyms_per_keycode;
	const int total = xcb_get_keyboard_mapping_keysyms_length(r);
	int n = 0;
	for (int i = 0; per > 0 && i < total / per && n < max; i++) {
		for (int lvl = 0; lvl < per && lvl < 2; lvl++) {
			if (syms[i * per + lvl] == keysym) {
				out[n++] = (xcb_keycode_t)(min + i);
				break;
			}
		}
	}
	free(r);
	return n;
}

static const uint16_t s_lock_variants[] = {
    0,
    SERVICE_HOTKEY_X11_LOCK,
    SERVICE_HOTKEY_X11_MOD2,
    SERVICE_HOTKEY_X11_LOCK | SERVICE_HOTKEY_X11_MOD2,
};

static void
x11_ungrab(struct worker *w)
{
	if (w->xcb == NULL) {
		return;
	}
	for (int k = 0; k < w->x11_keycode_count; k++) {
		for (size_t v = 0; v < sizeof(s_lock_variants) / sizeof(s_lock_variants[0]); v++) {
			xcb_ungrab_key(w->xcb, w->x11_keycodes[k], w->root,
			               (uint16_t)(w->x11_mods | s_lock_variants[v]));
		}
	}
	w->x11_keycode_count = 0;
	xcb_flush(w->xcb);
}

static bool
x11_grab(struct worker *w, const char *combo)
{
	if (w->xcb == NULL) {
		if (!x11_session()) {
			return false;
		}
		int screen_num = 0;
		w->xcb = xcb_connect(NULL, &screen_num);
		if (w->xcb == NULL || xcb_connection_has_error(w->xcb)) {
			if (w->xcb != NULL) {
				xcb_disconnect(w->xcb);
			}
			w->xcb = NULL;
			return false;
		}
		xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(w->xcb));
		for (int i = 0; i < screen_num && it.rem > 0; i++) {
			xcb_screen_next(&it);
		}
		if (it.rem <= 0) {
			xcb_disconnect(w->xcb);
			w->xcb = NULL;
			return false;
		}
		w->root = it.data->root;
	}

	x11_ungrab(w);

	struct service_hotkey hk;
	uint32_t keysym = 0;
	uint32_t mods = 0;
	if (!service_hotkey_parse(combo, &hk) || !service_hotkey_to_x11(&hk, &keysym, &mods)) {
		return false;
	}
	w->x11_mods = mods;
	w->x11_keycode_count = x11_keycodes_for(w, keysym, w->x11_keycodes, 8);
	if (w->x11_keycode_count == 0) {
		U_LOG_W("Workspace hotkey: no key on this X11 keyboard map produces '%s'.", combo);
		return false;
	}

	bool busy = false;
	for (int k = 0; k < w->x11_keycode_count; k++) {
		for (size_t v = 0; v < sizeof(s_lock_variants) / sizeof(s_lock_variants[0]); v++) {
			xcb_void_cookie_t c =
			    xcb_grab_key_checked(w->xcb, 1, w->root, (uint16_t)(mods | s_lock_variants[v]),
			                         w->x11_keycodes[k], XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
			xcb_generic_error_t *e = xcb_request_check(w->xcb, c);
			if (e != NULL) {
				busy = true; // BadAccess: another client holds this chord
				free(e);
			}
		}
	}
	xcb_flush(w->xcb);
	if (busy && !w->warned_x11_busy) {
		w->warned_x11_busy = true;
		U_LOG_W("Workspace hotkey: another X11 client already grabs '%s'; the launch hotkey may not fire.",
		        combo);
	}
	return true;
}

static void
x11_dispatch(struct worker *w)
{
	if (w->xcb == NULL) {
		return;
	}
	xcb_generic_event_t *ev;
	bool remap = false;
	while ((ev = xcb_poll_for_event(w->xcb)) != NULL) {
		const uint8_t type = ev->response_type & 0x7f;
		if (type == XCB_KEY_PRESS && w->backend == BACKEND_X11 && !w->applied_suspend) {
			const xcb_key_press_event_t *kp = (const xcb_key_press_event_t *)ev;
			const uint16_t state =
			    kp->state & (uint16_t)~(SERVICE_HOTKEY_X11_LOCK | SERVICE_HOTKEY_X11_MOD2);
			bool ours = false;
			for (int k = 0; k < w->x11_keycode_count; k++) {
				ours = ours || kp->detail == w->x11_keycodes[k];
			}
			const uint64_t now = os_monotonic_get_ns();
			if (ours && (state & 0xffu) == w->x11_mods && now - w->last_press_ns > X11_REPEAT_DEBOUNCE_NS) {
				w->last_press_ns = now;
				activate("pressed (X11 grab)");
			} else if (ours) {
				w->last_press_ns = now; // held: auto-repeat
			}
		} else if (type == XCB_MAPPING_NOTIFY) {
			remap = true;
		}
		free(ev);
	}
	if (xcb_connection_has_error(w->xcb)) {
		xcb_disconnect(w->xcb);
		w->xcb = NULL;
		w->x11_keycode_count = 0;
		if (w->backend == BACKEND_X11) {
			w->backend = BACKEND_NONE;
		}
		return;
	}
	if (remap && w->backend == BACKEND_X11 && !w->applied_suspend && w->applied_combo[0] != '\0') {
		(void)x11_grab(w, w->applied_combo);
	}
}

static void
x11_close(struct worker *w)
{
	if (w->xcb != NULL) {
		x11_ungrab(w);
		xcb_disconnect(w->xcb);
		w->xcb = NULL;
	}
}

#endif // SERVICE_HOTKEY_HAVE_XCB


/*
 *
 * Backend selection.
 *
 */

static void
release_backend(struct worker *w)
{
#ifdef SERVICE_HOTKEY_HAVE_XCB
	if (w->backend == BACKEND_X11) {
		x11_ungrab(w);
	}
#endif
	w->backend = BACKEND_NONE;
}

//! (Re)apply @p combo / @p suspend. Picks the backend afresh each time.
static void
apply(struct worker *w, const char *combo, bool suspend, bool ext_just_left)
{
	const bool armed = combo[0] != '\0';

#ifdef SERVICE_HOTKEY_HAVE_DBUS
	// The extension first: it is the only backend that works under Wayland,
	// and under X11 GNOME it is mutter's own keybinding path (no fight with
	// mutter over a root grab), and it survives the service's idle exit.
	if (ext_present(w)) {
		char accel[64] = "";
		struct service_hotkey hk;
		if (armed &&
		    (!service_hotkey_parse(combo, &hk) || !service_hotkey_to_accelerator(&hk, accel, sizeof(accel)))) {
			accel[0] = '\0';
		}
		bool pending = false;
		if (ext_configure(w, accel, &pending)) {
			if (w->backend != BACKEND_EXTENSION) {
				release_backend(w);
				U_LOG_W("Workspace hotkey: %s%s via the GNOME Shell extension (unit '%s').",
				        armed ? "armed " : "disarmed", armed ? combo : "", w->unit);
			}
			w->backend = BACKEND_EXTENSION;
			ext_suspend(w, suspend && armed);
			snprintf(w->applied_combo, sizeof(w->applied_combo), "%s", combo);
			w->applied_suspend = suspend && armed;
			if (pending && armed) {
				activate("pressed before this service started (carried by the GNOME Shell extension)");
			}
			return;
		}
	}
#endif

	if (w->backend == BACKEND_EXTENSION) {
		w->backend = BACKEND_NONE; // the extension went away
	}
	snprintf(w->applied_combo, sizeof(w->applied_combo), "%s", combo);
	w->applied_suspend = suspend && armed;

	if (!armed) {
		release_backend(w);
		return;
	}

#ifdef SERVICE_HOTKEY_HAVE_XCB
	if (suspend) {
		if (w->backend == BACKEND_X11) {
			x11_ungrab(w); // stays BACKEND_X11: resumes on suspend=false / timeout
		}
		return;
	}
	if (x11_grab(w, combo)) {
		if (w->backend != BACKEND_X11) {
			U_LOG_W(
			    "Workspace hotkey: armed %s via an X11 root-window grab (only while this service runs).",
			    combo);
		}
		w->backend = BACKEND_X11;
		return;
	}
#endif

	w->backend = BACKEND_NONE;
	// The extension leaving (lock screen, shell restart) is transient: it
	// comes back and is re-configured; that is not the "nothing works" case.
	if (!w->warned_none && !ext_just_left) {
		w->warned_none = true;
		U_LOG_W(
		    "Workspace hotkey: no way to grab '%s' in this session (no GNOME Shell extension "
		    "window-geometry@displayxr.org v13+, not an X11 session). Launch the workspace controller from the "
		    "dashboard or with `displayxr-cli workspace launch <id>`.",
		    combo);
	}
}


/*
 *
 * Worker.
 *
 */

static void *
worker_main(void *arg)
{
	(void)arg;
	struct worker w;
	memset(&w, 0, sizeof(w));
	read_own_unit(w.unit, sizeof(w.unit));

#ifdef SERVICE_HOTKEY_HAVE_DBUS
	ext_connect(&w);
#endif

	pthread_mutex_lock(&s_lock);
	while (!s_stop) {
		bool dirty = s_dirty;
		s_dirty = false;
		char combo[SERVICE_HOTKEY_MAX];
		snprintf(combo, sizeof(combo), "%s", s_combo);
		bool suspend = s_suspend;
		pthread_mutex_unlock(&s_lock);

		const uint64_t now = os_monotonic_get_ns();

		// A new suspend starts (or re-arms) the X11 safety timeout; the
		// extension runs its own from the timeout we pass it.
		if (dirty && suspend) {
			w.suspend_deadline_ns = now + (uint64_t)SUSPEND_TIMEOUT_MS * 1000ull * 1000ull;
		}
		if (suspend && w.suspend_deadline_ns != 0 && now >= w.suspend_deadline_ns) {
			U_LOG_W("Workspace hotkey resumed (timeout).");
			pthread_mutex_lock(&s_lock);
			s_suspend = false;
			pthread_mutex_unlock(&s_lock);
			suspend = false;
			w.suspend_deadline_ns = 0;
			dirty = true;
		}

		bool ext_changed = false;
#ifdef SERVICE_HOTKEY_HAVE_DBUS
		if (w.ext_changed) {
			w.ext_changed = false;
			ext_changed = true;
			dirty = true; // the extension appeared / left: re-pick the backend
		}
#endif
		if (dirty) {
			apply(&w, combo, suspend, ext_changed && w.backend == BACKEND_EXTENSION);
		}

		// Wait for input on either connection, or the next request tick.
		struct pollfd fds[2];
		nfds_t nfds = 0;
#ifdef SERVICE_HOTKEY_HAVE_DBUS
		int dbus_fd = -1;
		if (w.bus != NULL && dbus_connection_get_unix_fd(w.bus, &dbus_fd) && dbus_fd >= 0) {
			fds[nfds].fd = dbus_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}
#endif
#ifdef SERVICE_HOTKEY_HAVE_XCB
		if (w.xcb != NULL) {
			fds[nfds].fd = xcb_get_file_descriptor(w.xcb);
			fds[nfds].events = POLLIN;
			nfds++;
		}
#endif
		(void)poll(fds, nfds, WORKER_POLL_MS);

#ifdef SERVICE_HOTKEY_HAVE_DBUS
		ext_dispatch(&w);
#endif
#ifdef SERVICE_HOTKEY_HAVE_XCB
		x11_dispatch(&w);
#endif
		pthread_mutex_lock(&s_lock);
	}
	pthread_mutex_unlock(&s_lock);

	// The extension keeps its cached accelerator on purpose: the next press
	// after this (idle-)exit starts the service again. Only the X11 grab, which
	// dies with this process anyway, is released explicitly.
#ifdef SERVICE_HOTKEY_HAVE_XCB
	x11_close(&w);
#endif
#ifdef SERVICE_HOTKEY_HAVE_DBUS
	ext_close(&w);
#endif
	return NULL;
}


/*
 *
 * Public API.
 *
 */

void
service_hotkey_linux_start(service_hotkey_linux_activate_fn on_activate)
{
	pthread_mutex_lock(&s_lock);
	if (s_started) {
		pthread_mutex_unlock(&s_lock);
		return;
	}
#ifdef SERVICE_HOTKEY_HAVE_DBUS
	dbus_threads_init_default();
#endif
	s_on_activate = on_activate;
	s_stop = false;
	s_started = pthread_create(&s_thread, NULL, worker_main, NULL) == 0;
	if (!s_started) {
		U_LOG_W("Workspace hotkey: could not start the worker thread.");
	}
	pthread_mutex_unlock(&s_lock);
}

void
service_hotkey_linux_arm(const char *combo)
{
	pthread_mutex_lock(&s_lock);
	const char *c = combo != NULL ? combo : "";
	if (strcmp(s_combo, c) != 0) {
		snprintf(s_combo, sizeof(s_combo), "%s", c);
		s_dirty = true;
	} else if (!s_dirty) {
		// Same combo: still re-apply once (a reload re-pushes it, which also
		// recovers an extension that lost it).
		s_dirty = true;
	}
	pthread_mutex_unlock(&s_lock);
}

void
service_hotkey_linux_suspend(bool suspend)
{
	pthread_mutex_lock(&s_lock);
	if (suspend || s_suspend) {
		U_LOG_W("Workspace hotkey %s (explicit).", suspend ? "suspended" : "resumed");
	}
	s_suspend = suspend;
	s_dirty = true; // a repeated true re-arms the timeout
	pthread_mutex_unlock(&s_lock);
}

void
service_hotkey_linux_stop(void)
{
	pthread_mutex_lock(&s_lock);
	if (!s_started) {
		pthread_mutex_unlock(&s_lock);
		return;
	}
	s_stop = true;
	pthread_mutex_unlock(&s_lock);
	pthread_join(s_thread, NULL);
	pthread_mutex_lock(&s_lock);
	s_started = false;
	s_on_activate = NULL;
	s_combo[0] = '\0';
	s_suspend = false;
	s_dirty = false;
	pthread_mutex_unlock(&s_lock);
}
