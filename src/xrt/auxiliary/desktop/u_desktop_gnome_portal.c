// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: the RemoteDesktop portal session, the portal
 *         clipboard and libei — the app's consent and its input path (ADR-056).
 *
 * ## Consent and the session lifetime
 *
 * org.freedesktop.portal.RemoteDesktop, in the APP's name: CreateSession →
 * SelectDevices (KEYBOARD | POINTER, persist_mode 2, the restore token if any) →
 * Clipboard.RequestClipboard → Start (the dialog, the first time) → ConnectToEIS
 * → a libei sender context.
 *
 * Before the first portal call the connection registers the host app id with
 * org.freedesktop.host.portal.Registry (xdg-desktop-portal >= 1.19) — from
 * `DXR_DESKTOP_APP_ID`, else from the basename of `GIO_LAUNCHED_DESKTOP_FILE`.
 * The portal accepts it only when <app id>.desktop is installed; then it names the
 * app in its dialog and in Settings and binds the approval and its restore token
 * to that id. Otherwise (no id, no .desktop file, a portal < 1.19) the portal
 * attributes the session by the process's systemd scope (the launcher, terminal
 * or IDE that started it), or to no app.
 *
 * The grant survives a restart of the app (spec §6.4, ADR-056 Amendment 2): the
 * restore token and what it covers (input, the screens, which monitors) are saved
 * in a private file (u_desktop_gnome_grant.h, 0600 in a 0700 directory, written
 * atomically under $XDG_STATE_HOME/displayxr/desktop-control/) keyed by the app's
 * own identity — its registered app id, else its own app id, else its executable
 * path — read at the first request or restore and rewritten after every Start. So
 * the user sees the dialog once per app, not once per launch. The portal stays the
 * judge: it honours the token only for the id it bound it to, so the first request
 * of a process restores the saved grant with a dialog allowed
 * (GP_OPEN_RESTORE_OR_ASK) — silent when launched the same way, the dialog once when
 * launched from another context, whose new token then replaces the saved one. The
 * file is removed when the user declines a dialog that had no grant to keep,
 * declines the restore's dialog, or declines an ASK_AGAIN; when a silent restore
 * is not silent (the portal no longer honours the token) or is refused; when the
 * session is stopped from GNOME's indicator (any close that is not a lock, as
 * below); and by the kill switch (`displayxr-cli desktop stop` removes every saved
 * grant). A process whose file is removed under it keeps its grant in memory until
 * it exits but never writes the file again.
 *
 * A session is opened per action burst and closed after 2 s without input, so
 * GNOME's remote-access indicator is up only while the agent acts. Re-opening
 * restores silently with the token (whatever the last Start returned). Locking the
 * screen closes portal sessions (the Session's Closed signal, sent as the lock
 * starts — before ScreenSaver.GetActive says so, hence session_closed_by_portal's
 * 3 s wait); the next action after unlock re-opens one. Without a token (a portal that did not persist) the session is
 * kept open instead, since re-opening would prompt.
 *
 * ## Input
 *
 * libei, dlopen'd ("libei.so.1": no DT_NEEDED — it is absent on Ubuntu 22.04 and
 * is not one of package_deb_linux.sh's STABLE_SONAMES). Absolute pointer motion is
 * in the device regions' coordinates, which on GNOME are stage (logical) px — the
 * extension's space, and right on mixed / fractional scales. The portal's own
 * NotifyPointerMotionAbsolute D-Bus path is deliberately NOT used: it maps HiDPI
 * coordinates wrongly (the prototype clicked the wrong window with it).
 *
 * Chord keys: a character ("ctrl+z") is the key producing it on the ACTIVE layout
 * of the keymap libei hands out for the keyboard device (compiled with
 * xkbcommon, dlopen'd "libxkbcommon.so.0"; the active group follows
 * EI_EVENT_KEYBOARD_MODIFIERS) — GNOME matches shortcuts by keysym, so on AZERTY
 * ctrl+z must press the key labelled Z. Named keys are fixed evdev codes.
 *
 * ## Text
 *
 * Never key codes (an active IME — Korean, Japanese — rewrites them): the text is
 * offered on the clipboard through the portal (SetSelection; SelectionTransfer →
 * SelectionWrite → write → SelectionWriteDone) and pasted with Ctrl+V, or
 * Ctrl+Shift+V in a terminal. The previous clipboard text is read first
 * (SelectionRead) and offered again ~1.2 s after the paste keystroke (every read
 * until then gets the agent's text, so a clipboard manager reading first cannot
 * make a slow target paste the previous text). The selection changing hands
 * (SelectionOwnerChanged, session_is_owner false: the user copied) at any time
 * from before SelectionRead through SetSelection, up to the check after Ctrl is
 * down and before V, cancels the paste (Ctrl released without V); after V it
 * fails — "the user's clipboard, not the agent's text" when the agent's text was
 * never read, else unknown unless read AND the field changed; either way the
 * previous text is never offered over the user's. Delivery is confirmed by
 * the focused AT-SPI text field changing when there is one; else a read after
 * the keystroke (possibly a clipboard manager's) gives "ok (unverified)". A
 * reader closing its fd mid-write is EPIPE (SIGPIPE blocked for the write, on
 * this thread only — this is the app's process). LIMITATION: a portal
 * session's selection dies with the session, so ~2 s after the burst (the idle
 * close) GNOME's clipboard no longer holds either text unless a clipboard manager
 * kept a copy.
 *
 * ## Threading
 *
 * Everything here runs on ONE private thread with its own private session-bus
 * connection (never the observation connection). Callers post a command and
 * wait for it (bounded); a command a caller gave up on is dropped, or, if it is
 * already running, completes and is freed by the thread.
 */

#include "u_desktop_gnome_portal_priv.h"

#include "util/u_logging.h"

#include <dbus/dbus.h>
#include <linux/input-event-codes.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define REQUEST_IFACE "org.freedesktop.portal.Request"
#define SESSION_IFACE "org.freedesktop.portal.Session"
#define REGISTRY_IFACE "org.freedesktop.host.portal.Registry"

#define RD_DEVICE_KEYBOARD 1u
#define RD_DEVICE_POINTER 2u

//! Close the session after this long without input.
#define SESSION_IDLE_CLOSE_MS 2000
//! The interactive dialog.
#define INTERACTIVE_START_TIMEOUT_MS (10 * 60 * 1000)
//! After the portal closed our session: how long a lock may take to show up in
//! org.gnome.ScreenSaver.GetActive before the close is read as the user's Stop.
#define LOCK_SETTLE_MS 3000
#define LOCK_POLL_MS 100

bool
gnome_portal_libei_available(void)
{
	return gnome_libs_ei();
}

/*
 * The portal object
 */

void
gp_set_msg(char *msg, const char *fmt, ...)
{
	if (msg == NULL) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, U_DESKTOP_MSG_SIZE, fmt, ap);
	va_end(ap);
}

static void
wake_thread(struct gnome_portal *p)
{
	char b = 1;
	ssize_t r = write(p->wake[1], &b, 1);
	(void)r;
}

/*
 * a{sv} building and reading
 */

static void
dict_add_basic(DBusMessageIter *dict, const char *key, int type, const char *sig, const void *val)
{
	DBusMessageIter entry, var;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &var);
	dbus_message_iter_append_basic(&var, type, val);
	dbus_message_iter_close_container(&entry, &var);
	dbus_message_iter_close_container(dict, &entry);
}

static void
dict_add_str(DBusMessageIter *dict, const char *key, const char *val)
{
	dict_add_basic(dict, key, DBUS_TYPE_STRING, "s", &val);
}

static void
dict_add_u32(DBusMessageIter *dict, const char *key, uint32_t val)
{
	dbus_uint32_t v = val;
	dict_add_basic(dict, key, DBUS_TYPE_UINT32, "u", &v);
}

void
gp_dict_add_strv(DBusMessageIter *dict, const char *key, const char *const *vals, int n)
{
	DBusMessageIter entry, var, arr;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &var);
	dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &arr);
	for (int i = 0; i < n; i++) {
		dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &vals[i]);
	}
	dbus_message_iter_close_container(&var, &arr);
	dbus_message_iter_close_container(&entry, &var);
	dbus_message_iter_close_container(dict, &entry);
}


void
gp_pmsg_begin(struct pmsg *pm, const char *iface, const char *method, const char *session, const char *str)
{
	pm->m = dbus_message_new_method_call(PORTAL_DEST, PORTAL_PATH, iface, method);
	dbus_message_iter_init_append(pm->m, &pm->it);
	if (session != NULL) {
		dbus_message_iter_append_basic(&pm->it, DBUS_TYPE_OBJECT_PATH, &session);
	}
	if (str != NULL) {
		dbus_message_iter_append_basic(&pm->it, DBUS_TYPE_STRING, &str);
	}
	dbus_message_iter_open_container(&pm->it, DBUS_TYPE_ARRAY, "{sv}", &pm->dict);
}

//! Send; returns the reply (caller unrefs) or NULL with @p err filled.
DBusMessage *
gp_pmsg_send(struct gnome_portal *p, struct pmsg *pm, int timeout_ms, struct gdbus_err *err)
{
	dbus_message_iter_close_container(&pm->it, &pm->dict);
	DBusError e;
	dbus_error_init(&e);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(p->c, pm->m, timeout_ms, &e);
	dbus_message_unref(pm->m);
	pm->m = NULL;
	// Every call here goes to the portal: its reply names the portal's unique bus
	// name, the only sender whose Response / Closed / Clipboard signals count.
	const char *sender = reply != NULL ? dbus_message_get_sender(reply) : NULL;
	if (sender != NULL && strcmp(sender, p->portal_owner) != 0) {
		snprintf(p->portal_owner, sizeof(p->portal_owner), "%s", sender);
	}
	if (err != NULL) {
		err->name[0] = err->message[0] = '\0';
		if (dbus_error_is_set(&e)) {
			snprintf(err->name, sizeof(err->name), "%s", e.name ? e.name : "");
			snprintf(err->message, sizeof(err->message), "%s", e.message ? e.message : "");
		}
	}
	dbus_error_free(&e);
	return reply;
}

static void
next_token(struct gnome_portal *p, char *out, size_t cap)
{
	snprintf(out, cap, "dxr%u_%u", (unsigned)getpid(), ++p->token_counter);
}

/*
 * Response / signal handling
 */

static void
parse_results(struct gnome_portal *p, DBusMessageIter *arr)
{
	DBusMessageIter dict;
	dbus_message_iter_recurse(arr, &dict);
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, var;
		const char *key = NULL;
		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		if (dbus_message_iter_next(&entry) && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
			dbus_message_iter_recurse(&entry, &var);
			int t = dbus_message_iter_get_arg_type(&var);
			if (key && strcmp(key, "session_handle") == 0 &&
			    (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH)) {
				const char *s = NULL;
				dbus_message_iter_get_basic(&var, &s);
				snprintf(p->resp_session, sizeof(p->resp_session), "%s", s ? s : "");
			} else if (key && strcmp(key, "restore_token") == 0 && t == DBUS_TYPE_STRING) {
				const char *s = NULL;
				dbus_message_iter_get_basic(&var, &s);
				snprintf(p->resp_token, sizeof(p->resp_token), "%s", s ? s : "");
				p->resp_has_token = s != NULL && s[0] != '\0';
			} else if (key && strcmp(key, "clipboard_enabled") == 0 && t == DBUS_TYPE_BOOLEAN) {
				dbus_bool_t b = FALSE;
				dbus_message_iter_get_basic(&var, &b);
				p->resp_clip = b;
			} else if (key != NULL) {
				gp_cap_parse_result(p, key, &var); // "devices", "streams" (screen capture)
			}
		}
		dbus_message_iter_next(&dict);
	}
}


static void
ei_teardown(struct gnome_portal *p)
{
	gp_xkb_forget(p);
	for (int i = 0; i < GP_MAX_DEVICES; i++) {
		if (p->devs[i].dev != NULL) {
			g_ei.device_unref(p->devs[i].dev);
		}
		p->devs[i].dev = NULL;
		p->devs[i].resumed = false;
	}
	if (p->ei != NULL) {
		g_ei.unref(p->ei); // closes the EIS fd
		p->ei = NULL;
	}
	p->ei_dead = false;
}

void
gp_session_forget(struct gnome_portal *p)
{
	gp_cap_forget(p);
	ei_teardown(p);
	p->session_open = false;
	p->session[0] = '\0';
	p->clipboard_enabled = false;
	p->clip_owner = false;
	p->clip_is_paste = false;
	p->paste_armed = false;
	free(p->clip_text);
	p->clip_text = NULL;
	free(p->clip_prev);
	p->clip_prev = NULL;
	p->hold_until_ms = 0;
	pthread_mutex_lock(&p->lock);
	p->session_open_shared = false;
	if (!p->have_token && p->access == U_DESKTOP_ACCESS_GRANTED) {
		// Nothing to restore from: the next action needs a new request.
		p->access = U_DESKTOP_ACCESS_UNKNOWN;
	}
	pthread_mutex_unlock(&p->lock);
}

static void
session_close(struct gnome_portal *p)
{
	if (!p->session_open && p->session[0] == '\0') {
		ei_teardown(p);
		return;
	}
	ei_teardown(p);
	if (p->session[0] != '\0' && p->c != NULL) {
		DBusMessage *m = dbus_message_new_method_call(PORTAL_DEST, p->session, SESSION_IFACE, "Close");
		if (m != NULL) {
			dbus_message_set_no_reply(m, TRUE);
			dbus_connection_send(p->c, m, NULL);
			dbus_connection_flush(p->c);
			dbus_message_unref(m);
		}
	}
	U_LOG_I("desktop/gnome: closed the RemoteDesktop session");
	gp_session_forget(p);
}



//! org.gnome.ScreenSaver.GetActive on the portal connection (true only on a definite yes).
static bool
screen_locked_now(struct gnome_portal *p)
{
	DBusMessage *reply = gdbus_call(p->c, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver",
	                                "org.gnome.ScreenSaver", "GetActive", 500, NULL, DBUS_TYPE_INVALID);
	dbus_bool_t active = FALSE;
	if (reply != NULL) {
		dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &active, DBUS_TYPE_INVALID);
		dbus_message_unref(reply);
	}
	return active;
}

/*!
 * The portal's session went away under us: a lock (restorable) or the user
 * stopping it from GNOME's indicator (a revocation — never restored silently).
 *
 * The portal closes its sessions as the lock BEGINS, while the shield is still
 * animating in: GetActive reads false for a while after the Closed signal (seen
 * at +300 ms on GNOME 50). So a close is classified as a Stop only after
 * GetActive stayed false for LOCK_SETTLE_MS — a lock read at any point in that
 * window keeps the token (the next action after unlock restores silently, as a
 * lock with no session open already did).
 */
static void
session_closed_by_portal(struct gnome_portal *p)
{
	gp_session_forget(p);
	bool locked = screen_locked_now(p);
	uint64_t deadline = gdbus_now_ms() + LOCK_SETTLE_MS;
	while (!locked && gdbus_now_ms() < deadline && !gp_quitting(p)) {
		gdbus_sleep_ms(LOCK_POLL_MS);
		locked = screen_locked_now(p);
	}
	if (locked) {
		U_LOG_W("desktop/gnome: the screen lock closed the RemoteDesktop session; the next action restores it");
		return;
	}
	U_LOG_W(
	    "desktop/gnome: the RemoteDesktop session was stopped (not by a lock) — treating it as a revoked "
	    "grant; the app must request access again");
	char stopped[U_DESKTOP_GNOME_TOKEN_SIZE];
	pthread_mutex_lock(&p->lock);
	snprintf(stopped, sizeof(stopped), "%s", p->have_token ? p->token : "");
	p->have_token = false;
	memset(p->token, 0, sizeof(p->token));
	p->access = U_DESKTOP_ACCESS_DENIED;
	gp_cap_revoked_locked(p); // one session carries both scopes
	pthread_mutex_unlock(&p->lock);
	// The user's Stop ends the saved grant too: the next launch asks again.
	if (stopped[0] != '\0') {
		gp_grant_forget(p, stopped, "the session was stopped, not by a lock");
	}
	memset(stopped, 0, sizeof(stopped));
}

static void
handle_message(struct gnome_portal *p, DBusMessage *m)
{
	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_SIGNAL) {
		return;
	}
	if (dbus_message_is_signal(m, DBUS_INTERFACE_LOCAL, "Disconnected")) {
		U_LOG_W("desktop/gnome: lost the portal D-Bus connection");
		gp_session_forget(p);
		return;
	}
	// Everything else must come from the portal itself: any bus peer can emit a
	// signal named Response / Closed / SelectionTransfer (a forged grant).
	const char *sender = dbus_message_get_sender(m);
	if (sender == NULL || p->portal_owner[0] == '\0' || strcmp(sender, p->portal_owner) != 0) {
		return;
	}
	const char *path = dbus_message_get_path(m);
	if (dbus_message_is_signal(m, REQUEST_IFACE, "Response")) {
		if (path == NULL || strcmp(path, p->wait_path) != 0) {
			return;
		}
		DBusMessageIter it;
		if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
			return;
		}
		dbus_uint32_t code = 2;
		dbus_message_iter_get_basic(&it, &code);
		p->resp_code = code;
		if (dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
			parse_results(p, &it);
		}
		p->resp_done = true;
	} else if (dbus_message_is_signal(m, SESSION_IFACE, "Closed")) {
		if (path != NULL && p->session[0] != '\0' && strcmp(path, p->session) == 0) {
			session_closed_by_portal(p);
		}
	} else if (dbus_message_is_signal(m, CLIP_IFACE, "SelectionTransfer")) {
		const char *session = NULL, *mime = NULL;
		dbus_uint32_t serial = 0;
		if (dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &session, DBUS_TYPE_STRING, &mime,
		                          DBUS_TYPE_UINT32, &serial, DBUS_TYPE_INVALID) &&
		    p->session_open && strcmp(session, p->session) == 0) {
			gp_serve_transfer(p, mime, serial);
		}
	} else if (dbus_message_is_signal(m, CLIP_IFACE, "SelectionOwnerChanged")) {
		DBusMessageIter it;
		const char *session = NULL;
		if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_OBJECT_PATH) {
			return;
		}
		dbus_message_iter_get_basic(&it, &session);
		if (!p->session_open || strcmp(session, p->session) != 0 || !dbus_message_iter_next(&it) ||
		    dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
			return;
		}
		bool owner = false;
		DBusMessageIter dict;
		dbus_message_iter_recurse(&it, &dict);
		while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
			DBusMessageIter entry, var;
			const char *key = NULL;
			dbus_message_iter_recurse(&dict, &entry);
			dbus_message_iter_get_basic(&entry, &key);
			if (key && strcmp(key, "session_is_owner") == 0 && dbus_message_iter_next(&entry)) {
				dbus_message_iter_recurse(&entry, &var);
				if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BOOLEAN) {
					dbus_bool_t b = FALSE;
					dbus_message_iter_get_basic(&var, &b);
					owner = b;
				}
			}
			dbus_message_iter_next(&dict);
		}
		if (!owner) {
			p->clip_foreign_gen++;
		}
		if (!owner && p->clip_owner && p->clip_is_paste) {
			// Someone else took the selection while it held the agent's text.
			p->clip_lost = true;
		}
		p->clip_owner = owner;
	}
}

/*
 * libei events
 */

static void
ei_pump(struct gnome_portal *p)
{
	if (p->ei == NULL) {
		return;
	}
	g_ei.dispatch(p->ei);
	struct ei_event *ev;
	while ((ev = g_ei.get_event(p->ei)) != NULL) {
		int type = g_ei.event_get_type(ev);
		switch (type) {
		case EIX_EVENT_CONNECT: U_LOG_I("desktop/gnome: libei connected"); break;
		case EIX_EVENT_DISCONNECT:
			U_LOG_W("desktop/gnome: the EIS server disconnected libei");
			p->ei_dead = true;
			break;
		case EIX_EVENT_SEAT_ADDED: {
			struct ei_seat *seat = g_ei.event_get_seat(ev);
			g_ei.seat_bind_capabilities(seat, EIX_CAP_POINTER_ABSOLUTE, EIX_CAP_BUTTON, EIX_CAP_SCROLL,
			                            EIX_CAP_KEYBOARD, NULL);
			break;
		}
		case EIX_EVENT_DEVICE_ADDED: {
			struct ei_device *d = g_ei.event_get_device(ev);
			for (int i = 0; i < GP_MAX_DEVICES; i++) {
				if (p->devs[i].dev == NULL) {
					p->devs[i].dev = g_ei.device_ref(d);
					p->devs[i].resumed = false;
					U_LOG_I("desktop/gnome: libei device \"%s\"%s%s%s%s", g_ei.device_get_name(d),
					        g_ei.device_has_capability(d, EIX_CAP_POINTER_ABSOLUTE) ? " abs" : "",
					        g_ei.device_has_capability(d, EIX_CAP_BUTTON) ? " button" : "",
					        g_ei.device_has_capability(d, EIX_CAP_SCROLL) ? " scroll" : "",
					        g_ei.device_has_capability(d, EIX_CAP_KEYBOARD) ? " keyboard" : "");
					break;
				}
			}
			break;
		}
		case EIX_EVENT_DEVICE_REMOVED: {
			struct ei_device *d = g_ei.event_get_device(ev);
			for (int i = 0; i < GP_MAX_DEVICES; i++) {
				if (p->devs[i].dev == d) {
					if (p->xkb_dev == d) {
						gp_xkb_forget(p);
					}
					g_ei.device_unref(d);
					p->devs[i].dev = NULL;
					p->devs[i].resumed = false;
				}
			}
			break;
		}
		case EIX_EVENT_DEVICE_PAUSED:
		case EIX_EVENT_DEVICE_RESUMED: {
			struct ei_device *d = g_ei.event_get_device(ev);
			for (int i = 0; i < GP_MAX_DEVICES; i++) {
				if (p->devs[i].dev == d) {
					p->devs[i].resumed = type == EIX_EVENT_DEVICE_RESUMED;
					if (p->devs[i].resumed) {
						g_ei.device_start_emulating(d, ++p->seq);
					}
				}
			}
			break;
		}
		case EIX_EVENT_KEYBOARD_MODIFIERS:
			if (g_ei.event_keyboard_get_xkb_group != NULL) {
				p->xkb_group = g_ei.event_keyboard_get_xkb_group(ev);
			}
			break;
		default: break;
		}
		g_ei.event_unref(ev);
	}
	if (p->ei_dead) {
		ei_teardown(p);
	}
}

//! A resumed device with @p cap; with @p prefer_abs, one that is also absolute first.
struct ei_device *
gp_find_dev(struct gnome_portal *p, int cap, bool prefer_abs)
{
	struct ei_device *any = NULL;
	for (int i = 0; i < GP_MAX_DEVICES; i++) {
		struct ei_device *d = p->devs[i].dev;
		if (d == NULL || !p->devs[i].resumed || !g_ei.device_has_capability(d, cap)) {
			continue;
		}
		if (!prefer_abs || g_ei.device_has_capability(d, EIX_CAP_POINTER_ABSOLUTE)) {
			return d;
		}
		if (any == NULL) {
			any = d;
		}
	}
	return any;
}

/*
 * The loop
 */

//! Wait up to @p timeout_ms for anything (a command, a D-Bus message, libei) and handle it.
void
gp_pump(struct gnome_portal *p, int timeout_ms)
{
	struct pollfd fds[3];
	int n = 0;
	fds[n++] = (struct pollfd){p->wake[0], POLLIN, 0};
	int dbus_fd = -1;
	if (p->c != NULL && dbus_connection_get_unix_fd(p->c, &dbus_fd)) {
		fds[n++] = (struct pollfd){dbus_fd, POLLIN, 0};
	}
	if (p->ei != NULL) {
		fds[n++] = (struct pollfd){g_ei.get_fd(p->ei), POLLIN, 0};
	}
	if (p->c != NULL && dbus_connection_get_dispatch_status(p->c) == DBUS_DISPATCH_DATA_REMAINS) {
		timeout_ms = 0;
	}
	poll(fds, (nfds_t)n, timeout_ms);
	if (fds[0].revents & POLLIN) {
		char buf[64];
		while (read(p->wake[0], buf, sizeof(buf)) > 0) {
		}
	}
	if (p->c != NULL) {
		dbus_connection_read_write(p->c, 0);
		DBusMessage *m;
		while ((m = dbus_connection_pop_message(p->c)) != NULL) {
			handle_message(p, m);
			dbus_message_unref(m);
		}
	}
	ei_pump(p);
}

bool
gp_quitting(struct gnome_portal *p)
{
	pthread_mutex_lock(&p->lock);
	bool q = p->quit;
	pthread_mutex_unlock(&p->lock);
	return q;
}

//! Wait for the Response of request @p path.
static bool
wait_response(struct gnome_portal *p, uint32_t timeout_ms)
{
	uint64_t deadline = gdbus_now_ms() + timeout_ms;
	while (!p->resp_done) {
		uint64_t now = gdbus_now_ms();
		if (now >= deadline || gp_quitting(p) || p->c == NULL) {
			return false;
		}
		uint64_t left = deadline - now;
		gp_pump(p, left > 100 ? 100 : (int)left);
	}
	return true;
}

static void
expect_response(struct gnome_portal *p, const char *token)
{
	snprintf(p->wait_path, sizeof(p->wait_path), "%s/request/%s/%s", PORTAL_PATH, p->sender, token);
	p->resp_done = false;
	p->resp_code = 2;
	p->resp_session[0] = '\0';
	p->resp_token[0] = '\0';
	p->resp_has_token = false;
	p->resp_clip = false;
	gp_cap_expect(p);
}

//! The request handle the method returned (older portals may not follow the token convention).
static void
take_request_handle(struct gnome_portal *p, DBusMessage *reply)
{
	const char *handle = NULL;
	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &handle, DBUS_TYPE_INVALID) && handle &&
	    strcmp(handle, p->wait_path) != 0) {
		snprintf(p->wait_path, sizeof(p->wait_path), "%s", handle);
	}
}

static void
app_id_from_env(char *out, size_t cap)
{
	out[0] = '\0';
	const char *id = getenv("DXR_DESKTOP_APP_ID");
	if (id != NULL && id[0] != '\0') {
		snprintf(out, cap, "%s", id);
		return;
	}
	const char *df = getenv("GIO_LAUNCHED_DESKTOP_FILE");
	if (df != NULL && df[0] != '\0') {
		const char *base = strrchr(df, '/');
		base = base ? base + 1 : df;
		snprintf(out, cap, "%s", base);
		size_t len = strlen(out);
		if (len > 8 && strcmp(out + len - 8, ".desktop") == 0) {
			out[len - 8] = '\0';
		}
	}
}

//! org.freedesktop.host.portal.Registry.Register — must precede every other portal call on the connection.
static void
register_app_once(struct gnome_portal *p)
{
	if (p->registered_tried) {
		return;
	}
	p->registered_tried = true;
	char app_id[256];
	app_id_from_env(app_id, sizeof(app_id));
	if (app_id[0] == '\0') {
		U_LOG_W(
		    "desktop/gnome: no app id (set DXR_DESKTOP_APP_ID, or launch from a .desktop file) — the "
		    "portal attributes the request to the launch context and its dialog names no app; the grant is "
		    "kept per executable");
		return;
	}
	struct pmsg pm;
	gp_pmsg_begin(&pm, REGISTRY_IFACE, "Register", NULL, app_id);
	struct gdbus_err err;
	DBusMessage *reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply != NULL) {
		U_LOG_I("desktop/gnome: registered host app id \"%s\" with the portal", app_id);
		dbus_message_unref(reply);
		snprintf(p->registered_id, sizeof(p->registered_id), "%s", app_id);
	} else {
		U_LOG_W(
		    "desktop/gnome: could not register app id \"%s\" with the portal (%s%s%s) — needs "
		    "xdg-desktop-portal >= 1.19 and an installed %s.desktop; the portal attributes the request to "
		    "the launch context and its dialog names no app (the grant is still kept, under that id)",
		    app_id, err.name, err.message[0] ? ": " : "", err.message, app_id);
	}
}


/*
 * The saved grant (spec §6.4, ADR-056 Amendment 2; u_desktop_gnome_grant.h).
 */

//! Take @p g as this process's grant. Lock held.
static void
grant_adopt_locked(struct gnome_portal *p, const struct u_desktop_gnome_grant *g)
{
	p->have_token = true;
	snprintf(p->token, sizeof(p->token), "%s", g->token);
	p->cap.token_sc = g->screencast;
	p->cap.token_devices = g->devices;
	p->cap.token_screens = g->screens;
}

//! ... and its covered monitors (portal-thread fields), for the monitor-blink guard.
static void
grant_adopt_monitors(struct gnome_portal *p, const struct u_desktop_gnome_grant *g)
{
	uint32_t n = g->screens ? g->nmonitors : 0;
	const uint32_t cap = sizeof(p->cap.covered) / sizeof(p->cap.covered[0]);
	n = n > cap ? cap : n;
	for (uint32_t i = 0; i < n; i++) {
		snprintf(p->cap.covered[i], sizeof(p->cap.covered[0]), "%s", g->monitors[i]);
	}
	p->cap.ncovered = n;
	// No layout change seen yet: restore as soon as every covered monitor is there.
	memset(&p->cap.layout, 0, sizeof(p->cap.layout));
}

//! The kill switch is on (or the store is unreadable): never save then.
static bool
kill_switch_on(void)
{
	struct u_desktop_policy *pol = u_desktop_policy_create();
	bool on = pol == NULL || u_desktop_policy_is_suspended(pol);
	u_desktop_policy_destroy(&pol);
	return on;
}

static void
grant_disown(struct gnome_portal *p)
{
	p->saved.on_disk = false;
	p->saved.disowned = true;
	memset(p->saved.token, 0, sizeof(p->saved.token));
	U_LOG_W(
	    "desktop/gnome: the saved grant of \"%s\" was removed (the kill switch, or by hand) — this process "
	    "keeps its grant until it exits but no longer saves it",
	    p->saved.key);
}

void
gp_grant_sync(struct gnome_portal *p)
{
	bool first = !p->saved.inited;
	if (first) {
		p->saved.inited = true;
		// The app's own identity: its registered app id, else its own app id (even
		// unregistered), else its executable. The portal judges the token; the key
		// only says whose file it is.
		if (p->c != NULL) {
			register_app_once(p);
		}
		char env_id[256];
		app_id_from_env(env_id, sizeof(env_id));
		u_desktop_self_identity(p->saved.exe, sizeof(p->saved.exe), NULL, 0);
		u_desktop_gnome_launch_scope(p->saved.scope, sizeof(p->saved.scope));
		p->saved.source = u_desktop_gnome_grant_key(p->registered_id, env_id, p->saved.exe, p->saved.key,
		                                            sizeof(p->saved.key));
		if (p->saved.source == U_DESKTOP_GNOME_KEY_NONE) {
			U_LOG_W(
			    "desktop/gnome: nothing to key a saved grant by (no app id, no executable path) — not "
			    "saved");
			return;
		}
		U_LOG_I("desktop/gnome: the grant is kept under \"%s\" (%s)", p->saved.key,
		        u_desktop_gnome_key_source_str(p->saved.source));
	}
	if (p->saved.key[0] == '\0' || p->saved.disowned) {
		return;
	}
	struct u_desktop_gnome_grant g;
	enum u_desktop_gnome_grant_status st = u_desktop_gnome_grant_load(p->saved.key, &g);
	if (st == U_DESKTOP_GNOME_GRANT_NONE) {
		if (p->saved.on_disk) {
			grant_disown(p);
		}
		return;
	}
	if (st != U_DESKTOP_GNOME_GRANT_OK) {
		if (first) {
			U_LOG_W("desktop/gnome: ignored the saved grant of \"%s\" (%s%s) — the dialog asks again",
			        p->saved.key, u_desktop_gnome_grant_status_str(st),
			        st == U_DESKTOP_GNOME_GRANT_CORRUPT  ? ": removed"
			        : st == U_DESKTOP_GNOME_GRANT_UNSAFE ? ": not a private file of this user"
			                                             : ": written by a newer runtime");
		}
		return;
	}
	if (p->saved.on_disk && strcmp(g.token, p->saved.token) == 0) {
		memset(&g, 0, sizeof(g));
		return; // the file is what this process saved or loaded
	}
	// First use, or another process of this app saved a grant since (a Start may
	// renew the token, spending ours): take it, unless ours covers something else
	// (never trade a grant for one of another shape).
	pthread_mutex_lock(&p->lock);
	bool have = p->have_token;
	bool take = !have || (p->cap.token_sc == g.screencast && p->cap.token_devices == g.devices &&
	                      p->cap.token_screens == g.screens);
	if (take) {
		grant_adopt_locked(p, &g);
	}
	pthread_mutex_unlock(&p->lock);
	if (take) {
		grant_adopt_monitors(p, &g);
		p->saved.on_disk = true;
		p->saved.unverified = true; // the portal has not seen it in this process yet
		snprintf(p->saved.token, sizeof(p->saved.token), "%s", g.token);
		if (!have) {
			U_LOG_W(
			    "desktop/gnome: using the saved grant of \"%s\" (%s%s%s) — no dialog while the portal "
			    "honours it",
			    p->saved.key, g.screencast ? "screen cast: " : "", g.devices ? "input" : "",
			    g.screens ? (g.devices ? " + screens" : "screens") : "");
		} else {
			U_LOG_I("desktop/gnome: took the grant another process of \"%s\" saved since", p->saved.key);
		}
	}
	memset(&g, 0, sizeof(g));
}

void
gp_grant_save(struct gnome_portal *p)
{
	p->saved.unverified = false; // a Start succeeded: what is in memory is the portal's answer here
	if (p->saved.key[0] == '\0' || p->saved.disowned) {
		return;
	}
	struct u_desktop_gnome_grant g;
	memset(&g, 0, sizeof(g));
	pthread_mutex_lock(&p->lock);
	bool have = p->have_token;
	if (have) {
		snprintf(g.token, sizeof(g.token), "%s", p->token);
		g.screencast = p->cap.token_sc;
		g.devices = p->cap.token_devices;
		g.screens = p->cap.token_screens;
	}
	pthread_mutex_unlock(&p->lock);
	if (!have || !(g.devices || g.screens)) {
		// Nothing restorable: the token the file held was spent by this Start.
		if (p->saved.on_disk) {
			char spent[U_DESKTOP_GNOME_TOKEN_SIZE];
			snprintf(spent, sizeof(spent), "%s", p->saved.token);
			gp_grant_forget(p, spent, "the new session has nothing to restore");
			memset(spent, 0, sizeof(spent));
		}
		memset(&g, 0, sizeof(g));
		return;
	}
	if (g.screens) {
		const uint32_t cap = sizeof(g.monitors) / sizeof(g.monitors[0]);
		g.nmonitors = p->cap.ncovered > cap ? cap : p->cap.ncovered;
		for (uint32_t i = 0; i < g.nmonitors; i++) {
			snprintf(g.monitors[i], sizeof(g.monitors[0]), "%s", p->cap.covered[i]);
		}
	}
	// Diagnostics in the file: whose key, which id the portal used (when known), what ran.
	const struct u_desktop_gnome_grant_origin origin = {
	    .source = p->saved.source,
	    .portal_app_id = p->registered_id,
	    .exe = p->saved.exe,
	    .launch_scope = p->saved.scope,
	};
	if (p->saved.on_disk && !u_desktop_grant_exists(p->saved.key)) {
		grant_disown(p); // removed under us: never brought back by this process
	} else if (kill_switch_on()) {
		U_LOG_W("desktop/gnome: the kill switch is on — the grant is not saved");
	} else if (u_desktop_gnome_grant_save(p->saved.key, &g, &origin)) {
		p->saved.on_disk = true;
		snprintf(p->saved.token, sizeof(p->saved.token), "%s", g.token);
		U_LOG_I("desktop/gnome: saved the grant of \"%s\" for its next launch", p->saved.key);
	} else {
		U_LOG_W("desktop/gnome: could not save the grant of \"%s\" — its next launch asks again", p->saved.key);
	}
	memset(&g, 0, sizeof(g));
}

void
gp_grant_forget(struct gnome_portal *p, const char *token, const char *why)
{
	if (p->saved.key[0] == '\0') {
		return;
	}
	if (u_desktop_gnome_grant_forget(p->saved.key, token)) {
		U_LOG_W("desktop/gnome: removed the saved grant of \"%s\" (%s) — the next request shows the dialog",
		        p->saved.key, why);
	}
	p->saved.on_disk = false;
	memset(p->saved.token, 0, sizeof(p->saved.token));
}

static bool
ei_ready(struct gnome_portal *p)
{
	return p->ei != NULL && gp_find_dev(p, EIX_CAP_POINTER_ABSOLUTE, false) != NULL &&
	       gp_find_dev(p, EIX_CAP_KEYBOARD, false) != NULL;
}

static enum u_desktop_result
connect_eis(struct gnome_portal *p, char *msg)
{
	struct pmsg pm;
	gp_pmsg_begin(&pm, RD_IFACE, "ConnectToEIS", p->session, NULL);
	struct gdbus_err err;
	DBusMessage *reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	int fd = -1;
	if (reply == NULL || !dbus_message_get_args(reply, NULL, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID) || fd < 0) {
		gp_set_msg(msg, "ConnectToEIS failed: %s %s", err.name, err.message);
		if (reply != NULL) {
			dbus_message_unref(reply);
		}
		return U_DESKTOP_ERR_FAILED;
	}
	dbus_message_unref(reply);

	p->ei = g_ei.new_sender(NULL);
	if (p->ei == NULL) {
		close(fd);
		gp_set_msg(msg, "ei_new_sender failed");
		return U_DESKTOP_ERR_FAILED;
	}
	g_ei.configure_name(p->ei, "DisplayXR desktop control");
	if (g_ei.setup_backend_fd(p->ei, fd) != 0) { // takes the fd
		ei_teardown(p);
		gp_set_msg(msg, "ei_setup_backend_fd failed");
		return U_DESKTOP_ERR_FAILED;
	}
	uint64_t deadline = gdbus_now_ms() + 3000;
	while (!ei_ready(p) && p->ei != NULL && gdbus_now_ms() < deadline && !gp_quitting(p)) {
		gp_pump(p, 20);
	}
	if (p->ei == NULL) {
		gp_set_msg(msg, "the EIS server refused the libei connection");
		return U_DESKTOP_ERR_FAILED;
	}
	if (!ei_ready(p)) {
		U_LOG_W("desktop/gnome: libei: no%s%s device resumed within 3 s — continuing with what there is",
		        gp_find_dev(p, EIX_CAP_POINTER_ABSOLUTE, false) ? "" : " absolute-pointer",
		        gp_find_dev(p, EIX_CAP_KEYBOARD, false) ? "" : " keyboard");
	}
	return U_DESKTOP_OK;
}

//! CreateSession → SelectDevices → RequestClipboard → Start → ConnectToEIS. See enum gp_open.
static enum u_desktop_result
open_session(struct gnome_portal *p, enum gp_open mode, char *msg)
{
	// Restoring: the token's shape and the monitor-blink guard. A dialog: the long wait.
	const bool restoring = mode != GP_OPEN_ASK;
	const bool interactive = mode != GP_OPEN_RESTORE;
	if (p->c == NULL) {
		gp_set_msg(msg, "no D-Bus session bus");
		return U_DESKTOP_ERR_FAILED;
	}
	register_app_once(p);
	session_close(p);
	p->start_declined = false;
	if (restoring) {
		// Never restore a screen grant while a monitor it covers is missing or the
		// layout just changed: GNOME would save the reduced approval.
		enum u_desktop_result rr = gp_cap_before_restore(p, msg);
		if (rr != U_DESKTOP_OK) {
			return rr;
		}
	}

	char token[64], stoken[64];
	struct gdbus_err err;

	// CreateSession
	next_token(p, token, sizeof(token));
	next_token(p, stoken, sizeof(stoken));
	expect_response(p, token);
	struct pmsg pm;
	gp_pmsg_begin(&pm, RD_IFACE, "CreateSession", NULL, NULL);
	dict_add_str(&pm.dict, "handle_token", token);
	dict_add_str(&pm.dict, "session_handle_token", stoken);
	DBusMessage *reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply == NULL) {
		gp_set_msg(msg, "RemoteDesktop.CreateSession failed: %s %s", err.name, err.message);
		return U_DESKTOP_ERR_FAILED;
	}
	take_request_handle(p, reply);
	dbus_message_unref(reply);
	if (!wait_response(p, CALL_TIMEOUT_MS) || p->resp_code != 0 || p->resp_session[0] == '\0') {
		gp_set_msg(msg, "RemoteDesktop.CreateSession: no session (response %u)", p->resp_code);
		return U_DESKTOP_ERR_FAILED;
	}
	snprintf(p->session, sizeof(p->session), "%s", p->resp_session);
	p->session_open = true;

	// SelectDevices
	char restore[1024] = {0};
	pthread_mutex_lock(&p->lock);
	if (p->have_token) {
		snprintf(restore, sizeof(restore), "%s", p->token);
	}
	pthread_mutex_unlock(&p->lock);
	next_token(p, token, sizeof(token));
	expect_response(p, token);
	gp_pmsg_begin(&pm, RD_IFACE, "SelectDevices", p->session, NULL);
	dict_add_str(&pm.dict, "handle_token", token);
	dict_add_u32(&pm.dict, "types", RD_DEVICE_KEYBOARD | RD_DEVICE_POINTER);
	if (p->rd_version >= 2 && p->input) {
		dict_add_u32(&pm.dict, "persist_mode", 2); // until revoked
		if (restore[0] != '\0') {
			dict_add_str(&pm.dict, "restore_token", restore);
		}
	}
	reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply == NULL) {
		gp_set_msg(msg, "RemoteDesktop.SelectDevices failed: %s %s", err.name, err.message);
		session_close(p);
		return U_DESKTOP_ERR_FAILED;
	}
	take_request_handle(p, reply);
	dbus_message_unref(reply);
	if (!wait_response(p, CALL_TIMEOUT_MS) || p->resp_code != 0) {
		gp_set_msg(msg, "RemoteDesktop.SelectDevices refused (response %u)", p->resp_code);
		session_close(p);
		return U_DESKTOP_ERR_FAILED;
	}

	// The screens, in the same session and the same dialog (screen capture).
	enum u_desktop_result sr = gp_cap_select_sources(p, !restoring, msg);
	if (sr != U_DESKTOP_OK) {
		session_close(p);
		return sr;
	}

	// RequestClipboard — before Start. No Request object, no response.
	if (p->have_clipboard_portal && p->input) {
		gp_pmsg_begin(&pm, CLIP_IFACE, "RequestClipboard", p->session, NULL);
		reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
		if (reply != NULL) {
			dbus_message_unref(reply);
		} else {
			U_LOG_W("desktop/gnome: Clipboard.RequestClipboard failed (%s) — type_text unavailable",
			        err.name);
		}
	}

	// Start — the dialog (or a silent restore).
	next_token(p, token, sizeof(token));
	expect_response(p, token);
	// parent_window ("" = none): GNOME opens the dialog on the app's window; a handle
	// it cannot resolve (a stale xdg-foreign export) is logged and ignored there.
	gp_pmsg_begin(&pm, RD_IFACE, "Start", p->session, p->parent_window);
	dict_add_str(&pm.dict, "handle_token", token);
	reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply == NULL) {
		gp_set_msg(msg, "RemoteDesktop.Start failed: %s %s", err.name, err.message);
		session_close(p);
		return U_DESKTOP_ERR_FAILED;
	}
	take_request_handle(p, reply);
	dbus_message_unref(reply);
	uint32_t start_timeout = interactive ? INTERACTIVE_START_TIMEOUT_MS : RESTORE_START_TIMEOUT_MS;
	if (!wait_response(p, start_timeout)) {
		// Closing the session alone leaves a portal dialog up (seen on GNOME 50):
		// close the pending request first.
		gp_cap_close_request(p);
		session_close(p); // also dismisses a dialog nobody is answering
		if (interactive) {
			gp_set_msg(msg, "no answer to the remote-desktop dialog");
			return U_DESKTOP_ERR_TIMEOUT;
		}
		// A restore that is not silent means the portal no longer honours the
		// token (revoked / expired): drop it, so later actions fail at once
		// instead of waiting here again — and the saved copy with it.
		pthread_mutex_lock(&p->lock);
		p->have_token = false;
		memset(p->token, 0, sizeof(p->token));
		p->access = U_DESKTOP_ACCESS_DENIED;
		gp_cap_revoked_locked(p);
		pthread_mutex_unlock(&p->lock);
		gp_grant_forget(p, restore, "the portal no longer restores it silently");
		memset(restore, 0, sizeof(restore));
		gp_set_msg(msg,
		           "the portal did not restore the session silently (grant revoked?) — request access again");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	if (p->resp_code != 0) {
		bool cancelled = p->resp_code == 1;
		p->start_declined = cancelled;
		gp_set_msg(msg, cancelled ? "the user declined remote control" : "the portal refused the session");
		session_close(p);
		if (restoring) {
			// The token is no good any more (nor its saved copy). After a restore that
			// showed its dialog (GP_OPEN_RESTORE_OR_ASK), a Cancel is the user's no.
			pthread_mutex_lock(&p->lock);
			p->have_token = false;
			memset(p->token, 0, sizeof(p->token));
			p->access = U_DESKTOP_ACCESS_DENIED;
			gp_cap_revoked_locked(p);
			pthread_mutex_unlock(&p->lock);
			gp_grant_forget(p, restore,
			                mode == GP_OPEN_RESTORE ? "the portal refused to restore it"
			                : cancelled             ? "the user declined the restore's dialog"
			                                        : "the portal refused it");
		}
		memset(restore, 0, sizeof(restore));
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	memset(restore, 0, sizeof(restore));
	p->clipboard_enabled = p->resp_clip;
	pthread_mutex_lock(&p->lock);
	// Tokens are single use: keep the new one, or none.
	p->have_token = p->resp_has_token;
	snprintf(p->token, sizeof(p->token), "%s", p->resp_has_token ? p->resp_token : "");
	p->session_open_shared = true;
	bool have_token = p->have_token;
	pthread_mutex_unlock(&p->lock);
	// What the Start response granted (input? screens?) + the PipeWire remote.
	bool devices = gp_cap_after_start(p);
	// The renewed token (or none) for the next launch.
	gp_grant_save(p);
	if (!p->input) {
		// Consent only: the user answered; nothing to drive.
		U_LOG_I("desktop/gnome: remote-control consent granted (no libei: observation only)");
		session_close(p);
		return U_DESKTOP_OK;
	}
	if (!have_token && !p->warned_no_token) {
		p->warned_no_token = true;
		U_LOG_W(
		    "desktop/gnome: the portal returned no restore token (RemoteDesktop v%u) — keeping the session "
		    "open instead of closing it when idle",
		    p->rd_version);
	}
	if (!p->clipboard_enabled && p->have_clipboard_portal) {
		U_LOG_W("desktop/gnome: the session has no clipboard access — type_text unavailable");
	}
	if (!devices) {
		// The user left "allow remote interaction" off: screens only, no libei.
		U_LOG_W("desktop/gnome: the session grants no input (remote interaction left off)");
		p->last_use_ms = gdbus_now_ms();
		return U_DESKTOP_OK;
	}

	enum u_desktop_result r = connect_eis(p, msg);
	if (r != U_DESKTOP_OK) {
		session_close(p);
		return r;
	}
	p->last_use_ms = gdbus_now_ms();
	U_LOG_I("desktop/gnome: RemoteDesktop session %s (%s)", restoring ? "restored" : "granted", p->session);
	return U_DESKTOP_OK;
}

enum u_desktop_result
gp_ensure_session(struct gnome_portal *p, bool restore, char *msg)
{
	if (!p->input) {
		gp_set_msg(msg, "no input on this system (libei.so.1 missing, or RemoteDesktop portal < v2)");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	if (!(p->session_open && p->ei != NULL)) {
		gp_grant_sync(p); // a restore may follow: the saved grant (first use / renewed elsewhere)
	}
	pthread_mutex_lock(&p->lock);
	// Only a grant that covers input restores for input (not a screens-only one).
	bool have_token = p->have_token && !p->cap.token_sc && p->cap.token_devices;
	enum u_desktop_access acc = p->access;
	pthread_mutex_unlock(&p->lock);
	if (acc == U_DESKTOP_ACCESS_DENIED) {
		gp_set_msg(msg, "remote control was declined or revoked — request access again");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	if (p->session_open && p->ei != NULL) {
		p->last_use_ms = gdbus_now_ms();
		return U_DESKTOP_OK;
	}
	if (p->session_open && p->cap.session_sc) {
		gp_session_close(p); // a screens-only session: reopened below as RemoteDesktop
	}
	if (!have_token) {
		gp_set_msg(msg, acc == U_DESKTOP_ACCESS_GRANTED
		                    ? "the remote-desktop session ended and cannot be restored — request access again"
		                    : "remote control was not granted to this app — request access first");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	if (!restore) {
		// After verification: restoring here would put seconds between the
		// check and the injection. prepare_input is where sessions are opened.
		gp_set_msg(msg, "the remote-desktop session closed mid-action (screen lock?) — retry");
		return U_DESKTOP_ERR_REFUSED;
	}
	return open_session(p, GP_OPEN_RESTORE, msg);
}

/*
 * Commands (run on the thread)
 */

static enum u_desktop_result
cmd_request(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_access acc;
	snprintf(p->parent_window, sizeof(p->parent_window), "%s", c->parent_window);
	gp_grant_sync(p); // the app's saved grant: no dialog when it covers input
	pthread_mutex_lock(&p->lock);
	bool already = gp_cap_control_granted_locked(p) || p->consent_only_granted;
	pthread_mutex_unlock(&p->lock);
	if (already && p->saved.unverified && !p->session_open) {
		// A saved grant this process has not used: the portal's answer, now —
		// silent when launched as it was issued, else its dialog once.
		enum u_desktop_result r = gp_grant_verify(p, c->msg);
		pthread_mutex_lock(&p->lock);
		bool still = gp_cap_control_granted_locked(p);
		pthread_mutex_unlock(&p->lock);
		acc = still ? U_DESKTOP_ACCESS_GRANTED : U_DESKTOP_ACCESS_DENIED;
		if (r != U_DESKTOP_OK) {
			U_LOG_W("desktop/gnome: access request (saved grant): %s", c->msg);
		}
	} else if (already) {
		acc = U_DESKTOP_ACCESS_GRANTED;
	} else {
		// A screens-only grant cannot be widened: ask fresh, keeping the screens
		// in the dialog (never downgrade), and the old grant if this one fails.
		struct gp_cap_saved saved;
		gp_cap_begin_upgrade(p, &saved);
		bool had = saved.have_token;
		enum u_desktop_result r = open_session(p, GP_OPEN_ASK, c->msg);
		bool declined = r != U_DESKTOP_OK && p->start_declined;
		gp_cap_end_upgrade(p, &saved, r == U_DESKTOP_OK);
		// A Cancel with no grant to keep: nothing of this app's is saved any more.
		if (r != U_DESKTOP_OK && u_desktop_gnome_grant_after_request(false, false, declined, false, had) ==
		                             U_DESKTOP_GNOME_GRANT_FORGET) {
			gp_grant_forget(p, NULL, "the user declined the dialog");
		}
		pthread_mutex_lock(&p->lock);
		bool granted = r == U_DESKTOP_OK && (gp_cap_control_granted_locked(p) || !p->input);
		pthread_mutex_unlock(&p->lock);
		acc = granted ? U_DESKTOP_ACCESS_GRANTED : U_DESKTOP_ACCESS_DENIED;
		if (r != U_DESKTOP_OK && gp_quitting(p)) {
			acc = U_DESKTOP_ACCESS_UNKNOWN; // torn down before the user answered
		}
		if (r == U_DESKTOP_OK && !p->input) {
			pthread_mutex_lock(&p->lock);
			p->consent_only_granted = true;
			pthread_mutex_unlock(&p->lock);
		}
		if (r != U_DESKTOP_OK) {
			U_LOG_W("desktop/gnome: access request: %s", c->msg);
		}
	}
	pthread_mutex_lock(&p->lock);
	p->access = acc;
	p->pending = false;
	pthread_mutex_unlock(&p->lock);
	c->done(c->done_ctx, acc);
	return U_DESKTOP_OK;
}



/*
 * Thread
 */

void
gp_cmd_free(struct gp_cmd *c)
{
	free(c->text);
	free(c);
}

static void *
portal_thread(void *arg)
{
	struct gnome_portal *p = arg;
	DBusError e;
	dbus_error_init(&e);
	p->c = dbus_bus_get_private(DBUS_BUS_SESSION, &e);
	if (p->c == NULL) {
		U_LOG_W("desktop/gnome: portal thread: no session bus (%s)", dbus_error_is_set(&e) ? e.message : "?");
		dbus_error_free(&e);
	} else {
		dbus_connection_set_exit_on_disconnect(p->c, FALSE);
		const char *unique = dbus_bus_get_unique_name(p->c);
		size_t j = 0;
		for (const char *s = unique ? unique : ""; *s && j + 1 < sizeof(p->sender); s++) {
			if (*s == ':') {
				continue;
			}
			p->sender[j++] = *s == '.' ? '_' : *s;
		}
		p->sender[j] = '\0';
		static const char *const rules[] = {
		    "type='signal',sender='" PORTAL_DEST "',interface='" REQUEST_IFACE "',member='Response'",
		    "type='signal',sender='" PORTAL_DEST "',interface='" SESSION_IFACE "',member='Closed'",
		    "type='signal',sender='" PORTAL_DEST "',interface='" CLIP_IFACE "'",
		};
		for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
			dbus_bus_add_match(p->c, rules[i], &e);
			if (dbus_error_is_set(&e)) {
				U_LOG_W("desktop/gnome: AddMatch %s: %s", rules[i], e.message);
				dbus_error_free(&e);
			}
		}
	}

	pthread_mutex_lock(&p->lock);
	while (!p->quit) {
		struct gp_cmd *c = p->head;
		if (c != NULL) {
			p->head = c->next;
			if (p->head == NULL) {
				p->tail = NULL;
			}
			c->started = true;
			pthread_mutex_unlock(&p->lock);

			c->msg[0] = '\0';
			enum u_desktop_result r = c->fn(p, c);

			pthread_mutex_lock(&p->lock);
			c->result = r;
			c->finished = true;
			if (c->async || c->abandoned) {
				gp_cmd_free(c);
			} else {
				pthread_cond_broadcast(&p->cond);
			}
			continue;
		}
		pthread_mutex_unlock(&p->lock);

		// Close the session after a quiet spell (only when it can be restored).
		int timeout = 250;
		if (p->session_open) {
			pthread_mutex_lock(&p->lock);
			bool have_token = p->have_token;
			bool grabbing = p->cap.grabs > 0;
			uint64_t last_use = p->last_use_ms;
			pthread_mutex_unlock(&p->lock);
			uint64_t now = gdbus_now_ms();
			uint64_t idle = now > last_use ? now - last_use : 0;
			if (grabbing) {
				// A capture is reading the streams; its end wakes the thread.
				timeout = 250;
			} else if (now < p->hold_until_ms) {
				// prepare_input asked to keep it (verification / idle wait in progress).
				uint64_t left = p->hold_until_ms - now;
				timeout = left < 250 ? (int)left + 1 : 250;
			} else if (have_token && idle >= SESSION_IDLE_CLOSE_MS) {
				session_close(p);
			} else if (have_token) {
				uint64_t left = SESSION_IDLE_CLOSE_MS - idle;
				timeout = left < 250 ? (int)left + 1 : 250;
			}
		}
		gp_pump(p, timeout);
		pthread_mutex_lock(&p->lock);
	}
	// Drop what is queued: async requests still get their one callback.
	struct gp_cmd *c = p->head;
	p->head = p->tail = NULL;
	pthread_mutex_unlock(&p->lock);
	while (c != NULL) {
		struct gp_cmd *next = c->next;
		pthread_mutex_lock(&p->lock);
		if (c->async) {
			pthread_mutex_unlock(&p->lock);
			c->done(c->done_ctx, U_DESKTOP_ACCESS_UNKNOWN); // never answered
			gp_cmd_free(c);
		} else {
			c->result = U_DESKTOP_ERR_FAILED;
			snprintf(c->msg, sizeof(c->msg), "desktop control is shutting down");
			c->finished = true;
			if (c->abandoned) {
				gp_cmd_free(c);
			}
			pthread_cond_broadcast(&p->cond);
			pthread_mutex_unlock(&p->lock);
		}
		c = next;
	}

	session_close(p);
	if (p->c != NULL) {
		dbus_connection_close(p->c);
		dbus_connection_unref(p->c);
		p->c = NULL;
	}
	return NULL;
}

//! Under p->lock.
static bool
ensure_thread_locked(struct gnome_portal *p)
{
	if (p->thread_started) {
		return true;
	}
	if (pthread_create(&p->thread, NULL, portal_thread, p) != 0) {
		return false;
	}
	p->thread_started = true;
	return true;
}

static void
enqueue_locked(struct gnome_portal *p, struct gp_cmd *c)
{
	c->next = NULL;
	if (p->tail != NULL) {
		p->tail->next = c;
	} else {
		p->head = c;
	}
	p->tail = c;
}

//! Run @p c on the thread and wait at most @p timeout_ms. Takes ownership of @p c.
enum u_desktop_result
gp_run_sync(struct gnome_portal *p, struct gp_cmd *c, uint32_t timeout_ms, char *msg)
{
	return gp_run_sync_keep(p, c, timeout_ms, msg, NULL);
}

enum u_desktop_result
gp_run_sync_keep(struct gnome_portal *p, struct gp_cmd *c, uint32_t timeout_ms, char *msg, struct gp_cmd *out)
{
	pthread_mutex_lock(&p->lock);
	if (p->quit || !ensure_thread_locked(p)) {
		pthread_mutex_unlock(&p->lock);
		gp_cmd_free(c);
		gp_set_msg(msg, "desktop control is shutting down");
		return U_DESKTOP_ERR_FAILED;
	}
	enqueue_locked(p, c);
	wake_thread(p);

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t ns = (uint64_t)ts.tv_nsec + (uint64_t)timeout_ms * 1000000ull;
	ts.tv_sec += (time_t)(ns / 1000000000ull);
	ts.tv_nsec = (long)(ns % 1000000000ull);
	while (!c->finished) {
		if (pthread_cond_timedwait(&p->cond, &p->lock, &ts) == ETIMEDOUT) {
			break;
		}
	}
	enum u_desktop_result r;
	if (c->finished) {
		if (out != NULL) {
			*out = *c;
			out->text = NULL;
			out->next = NULL;
		}
		r = c->result;
		if (msg != NULL && c->msg[0] != '\0') {
			snprintf(msg, U_DESKTOP_MSG_SIZE, "%s", c->msg);
		}
		gp_cmd_free(c);
	} else if (!c->started) {
		// Still queued: unlink it.
		struct gp_cmd **pp = &p->head;
		struct gp_cmd *prev = NULL;
		while (*pp != NULL && *pp != c) {
			prev = *pp;
			pp = &(*pp)->next;
		}
		if (*pp == c) {
			*pp = c->next;
			if (p->tail == c) {
				p->tail = prev;
			}
		}
		gp_cmd_free(c);
		r = U_DESKTOP_ERR_TIMEOUT;
		gp_set_msg(msg, "the portal is busy (an access dialog may be open)");
	} else {
		// Running: every command is bounded (D-Bus calls <= CALL_TIMEOUT_MS, the
		// silent restore <= RESTORE_START_TIMEOUT_MS, the paste <= ~3 s), so wait
		// for it rather than leave input in flight after we returned.
		while (!c->finished) {
			pthread_cond_wait(&p->cond, &p->lock);
		}
		if (out != NULL) {
			*out = *c;
			out->text = NULL;
			out->next = NULL;
		}
		r = c->result;
		if (msg != NULL && c->msg[0] != '\0') {
			snprintf(msg, U_DESKTOP_MSG_SIZE, "%s", c->msg);
		}
		gp_cmd_free(c);
	}
	pthread_mutex_unlock(&p->lock);
	return r;
}

struct gp_cmd *
gp_cmd_new(gp_cmd_fn fn)
{
	struct gp_cmd *c = calloc(1, sizeof(*c));
	if (c != NULL) {
		c->fn = fn;
	}
	return c;
}

/*
 * Public
 */

struct gnome_portal *
gnome_portal_create(uint32_t rd_version, bool have_clipboard)
{
	if (rd_version == 0) {
		return NULL;
	}
	struct gnome_portal *p = calloc(1, sizeof(*p));
	if (p == NULL) {
		return NULL;
	}
	if (pipe(p->wake) != 0) {
		free(p);
		return NULL;
	}
	fcntl(p->wake[0], F_SETFL, O_NONBLOCK);
	fcntl(p->wake[1], F_SETFL, O_NONBLOCK);
	fcntl(p->wake[0], F_SETFD, FD_CLOEXEC);
	fcntl(p->wake[1], F_SETFD, FD_CLOEXEC);
	pthread_mutex_init(&p->lock, NULL);
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&p->cond, &ca);
	pthread_condattr_destroy(&ca);
	p->rd_version = rd_version;
	p->have_clipboard_portal = have_clipboard;
	p->input = rd_version >= 2 && gnome_portal_libei_available();
	p->access = U_DESKTOP_ACCESS_UNKNOWN;
	gp_cap_construct(p);
	return p;
}

void
gnome_portal_destroy(struct gnome_portal *p)
{
	if (p == NULL) {
		return;
	}
	pthread_mutex_lock(&p->lock);
	p->quit = true;
	bool started = p->thread_started;
	pthread_mutex_unlock(&p->lock);
	wake_thread(p);
	if (started) {
		pthread_join(p->thread, NULL);
	}
	close(p->wake[0]);
	close(p->wake[1]);
	pthread_cond_destroy(&p->cond);
	pthread_mutex_destroy(&p->lock);
	// Wipe the token from memory (its saved copy, if any, stays for the next launch).
	memset(p->token, 0, sizeof(p->token));
	memset(p->saved.token, 0, sizeof(p->saved.token));
	free(p);
}

bool
gnome_portal_has_input(struct gnome_portal *p)
{
	return p->input;
}

enum u_desktop_access
gnome_portal_access(struct gnome_portal *p)
{
	pthread_mutex_lock(&p->lock);
	enum u_desktop_access a = p->access;
	if (p->pending) {
		a = U_DESKTOP_ACCESS_PENDING;
	} else if (gp_cap_control_granted_locked(p) || p->consent_only_granted) {
		a = U_DESKTOP_ACCESS_GRANTED;
	}
	pthread_mutex_unlock(&p->lock);
	return a;
}

void
gnome_portal_request(struct gnome_portal *p, const char *parent_window, u_desktop_access_done_fn done, void *ctx)
{
	struct gp_cmd *c = gp_cmd_new(cmd_request);
	if (c == NULL) {
		done(ctx, U_DESKTOP_ACCESS_DENIED);
		return;
	}
	snprintf(c->parent_window, sizeof(c->parent_window), "%s", parent_window != NULL ? parent_window : "");
	c->async = true;
	c->done = done;
	c->done_ctx = ctx;
	pthread_mutex_lock(&p->lock);
	if (p->quit || !ensure_thread_locked(p)) {
		pthread_mutex_unlock(&p->lock);
		gp_cmd_free(c);
		done(ctx, U_DESKTOP_ACCESS_DENIED);
		return;
	}
	p->pending = true;
	enqueue_locked(p, c);
	pthread_mutex_unlock(&p->lock);
	wake_thread(p);
}


/*
 * Session plumbing for u_desktop_gnome_capture.c (u_desktop_gnome_portal_priv.h).
 */

enum u_desktop_result
gp_open_session(struct gnome_portal *p, bool interactive, char *msg)
{
	return open_session(p, interactive ? GP_OPEN_ASK : GP_OPEN_RESTORE, msg);
}

enum u_desktop_result
gp_open_session_mode(struct gnome_portal *p, enum gp_open mode, char *msg)
{
	return open_session(p, mode, msg);
}

void
gp_session_close(struct gnome_portal *p)
{
	session_close(p);
}

void
gp_register_app(struct gnome_portal *p)
{
	register_app_once(p);
}

void
gp_next_token(struct gnome_portal *p, char *out, size_t cap)
{
	next_token(p, out, cap);
}

void
gp_expect_response(struct gnome_portal *p, const char *token)
{
	expect_response(p, token);
}

bool
gp_wait_response(struct gnome_portal *p, uint32_t timeout_ms)
{
	return wait_response(p, timeout_ms);
}

void
gp_take_request_handle(struct gnome_portal *p, DBusMessage *reply)
{
	take_request_handle(p, reply);
}

void
gp_dict_add_str(DBusMessageIter *dict, const char *key, const char *val)
{
	dict_add_str(dict, key, val);
}

void
gp_dict_add_u32(DBusMessageIter *dict, const char *key, uint32_t val)
{
	dict_add_u32(dict, key, val);
}

void
gp_dict_add_bool(DBusMessageIter *dict, const char *key, bool val)
{
	dbus_bool_t b = val ? TRUE : FALSE;
	dict_add_basic(dict, key, DBUS_TYPE_BOOLEAN, "b", &b);
}

bool
gp_enqueue_async(struct gnome_portal *p, struct gp_cmd *c)
{
	c->async = true;
	pthread_mutex_lock(&p->lock);
	if (p->quit || !ensure_thread_locked(p)) {
		pthread_mutex_unlock(&p->lock);
		return false;
	}
	enqueue_locked(p, c);
	pthread_mutex_unlock(&p->lock);
	wake_thread(p);
	return true;
}
