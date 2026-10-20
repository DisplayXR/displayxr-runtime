// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: text at a point through AT-SPI (ADR-056).
 *
 * The accessibility bus is a separate D-Bus bus (org.a11y.Bus.GetAddress on the
 * session bus), opened privately here. Wayland screen coordinates mean nothing to
 * a client, so the lookup is window-relative: the extension's pick gives the
 * window (pid, title, frame); here we find the AT-SPI application of that pid
 * (the root's children, each matched by its bus name's unix process id), its
 * frame whose name is the window title (no match: "" — never another window's),
 * then Component.GetAccessibleAtPoint in WINDOW coordinates (coord_type 1,
 * relative to the frame), descending until a leaf. The leaf's Text (GetText 0,-1)
 * if it has one, else its Name.
 *
 * Apps only answer when their toolkit exposes accessibility: GTK4 always does,
 * GTK3 / Qt / Electron once the session's org.a11y.Status says it is enabled
 * (a screen reader, or `gsettings set org.gnome.desktop.interface
 * toolkit-accessibility true`). This backend does not flip that setting.
 */

#include "u_desktop_gnome.h"

#include "util/u_logging.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATSPI_REGISTRY "org.a11y.atspi.Registry"
#define ATSPI_ROOT "/org/a11y/atspi/accessible/root"
#define ATSPI_NULL "/org/a11y/atspi/null"
#define ATSPI_ACCESSIBLE "org.a11y.atspi.Accessible"
#define ATSPI_COMPONENT "org.a11y.atspi.Component"
#define ATSPI_TEXT "org.a11y.atspi.Text"
#define ATSPI_COORD_TYPE_WINDOW 1u
// AtspiStateType bits (GetState's "au": bit n of word n / 32).
#define ATSPI_STATE_FOCUSED 12u
#define ATSPI_STATE_SHOWING 25u
#define ATSPI_STATE_MANAGES_DESCENDANTS 31u
//! The focused-text walk visits at most this many accessibles.
#define FOCUS_WALK_MAX_NODES 400u
//! Hash this much text on each side of the caret.
#define FOCUS_HASH_SPAN 1024

#define APP_CACHE 32

struct atspi_ref
{
	char bus[128];
	char path[256];
};

struct gnome_atspi
{
	pthread_mutex_t lock;
	char addr[512];
	DBusConnection *c;
	uint64_t last_connect_try_ms;

	// pid -> application, refreshed on a miss.
	struct
	{
		uint32_t pid;
		struct atspi_ref app;
	} cache[APP_CACHE];
	uint32_t ncache;
};

struct budget
{
	uint64_t deadline;
};

static int
left_ms(const struct budget *b)
{
	uint64_t now = gdbus_now_ms();
	if (now >= b->deadline) {
		return 0;
	}
	uint64_t l = b->deadline - now;
	return l < 2 ? 2 : (int)l;
}

static bool
connect_locked(struct gnome_atspi *a)
{
	if (a->c != NULL && dbus_connection_get_is_connected(a->c)) {
		return true;
	}
	if (a->c != NULL) {
		dbus_connection_close(a->c);
		dbus_connection_unref(a->c);
		a->c = NULL;
		a->ncache = 0;
	}
	uint64_t now = gdbus_now_ms();
	if (a->last_connect_try_ms != 0 && now - a->last_connect_try_ms < 2000) {
		return false; // don't hammer a dead bus from a frame loop
	}
	a->last_connect_try_ms = now;
	DBusError e;
	dbus_error_init(&e);
	DBusConnection *c = dbus_connection_open_private(a->addr, &e);
	if (c == NULL) {
		U_LOG_W("desktop/gnome: cannot open the accessibility bus: %s",
		        dbus_error_is_set(&e) ? e.message : "?");
		dbus_error_free(&e);
		return false;
	}
	dbus_connection_set_exit_on_disconnect(c, FALSE);
	if (!dbus_bus_register(c, &e)) {
		U_LOG_W("desktop/gnome: accessibility bus Hello failed: %s", dbus_error_is_set(&e) ? e.message : "?");
		dbus_error_free(&e);
		dbus_connection_close(c);
		dbus_connection_unref(c);
		return false;
	}
	a->c = c;
	return true;
}

//! Read an a(so) reply into @p out (up to @p cap). Returns the count stored.
static uint32_t
read_refs(DBusMessage *reply, struct atspi_ref *out, uint32_t cap)
{
	DBusMessageIter it, arr;
	uint32_t n = 0;
	if (!dbus_message_iter_init(reply, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
		return 0;
	}
	dbus_message_iter_recurse(&it, &arr);
	while (n < cap && dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
		DBusMessageIter st;
		const char *bus = NULL, *path = NULL;
		dbus_message_iter_recurse(&arr, &st);
		if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&st, &bus);
			dbus_message_iter_next(&st);
			if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_OBJECT_PATH) {
				dbus_message_iter_get_basic(&st, &path);
			}
		}
		if (bus && path) {
			snprintf(out[n].bus, sizeof(out[n].bus), "%s", bus);
			snprintf(out[n].path, sizeof(out[n].path), "%s", path);
			n++;
		}
		dbus_message_iter_next(&arr);
	}
	return n;
}

//! A single (so) reply.
static bool
read_ref(DBusMessage *reply, struct atspi_ref *out)
{
	DBusMessageIter it, st;
	if (!dbus_message_iter_init(reply, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRUCT) {
		return false;
	}
	dbus_message_iter_recurse(&it, &st);
	const char *bus = NULL, *path = NULL;
	if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_STRING) {
		return false;
	}
	dbus_message_iter_get_basic(&st, &bus);
	dbus_message_iter_next(&st);
	if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_OBJECT_PATH) {
		return false;
	}
	dbus_message_iter_get_basic(&st, &path);
	snprintf(out->bus, sizeof(out->bus), "%s", bus);
	snprintf(out->path, sizeof(out->path), "%s", path);
	return true;
}

static uint32_t
children(struct gnome_atspi *a, const struct atspi_ref *of, struct atspi_ref *out, uint32_t cap, struct budget *b)
{
	DBusMessage *reply =
	    gdbus_call(a->c, of->bus, of->path, ATSPI_ACCESSIBLE, "GetChildren", left_ms(b), NULL, DBUS_TYPE_INVALID);
	if (reply == NULL) {
		return 0;
	}
	uint32_t n = read_refs(reply, out, cap);
	dbus_message_unref(reply);
	return n;
}

//! Accessible.Name (a property) into @p buf.
static bool
get_name(struct gnome_atspi *a, const struct atspi_ref *r, char *buf, size_t cap, struct budget *b)
{
	const char *iface = ATSPI_ACCESSIBLE, *prop = "Name";
	DBusMessage *reply = gdbus_call(a->c, r->bus, r->path, "org.freedesktop.DBus.Properties", "Get", left_ms(b),
	                                NULL, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
	if (reply == NULL) {
		return false;
	}
	bool ok = false;
	DBusMessageIter it, var;
	if (dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&it, &var);
		if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
			const char *s = NULL;
			dbus_message_iter_get_basic(&var, &s);
			gdbus_copy_utf8(buf, cap, s ? s : "");
			ok = true;
		}
	}
	dbus_message_unref(reply);
	return ok;
}


static bool
has_interface(struct gnome_atspi *a, const struct atspi_ref *r, const char *iface, struct budget *b)
{
	DBusMessage *reply =
	    gdbus_call(a->c, r->bus, r->path, ATSPI_ACCESSIBLE, "GetInterfaces", left_ms(b), NULL, DBUS_TYPE_INVALID);
	if (reply == NULL) {
		return false;
	}
	bool found = false;
	DBusMessageIter it, arr;
	if (dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(&it, &arr);
		while (!found && dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
			const char *s = NULL;
			dbus_message_iter_get_basic(&arr, &s);
			found = s && strcmp(s, iface) == 0;
			dbus_message_iter_next(&arr);
		}
	}
	dbus_message_unref(reply);
	return found;
}

static bool
pid_of_bus(struct gnome_atspi *a, const char *bus, uint32_t *pid, struct budget *b)
{
	DBusMessage *reply =
	    gdbus_call(a->c, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionUnixProcessID",
	               left_ms(b), NULL, DBUS_TYPE_STRING, &bus, DBUS_TYPE_INVALID);
	if (reply == NULL) {
		return false;
	}
	dbus_uint32_t v = 0;
	bool ok = dbus_message_get_args(reply, NULL, DBUS_TYPE_UINT32, &v, DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	*pid = v;
	return ok;
}

//! The AT-SPI application object of process @p pid.
static bool
find_app(struct gnome_atspi *a, uint32_t pid, struct atspi_ref *out, struct budget *b)
{
	for (uint32_t i = 0; i < a->ncache; i++) {
		if (a->cache[i].pid == pid) {
			*out = a->cache[i].app;
			return true;
		}
	}
	struct atspi_ref *apps = calloc(128, sizeof(*apps)); // per call: nothing shared across instances
	if (apps == NULL) {
		return false;
	}
	struct atspi_ref root = {ATSPI_REGISTRY, ATSPI_ROOT};
	uint32_t n = children(a, &root, apps, 128, b);
	bool found = false;
	a->ncache = 0;
	for (uint32_t i = 0; i < n && left_ms(b) > 2; i++) {
		uint32_t app_pid = 0;
		if (!pid_of_bus(a, apps[i].bus, &app_pid, b)) {
			continue;
		}
		if (a->ncache < APP_CACHE) {
			a->cache[a->ncache].pid = app_pid;
			a->cache[a->ncache].app = apps[i];
			a->ncache++;
		}
		if (app_pid == pid && !found) {
			*out = apps[i];
			found = true;
		}
	}
	free(apps);
	return found;
}

static void
forget_app(struct gnome_atspi *a, uint32_t pid)
{
	for (uint32_t i = 0; i < a->ncache; i++) {
		if (a->cache[i].pid == pid) {
			a->cache[i] = a->cache[--a->ncache];
			return;
		}
	}
}

struct gnome_atspi *
gnome_atspi_create(DBusConnection *session_bus)
{
	DBusMessage *reply = gdbus_call(session_bus, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress", 500,
	                                NULL, DBUS_TYPE_INVALID);
	const char *addr = NULL;
	if (reply == NULL || !dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &addr, DBUS_TYPE_INVALID) ||
	    addr == NULL || addr[0] == '\0') {
		if (reply != NULL) {
			dbus_message_unref(reply);
		}
		return NULL;
	}
	struct gnome_atspi *a = calloc(1, sizeof(*a));
	if (a != NULL) {
		snprintf(a->addr, sizeof(a->addr), "%s", addr);
		pthread_mutex_init(&a->lock, NULL);
	}
	dbus_message_unref(reply);
	return a;
}

void
gnome_atspi_destroy(struct gnome_atspi *a)
{
	if (a == NULL) {
		return;
	}
	if (a->c != NULL) {
		dbus_connection_close(a->c);
		dbus_connection_unref(a->c);
	}
	pthread_mutex_destroy(&a->lock);
	free(a);
}

enum u_desktop_result
gnome_atspi_text_at(struct gnome_atspi *a,
                    uint32_t pid,
                    const char *title,
                    int32_t wx,
                    int32_t wy,
                    uint32_t budget_ms,
                    char *buf,
                    size_t cap,
                    char *msg)
{
	buf[0] = '\0';
	struct budget b = {gdbus_now_ms() + budget_ms};
	pthread_mutex_lock(&a->lock);
	if (!connect_locked(a)) {
		pthread_mutex_unlock(&a->lock);
		if (msg) {
			snprintf(msg, U_DESKTOP_MSG_SIZE, "the accessibility bus is unreachable");
		}
		return U_DESKTOP_ERR_FAILED;
	}
	gdbus_drain(a->c);

	enum u_desktop_result r = U_DESKTOP_OK;
	struct atspi_ref app, cur;
	struct atspi_ref *frames = NULL;
	if (!find_app(a, pid, &app, &b)) {
		// Not an accessible app (toolkit a11y off) — nothing to read.
		if (msg) {
			snprintf(msg, U_DESKTOP_MSG_SIZE, "the app (pid %u) exposes no accessibility tree", pid);
		}
		goto out;
	}

	// Its frame for THIS window, by title. No match → nothing: falling back to the
	// active / first frame could read another window's text.
	frames = calloc(64, sizeof(*frames)); // per call
	if (frames == NULL) {
		r = U_DESKTOP_ERR_FAILED;
		goto out;
	}
	uint32_t nf = children(a, &app, frames, 64, &b);
	if (nf == 0) {
		forget_app(a, pid); // stale cache entry, or an app with no windows
		goto out;
	}
	int pick = -1;
	for (uint32_t i = 0; i < nf && pick < 0 && left_ms(&b) > 2; i++) {
		char name[U_DESKTOP_TITLE_SIZE];
		if (title && title[0] && get_name(a, &frames[i], name, sizeof(name), &b) && strcmp(name, title) == 0) {
			pick = (int)i;
		}
	}
	if (pick < 0) {
		if (msg) {
			snprintf(msg, U_DESKTOP_MSG_SIZE, "no accessible frame matches the window");
		}
		goto out;
	}
	cur = frames[pick];

	// Descend.
	for (int depth = 0; depth < 32 && left_ms(&b) > 2; depth++) {
		dbus_int32_t x = wx, y = wy;
		dbus_uint32_t ct = ATSPI_COORD_TYPE_WINDOW;
		DBusMessage *reply =
		    gdbus_call(a->c, cur.bus, cur.path, ATSPI_COMPONENT, "GetAccessibleAtPoint", left_ms(&b), NULL,
		               DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_UINT32, &ct, DBUS_TYPE_INVALID);
		if (reply == NULL) {
			break;
		}
		struct atspi_ref child;
		bool ok = read_ref(reply, &child);
		dbus_message_unref(reply);
		if (!ok || strcmp(child.path, ATSPI_NULL) == 0 ||
		    (strcmp(child.path, cur.path) == 0 && strcmp(child.bus, cur.bus) == 0)) {
			break;
		}
		cur = child;
	}

	// Its text, else its name.
	if (left_ms(&b) > 2 && has_interface(a, &cur, ATSPI_TEXT, &b)) {
		dbus_int32_t start = 0, end = -1;
		DBusMessage *reply = gdbus_call(a->c, cur.bus, cur.path, ATSPI_TEXT, "GetText", left_ms(&b), NULL,
		                                DBUS_TYPE_INT32, &start, DBUS_TYPE_INT32, &end, DBUS_TYPE_INVALID);
		const char *s = NULL;
		if (reply != NULL && dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID) && s) {
			gdbus_copy_utf8(buf, cap, s);
		}
		if (reply != NULL) {
			dbus_message_unref(reply);
		}
	}
	if (buf[0] == '\0' && left_ms(&b) > 2) {
		get_name(a, &cur, buf, cap, &b);
	}
	if (buf[0] == '\0' && left_ms(&b) <= 2) {
		r = U_DESKTOP_ERR_TIMEOUT;
		if (msg) {
			snprintf(msg, U_DESKTOP_MSG_SIZE, "the accessibility tree did not answer within %u ms",
			         budget_ms);
		}
	}
out:
	free(frames);
	pthread_mutex_unlock(&a->lock);
	return r;
}

//! Accessible.GetState as a 64-bit mask; false on no answer.
static bool
get_state(struct gnome_atspi *a, const struct atspi_ref *r, uint64_t *mask, struct budget *b)
{
	DBusMessage *reply =
	    gdbus_call(a->c, r->bus, r->path, ATSPI_ACCESSIBLE, "GetState", left_ms(b), NULL, DBUS_TYPE_INVALID);
	if (reply == NULL) {
		return false;
	}
	*mask = 0;
	DBusMessageIter it, arr;
	if (dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(&it, &arr);
		for (int w = 0; w < 2 && dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_UINT32; w++) {
			dbus_uint32_t v = 0;
			dbus_message_iter_get_basic(&arr, &v);
			*mask |= (uint64_t)v << (32 * w);
			dbus_message_iter_next(&arr);
		}
	}
	dbus_message_unref(reply);
	return true;
}

//! An int32 property of org.a11y.atspi.Text.
static bool
get_text_prop(struct gnome_atspi *a, const struct atspi_ref *r, const char *prop, int32_t *out, struct budget *b)
{
	const char *iface = ATSPI_TEXT;
	DBusMessage *reply = gdbus_call(a->c, r->bus, r->path, "org.freedesktop.DBus.Properties", "Get", left_ms(b),
	                                NULL, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
	if (reply == NULL) {
		return false;
	}
	bool ok = false;
	DBusMessageIter it, var;
	if (dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&it, &var);
		if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_INT32) {
			dbus_int32_t v = 0;
			dbus_message_iter_get_basic(&var, &v);
			*out = v;
			ok = true;
		}
	}
	dbus_message_unref(reply);
	return ok;
}

void
gnome_atspi_focused_text(
    struct gnome_atspi *a, uint32_t pid, const char *title, uint32_t budget_ms, struct u_desktop_text_state *out)
{
	memset(out, 0, sizeof(*out));
	if (a == NULL || title == NULL || title[0] == '\0') {
		return;
	}
	struct budget b = {gdbus_now_ms() + budget_ms};
	pthread_mutex_lock(&a->lock);
	struct atspi_ref app;
	struct atspi_ref *q = NULL;
	if (!connect_locked(a)) {
		goto out;
	}
	gdbus_drain(a->c);
	if (!find_app(a, pid, &app, &b)) {
		goto out;
	}
	q = calloc(FOCUS_WALK_MAX_NODES + 64, sizeof(*q)); // per call
	if (q == NULL) {
		goto out;
	}
	// The window's frame, by title (as text_at: never another window's).
	uint32_t nf = children(a, &app, q, 64, &b);
	int frame = -1;
	for (uint32_t i = 0; i < nf && frame < 0 && left_ms(&b) > 2; i++) {
		char name[U_DESKTOP_TITLE_SIZE];
		if (get_name(a, &q[i], name, sizeof(name), &b) && strcmp(name, title) == 0) {
			frame = (int)i;
		}
	}
	if (frame < 0) {
		goto out;
	}
	q[0] = q[frame];
	// Breadth-first over the showing subtree until the FOCUSED accessible.
	uint32_t head = 0, tail = 1;
	while (head < tail && head < FOCUS_WALK_MAX_NODES && left_ms(&b) > 2) {
		struct atspi_ref cur = q[head++];
		uint64_t st = 0;
		if (!get_state(a, &cur, &st, &b)) {
			continue;
		}
		if (st & (1ull << ATSPI_STATE_FOCUSED)) {
			// The focus. Text or not, the walk ends here.
			if (!has_interface(a, &cur, ATSPI_TEXT, &b)) {
				goto out;
			}
			int32_t count = 0, caret = 0;
			if (!get_text_prop(a, &cur, "CharacterCount", &count, &b) ||
			    !get_text_prop(a, &cur, "CaretOffset", &caret, &b)) {
				goto out;
			}
			dbus_int32_t s0 = caret > FOCUS_HASH_SPAN ? caret - FOCUS_HASH_SPAN : 0;
			dbus_int32_t s1 = caret + FOCUS_HASH_SPAN;
			DBusMessage *reply =
			    gdbus_call(a->c, cur.bus, cur.path, ATSPI_TEXT, "GetText", left_ms(&b), NULL,
			               DBUS_TYPE_INT32, &s0, DBUS_TYPE_INT32, &s1, DBUS_TYPE_INVALID);
			const char *txt = NULL;
			uint64_t h = 0;
			if (reply != NULL &&
			    dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &txt, DBUS_TYPE_INVALID) &&
			    txt != NULL) {
				h = u_desktop_fnv1a(txt, strlen(txt));
			}
			if (reply != NULL) {
				dbus_message_unref(reply);
			}
			out->found = true;
			out->count = count;
			out->caret = caret;
			out->hash = h;
			goto out;
		}
		// A list that manages its descendants can hold thousands of rows: not where
		// a text field's focus lives.
		if (!(st & (1ull << ATSPI_STATE_SHOWING)) || (st & (1ull << ATSPI_STATE_MANAGES_DESCENDANTS))) {
			continue;
		}
		uint32_t room = FOCUS_WALK_MAX_NODES + 64 - tail;
		if (room > 64) {
			room = 64;
		}
		if (room == 0) {
			continue;
		}
		tail += children(a, &cur, &q[tail], room, &b);
	}
out:
	free(q);
	pthread_mutex_unlock(&a->lock);
}

int
gnome_atspi_app_responds(struct gnome_atspi *a, uint32_t pid, uint32_t budget_ms)
{
	struct budget b = {gdbus_now_ms() + budget_ms};
	pthread_mutex_lock(&a->lock);
	if (!connect_locked(a)) {
		pthread_mutex_unlock(&a->lock);
		return -1;
	}
	gdbus_drain(a->c);
	int r = -1;
	struct atspi_ref app;
	// find_app talks to the registry and the bus daemon only, never to the app.
	if (find_app(a, pid, &app, &b) && left_ms(&b) > 2) {
		// A property read on the app object is answered by the app's own main loop
		// (GTK dispatches its AT-SPI objects there): an answer — even an error — is
		// a live app; no answer within the budget is a busy or hung one.
		const char *iface = ATSPI_ACCESSIBLE, *prop = "Name";
		struct gdbus_err err;
		DBusMessage *reply =
		    gdbus_call(a->c, app.bus, app.path, "org.freedesktop.DBus.Properties", "Get", left_ms(&b), &err,
		               DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
		if (reply != NULL) {
			dbus_message_unref(reply);
			r = 1;
		} else if (strcmp(err.name, DBUS_ERROR_NO_REPLY) == 0 || strcmp(err.name, DBUS_ERROR_TIMEOUT) == 0) {
			r = 0;
		} else if (strcmp(err.name, DBUS_ERROR_SERVICE_UNKNOWN) == 0 ||
		           strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER) == 0) {
			forget_app(a, pid); // a stale cache entry: cannot tell
		} else if (err.name[0] != '\0') {
			r = 1; // the app itself answered, with an error
		}
	}
	pthread_mutex_unlock(&a->lock);
	return r;
}
