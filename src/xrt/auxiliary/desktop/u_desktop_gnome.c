// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-control backend: GNOME Wayland (XR_DXR_desktop_control, ADR-056).
 *
 * Productises a prototype tested on GNOME 50.1 (Ubuntu 26.04). Three parts:
 *
 * - **Observation** — the DisplayXR GNOME extension (contrib/gnome-shell/
 *   window-geometry@displayxr.org, version 16): GetAllWindows (listed
 *   bottom-to-top: global.get_window_actors() order, so reversed here),
 *   GetPointer, GetWindowAt; org.gnome.ScreenSaver.GetActive;
 *   org.gnome.Mutter.IdleMonitor GetIdletime. One private session-bus
 *   connection behind a mutex, never shared with the action path, calls bounded
 *   to a few hundred ms. Stage (logical) px throughout.
 * - **Consent + input** — u_desktop_gnome_portal.c (RemoteDesktop portal + libei
 *   on their own thread and connection).
 * - **Text at a point** — u_desktop_gnome_atspi.c.
 *
 * ## Other apps' windows
 *
 * The extension never mutates another app's window (ADR-056 D5), so window ops on
 * them are done as the user would, through the app's own portal session:
 * ACTIVATE = Super+click on a visible part of the window (a point GetWindowAt
 * confirms picks it), then wait for mutter's short grab to end (keys sent during
 * it are lost) and confirm focus; MINIMIZE = activate + Super+H; MAXIMIZE =
 * activate + Super+Up; RESTORE = activate + Super+Down (a minimized window of
 * another app cannot be reached this way: UNSUPPORTED); CLOSE = activate + Alt+F4;
 * MOVE = Super+drag from a visible point, jumping the pointer (a slow drag over a
 * monitor edge pops Ubuntu's tiling assistant, which grabs the keyboard — Escape
 * if one is up), read back and corrected once. Resizing them is UNSUPPORTED.
 * A Super gesture can leave the overview open; it is closed with Escape.
 * The pointer is put back where it was after a gesture.
 *
 * ## The app's own window
 *
 * WindowPlacement1 (extension v15): MoveWindow, ActivateWindow, SetMinimized —
 * no portal. Maximizing it is UNSUPPORTED (it would fight DisplayXR's placement).
 * Extension v17 gives ActivateWindow focus only when the app had the user's
 * latest input; when it refuses, the agent falls back to a Super+click through
 * the portal, as for any other window (own_activate).
 */

#include "u_desktop_gnome.h"

#include "util/u_json.h"
#include "util/u_logging.h"

#include <dbus/dbus.h>
#include <linux/input-event-codes.h>

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define WLG_PATH "/org/displayxr/WindowGeometry"
#define WLG_IFACE "org.displayxr.WindowGeometry1"
#define WLG_SCHEMA_MAX 1

#define PORTAL_DEST "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"

#define OBS_TIMEOUT_MS GNOME_OBS_TIMEOUT_MS
#define TEXT_AT_BUDGET_MS 45

/*
 * D-Bus helpers (shared with the other two files)
 */

static void
err_copy(struct gdbus_err *err, DBusError *e)
{
	if (err == NULL) {
		return;
	}
	err->name[0] = err->message[0] = '\0';
	if (dbus_error_is_set(e)) {
		snprintf(err->name, sizeof(err->name), "%s", e->name ? e->name : "");
		snprintf(err->message, sizeof(err->message), "%s", e->message ? e->message : "");
	}
}

DBusMessage *
gdbus_call(DBusConnection *c,
           const char *dest,
           const char *path,
           const char *iface,
           const char *method,
           int timeout_ms,
           struct gdbus_err *err,
           int first_arg_type,
           ...)
{
	DBusError e;
	dbus_error_init(&e);
	DBusMessage *m = dbus_message_new_method_call(dest, path, iface, method);
	if (m == NULL) {
		if (err) {
			snprintf(err->name, sizeof(err->name), "%s", DBUS_ERROR_NO_MEMORY);
			err->message[0] = '\0';
		}
		return NULL;
	}
	if (first_arg_type != DBUS_TYPE_INVALID) {
		va_list ap;
		va_start(ap, first_arg_type);
		bool ok = dbus_message_append_args_valist(m, first_arg_type, ap);
		va_end(ap);
		if (!ok) {
			dbus_message_unref(m);
			if (err) {
				snprintf(err->name, sizeof(err->name), "%s", DBUS_ERROR_INVALID_ARGS);
				err->message[0] = '\0';
			}
			return NULL;
		}
	}
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(c, m, timeout_ms > 0 ? timeout_ms : 1, &e);
	dbus_message_unref(m);
	err_copy(err, &e);
	dbus_error_free(&e);
	return reply;
}

static DBusMessage *
get_prop(DBusConnection *c,
         const char *dest,
         const char *path,
         const char *iface,
         const char *prop,
         int timeout_ms,
         DBusMessageIter *var)
{
	DBusMessage *reply = gdbus_call(c, dest, path, "org.freedesktop.DBus.Properties", "Get", timeout_ms, NULL,
	                                DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
	DBusMessageIter it;
	if (reply == NULL) {
		return NULL;
	}
	if (!dbus_message_iter_init(reply, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT) {
		dbus_message_unref(reply);
		return NULL;
	}
	dbus_message_iter_recurse(&it, var);
	return reply;
}

bool
gdbus_get_prop_u32(DBusConnection *c,
                   const char *dest,
                   const char *path,
                   const char *iface,
                   const char *prop,
                   int timeout_ms,
                   uint32_t *out)
{
	DBusMessageIter var;
	DBusMessage *reply = get_prop(c, dest, path, iface, prop, timeout_ms, &var);
	if (reply == NULL) {
		return false;
	}
	bool ok = dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_UINT32;
	if (ok) {
		dbus_uint32_t v = 0;
		dbus_message_iter_get_basic(&var, &v);
		*out = v;
	}
	dbus_message_unref(reply);
	return ok;
}

bool
gdbus_get_prop_bool(DBusConnection *c,
                    const char *dest,
                    const char *path,
                    const char *iface,
                    const char *prop,
                    int timeout_ms,
                    bool *out)
{
	DBusMessageIter var;
	DBusMessage *reply = get_prop(c, dest, path, iface, prop, timeout_ms, &var);
	if (reply == NULL) {
		return false;
	}
	bool ok = dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BOOLEAN;
	if (ok) {
		dbus_bool_t v = FALSE;
		dbus_message_iter_get_basic(&var, &v);
		*out = v;
	}
	dbus_message_unref(reply);
	return ok;
}

void
gdbus_drain(DBusConnection *c)
{
	dbus_connection_read_write(c, 0);
	DBusMessage *m;
	while ((m = dbus_connection_pop_message(c)) != NULL) {
		dbus_message_unref(m);
	}
}

void
gdbus_copy_utf8(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	size_t len = strlen(src);
	if (len >= cap) {
		len = cap - 1;
		// Back off continuation bytes so the cut lands on a code-point boundary.
		while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
			len--;
		}
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

uint64_t
gdbus_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void
gdbus_sleep_ms(uint32_t ms)
{
	struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
	while (nanosleep(&ts, &ts) != 0) {
	}
}

/*
 * The backend
 */

void
gnome_set_msg(char *msg, const char *fmt, ...)
{
	if (msg == NULL) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, U_DESKTOP_MSG_SIZE, fmt, ap);
	va_end(ap);
}

//! One extension call returning a JSON string, parsed. NULL on any failure (@p msg filled).
static cJSON *
ext_json(struct gnome_backend *g, const char *method, bool with_xy, int32_t x, int32_t y, char *msg)
{
	dbus_int32_t ix = x, iy = y;
	struct gdbus_err err;
	pthread_mutex_lock(&g->obs_lock);
	DBusMessage *reply =
	    with_xy ? gdbus_call(g->obs, WLG_BUS, WLG_PATH, WLG_IFACE, method, OBS_TIMEOUT_MS, &err, DBUS_TYPE_INT32,
	                         &ix, DBUS_TYPE_INT32, &iy, DBUS_TYPE_INVALID)
	            : gdbus_call(g->obs, WLG_BUS, WLG_PATH, WLG_IFACE, method, OBS_TIMEOUT_MS, &err, DBUS_TYPE_INVALID);
	gdbus_drain(g->obs);
	pthread_mutex_unlock(&g->obs_lock);
	const char *json = NULL;
	if (reply == NULL || !dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID)) {
		gnome_set_msg(msg, "the DisplayXR GNOME extension did not answer %s (%s)", method,
		              err.name[0] ? err.name : "bad reply");
		if (reply != NULL) {
			dbus_message_unref(reply);
		}
		return NULL;
	}
	cJSON *root = cJSON_Parse(json);
	dbus_message_unref(reply);
	if (root == NULL) {
		gnome_set_msg(msg, "the GNOME extension's %s reply is not JSON", method);
		return NULL;
	}
	int version = 1;
	u_json_get_int(u_json_get(root, "version"), &version);
	if (version > WLG_SCHEMA_MAX) {
		if (!g->warned_schema) {
			g->warned_schema = true;
			U_LOG_W("desktop/gnome: the GNOME extension speaks schema version %d (this runtime knows %d)",
			        version, WLG_SCHEMA_MAX);
		}
		gnome_set_msg(msg, "the GNOME extension's schema version %d is newer than this runtime", version);
		cJSON_Delete(root);
		return NULL;
	}
	return root;
}

static uint64_t
json_u64(const cJSON *j)
{
	if (j == NULL || !cJSON_IsNumber(j) || j->valuedouble < 0) {
		return 0;
	}
	return (uint64_t)j->valuedouble; // ids are < 2^53: exact in a double
}

static bool
json_bool(const cJSON *j)
{
	return j != NULL && cJSON_IsTrue(j);
}

//! The desktop itself (a DESKTOP window, or Ubuntu's desktop-icons "ding" window).
static bool
is_desktop_entry(const cJSON *w)
{
	const cJSON *app = u_json_get(w, "app_id");
	const char *a = cJSON_IsString(app) ? app->valuestring : "";
	return strcmp(a, "com.rastersoft.ding") == 0 || strcmp(a, "ding") == 0;
}

static void
parse_window(const cJSON *w, struct u_desktop_window *out)
{
	memset(out, 0, sizeof(*out));
	out->id = json_u64(u_json_get(w, "id"));
	out->parent_id = json_u64(u_json_get(w, "parent_id"));
	out->pid = (uint32_t)json_u64(u_json_get(w, "pid"));
	const cJSON *app = u_json_get(w, "app_id");
	gdbus_copy_utf8(out->app_id, sizeof(out->app_id), cJSON_IsString(app) ? app->valuestring : "");
	const cJSON *title = u_json_get(w, "title");
	gdbus_copy_utf8(out->title, sizeof(out->title), cJSON_IsString(title) ? title->valuestring : "");
	int f[4] = {0, 0, 0, 0};
	u_json_get_int_array(u_json_get(w, "frame"), f, 4);
	out->frame = (struct u_desktop_rect){f[0], f[1], f[2], f[3]};
	if (json_bool(u_json_get(w, "focus"))) {
		out->state |= U_DESKTOP_WINDOW_FOCUSED;
	}
	if (json_bool(u_json_get(w, "minimized"))) {
		out->state |= U_DESKTOP_WINDOW_MINIMIZED;
	}
	if (json_bool(u_json_get(w, "maximized"))) {
		out->state |= U_DESKTOP_WINDOW_MAXIMIZED;
	}
	if (json_bool(u_json_get(w, "fullscreen"))) {
		out->state |= U_DESKTOP_WINDOW_FULLSCREEN;
	}
	if (out->pid != 0 && out->pid == (uint32_t)getpid()) {
		out->state |= U_DESKTOP_WINDOW_OWN;
	}
}

/*!
 * Every window, top-most first (malloc'd, caller frees). The extension lists
 * them bottom-to-top (mutter's window-actor order), so this reverses.
 */
enum u_desktop_result
gnome_snapshot(struct gnome_backend *g, struct u_desktop_window **out, uint32_t *n, char *msg)
{
	*out = NULL;
	*n = 0;
	if (!g->have_ext16) {
		gnome_set_msg(msg, "needs the DisplayXR GNOME extension version 16");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	cJSON *root = ext_json(g, "GetAllWindows", false, 0, 0, msg);
	if (root == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	const cJSON *arr = u_json_get(root, "windows");
	int total = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
	struct u_desktop_window *ws = calloc(total > 0 ? (size_t)total : 1, sizeof(*ws));
	if (ws == NULL) {
		cJSON_Delete(root);
		return U_DESKTOP_ERR_FAILED;
	}
	uint32_t k = 0;
	for (int i = total - 1; i >= 0; i--) {
		const cJSON *w = cJSON_GetArrayItem(arr, i);
		if (w == NULL || is_desktop_entry(w)) {
			continue;
		}
		parse_window(w, &ws[k]);
		if (ws[k].id == 0) {
			continue;
		}
		ws[k].z = k;
		k++;
	}
	cJSON_Delete(root);
	*out = ws;
	*n = k;
	return U_DESKTOP_OK;
}

const struct u_desktop_window *
gnome_find_window(const struct u_desktop_window *ws, uint32_t n, uint64_t id)
{
	for (uint32_t i = 0; i < n; i++) {
		if (ws[i].id == id) {
			return &ws[i];
		}
	}
	return NULL;
}

/*
 * Observation
 */

static bool
gnome_is_locked(struct u_desktop_backend *b)
{
	struct gnome_backend *g = gnome(b);
	pthread_mutex_lock(&g->obs_lock);
	DBusMessage *reply = gdbus_call(g->obs, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver",
	                                "org.gnome.ScreenSaver", "GetActive", OBS_TIMEOUT_MS, NULL, DBUS_TYPE_INVALID);
	pthread_mutex_unlock(&g->obs_lock);
	// Fail CLOSED: no answer (a timeout, the shell restarting) reads as locked, so
	// no action proceeds on a guess.
	dbus_bool_t active = TRUE;
	if (reply != NULL) {
		if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &active, DBUS_TYPE_INVALID)) {
			active = TRUE;
		}
		dbus_message_unref(reply);
	}
	return active;
}

static bool
gnome_user_idle_ms(struct u_desktop_backend *b, uint64_t *out_ms)
{
	struct gnome_backend *g = gnome(b);
	pthread_mutex_lock(&g->obs_lock);
	DBusMessage *reply =
	    gdbus_call(g->obs, "org.gnome.Mutter.IdleMonitor", "/org/gnome/Mutter/IdleMonitor/Core",
	               "org.gnome.Mutter.IdleMonitor", "GetIdletime", OBS_TIMEOUT_MS, NULL, DBUS_TYPE_INVALID);
	pthread_mutex_unlock(&g->obs_lock);
	dbus_uint64_t ms = 0;
	bool ok = reply != NULL && dbus_message_get_args(reply, NULL, DBUS_TYPE_UINT64, &ms, DBUS_TYPE_INVALID);
	if (reply != NULL) {
		dbus_message_unref(reply);
	}
	if (ok) {
		*out_ms = ms;
	}
	return ok;
}

static enum u_desktop_result
gnome_list_windows(struct u_desktop_backend *b, struct u_desktop_window *out, uint32_t cap, uint32_t *count)
{
	struct u_desktop_window *ws;
	uint32_t n;
	enum u_desktop_result r = gnome_snapshot(gnome(b), &ws, &n, NULL);
	if (r != U_DESKTOP_OK) {
		*count = 0;
		return r;
	}
	if (out != NULL) {
		memcpy(out, ws, (n < cap ? n : cap) * sizeof(*ws));
	}
	*count = n;
	free(ws);
	return U_DESKTOP_OK;
}

enum u_desktop_result
gnome_get_pointer_msg(struct gnome_backend *g, int32_t *x, int32_t *y, char *msg)
{
	if (!g->have_ext16) {
		gnome_set_msg(msg, "needs the DisplayXR GNOME extension version 16");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	cJSON *root = ext_json(g, "GetPointer", false, 0, 0, msg);
	if (root == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	enum u_desktop_result r = U_DESKTOP_OK;
	int ix = 0, iy = 0;
	if (json_bool(u_json_get(root, "locked"))) {
		gnome_set_msg(msg, "the screen is locked");
		r = U_DESKTOP_ERR_REFUSED;
	} else if (!u_json_get_int(u_json_get(root, "x"), &ix) || !u_json_get_int(u_json_get(root, "y"), &iy)) {
		gnome_set_msg(msg, "GetPointer reply without x/y");
		r = U_DESKTOP_ERR_FAILED;
	}
	*x = ix;
	*y = iy;
	cJSON_Delete(root);
	return r;
}

static enum u_desktop_result
gnome_get_pointer(struct u_desktop_backend *b, int32_t *x, int32_t *y)
{
	return gnome_get_pointer_msg(gnome(b), x, y, NULL);
}

enum u_desktop_result
gnome_pick_msg(struct gnome_backend *g, int32_t x, int32_t y, struct u_desktop_pick *out, char *msg)
{
	memset(out, 0, sizeof(*out));
	out->x = x;
	out->y = y;
	out->kind = U_DESKTOP_PICK_OUTSIDE;
	if (!g->have_ext16) {
		gnome_set_msg(msg, "needs the DisplayXR GNOME extension version 16");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	cJSON *root = ext_json(g, "GetWindowAt", true, x, y, msg);
	if (root == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	if (json_bool(u_json_get(root, "locked"))) {
		cJSON_Delete(root);
		gnome_set_msg(msg, "the screen is locked");
		return U_DESKTOP_ERR_REFUSED;
	}
	const cJSON *over = u_json_get(root, "over");
	const char *o = cJSON_IsString(over) ? over->valuestring : "outside";
	const cJSON *w = u_json_get(root, "window");
	if (strcmp(o, "window") == 0 && cJSON_IsObject(w)) {
		if (is_desktop_entry(w)) {
			out->kind = U_DESKTOP_PICK_DESKTOP;
		} else {
			out->kind = U_DESKTOP_PICK_WINDOW;
			parse_window(w, &out->window);
		}
	} else if (strcmp(o, "desktop") == 0) {
		out->kind = U_DESKTOP_PICK_DESKTOP;
	} else if (strcmp(o, "shell") == 0) {
		out->kind = U_DESKTOP_PICK_SHELL;
	} else {
		out->kind = U_DESKTOP_PICK_OUTSIDE;
	}
	cJSON_Delete(root);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
gnome_pick(struct u_desktop_backend *b, int32_t x, int32_t y, struct u_desktop_pick *out)
{
	return gnome_pick_msg(gnome(b), x, y, out, NULL);
}

enum u_desktop_result
gnome_focused_window(struct u_desktop_backend *b, uint64_t *out_id)
{
	*out_id = 0;
	struct u_desktop_window *ws;
	uint32_t n;
	enum u_desktop_result r = gnome_snapshot(gnome(b), &ws, &n, NULL);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (ws[i].state & U_DESKTOP_WINDOW_FOCUSED) {
			*out_id = ws[i].id;
			break;
		}
	}
	free(ws);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
gnome_text_at(struct u_desktop_backend *b, int32_t x, int32_t y, char *buf, size_t cap, char *msg)
{
	struct gnome_backend *g = gnome(b);
	if (cap > 0) {
		buf[0] = '\0';
	}
	if (g->atspi == NULL) {
		gnome_set_msg(msg, "no accessibility bus in this session");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	uint64_t start = gdbus_now_ms();
	struct u_desktop_pick p;
	enum u_desktop_result r = gnome_pick_msg(g, x, y, &p, msg);
	if (r != U_DESKTOP_OK || p.kind != U_DESKTOP_PICK_WINDOW) {
		return r;
	}
	uint64_t spent = gdbus_now_ms() - start;
	uint32_t budget = spent < TEXT_AT_BUDGET_MS - 5 ? (uint32_t)(TEXT_AT_BUDGET_MS - spent) : 5;
	return gnome_atspi_text_at(g->atspi, p.window.pid, p.window.title, x - p.window.frame.x, y - p.window.frame.y,
	                           budget, buf, cap, msg);
}

/*
 * Consent
 */

static enum u_desktop_access
gnome_access_status(struct u_desktop_backend *b)
{
	struct gnome_backend *g = gnome(b);
	return g->portal ? gnome_portal_access(g->portal) : U_DESKTOP_ACCESS_DENIED;
}

static void
gnome_request_access(struct u_desktop_backend *b,
                     const struct u_desktop_app_info *app,
                     u_desktop_access_done_fn done,
                     void *ctx)
{
	(void)app; // the portal names the app by its app id (see the portal file)
	struct gnome_backend *g = gnome(b);
	if (g->portal == NULL) {
		U_LOG_W("desktop/gnome: no RemoteDesktop portal — there is no OS consent path to ask through");
		done(ctx, U_DESKTOP_ACCESS_DENIED);
		return;
	}
	gnome_portal_request(g->portal, done, ctx);
}

/*
 * Input
 */

bool
gnome_can_input(struct gnome_backend *g, char *msg)
{
	if (g->portal == NULL || !gnome_portal_has_input(g->portal)) {
		gnome_set_msg(msg, "no input on this system (needs the RemoteDesktop portal v2 and libei.so.1)");
		return false;
	}
	return true;
}

//! Move, then confirm with the extension that the pointer arrived (within 2 px).
enum u_desktop_result
gnome_move_verified(struct gnome_backend *g, int32_t x, int32_t y, char *msg)
{
	return gnome_move_verified_ex(g, x, y, true, msg);
}

enum u_desktop_result
gnome_move_verified_ex(struct gnome_backend *g, int32_t x, int32_t y, bool restore, char *msg)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		enum u_desktop_result r = gnome_portal_motion_ex(g->portal, x, y, restore, msg);
		if (r != U_DESKTOP_OK || !g->have_ext16) {
			return r;
		}
		uint64_t deadline = gdbus_now_ms() + 300;
		int32_t px = 0, py = 0;
		do {
			if (gnome_get_pointer_msg(g, &px, &py, NULL) == U_DESKTOP_OK &&
			    abs(px - x) <= POINTER_TOLERANCE_PX && abs(py - y) <= POINTER_TOLERANCE_PX) {
				return U_DESKTOP_OK;
			}
			gdbus_sleep_ms(10);
		} while (gdbus_now_ms() < deadline);
		gnome_set_msg(msg, "the pointer did not arrive at (%d,%d) — it is at (%d,%d)", x, y, px, py);
	}
	return U_DESKTOP_ERR_TIMEOUT;
}

/*
 * The delivery guarantee (u_desktop.h "Delivery guarantee"): the last checks,
 * on the worker thread, right before each injection batch. guard->still_allowed
 * is the common layer's and is only ever called from this (its own) thread.
 */

static bool
guard_allows(const struct u_desktop_input_guard *guard, char *msg)
{
	if (guard != NULL && guard->still_allowed != NULL && !guard->still_allowed(guard->ctx)) {
		gnome_set_msg(msg, "stopped: desktop control was suspended, revoked or the screen locked");
		return false;
	}
	return true;
}

enum u_desktop_result
gnome_guard_focus(struct gnome_backend *g, const struct u_desktop_input_guard *guard, uint64_t target, char *msg)
{
	if (guard == NULL) {
		return U_DESKTOP_OK; // our own cleanup (Escape after a gesture)
	}
	if (!guard_allows(guard, msg)) {
		return U_DESKTOP_ERR_REFUSED;
	}
	if (target == 0) {
		gnome_set_msg(msg, "no target window");
		return U_DESKTOP_ERR_REFUSED;
	}
	struct u_desktop_window *ws;
	uint32_t n;
	enum u_desktop_result r = gnome_snapshot(g, &ws, &n, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	bool ok = false;
	for (uint32_t i = 0; i < n; i++) {
		if (ws[i].state & U_DESKTOP_WINDOW_FOCUSED) {
			ok = ws[i].id == target || ws[i].parent_id == target;
			break;
		}
	}
	free(ws);
	if (!ok) {
		gnome_set_msg(msg, "focus moved — the target window no longer has keyboard focus");
		return U_DESKTOP_ERR_REFUSED;
	}
	return U_DESKTOP_OK;
}

enum u_desktop_result
gnome_guard_point(struct gnome_backend *g,
                  const struct u_desktop_input_guard *guard,
                  uint64_t target,
                  int32_t x,
                  int32_t y,
                  char *msg)
{
	if (!guard_allows(guard, msg)) {
		return U_DESKTOP_ERR_REFUSED;
	}
	int32_t px = 0, py = 0;
	enum u_desktop_result r = gnome_get_pointer_msg(g, &px, &py, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	if (abs(px - x) > POINTER_TOLERANCE_PX || abs(py - y) > POINTER_TOLERANCE_PX) {
		gnome_set_msg(msg, "the pointer moved off the verified point");
		return U_DESKTOP_ERR_REFUSED;
	}
	struct u_desktop_pick pk;
	r = gnome_pick_msg(g, x, y, &pk, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	if (target == 0 || pk.kind != U_DESKTOP_PICK_WINDOW ||
	    (pk.window.id != target && pk.window.parent_id != target)) {
		gnome_set_msg(msg, "target no longer under the point");
		return U_DESKTOP_ERR_REFUSED;
	}
	return U_DESKTOP_OK;
}

enum u_desktop_result
gnome_send_chord(struct gnome_backend *g,
                 const struct u_desktop_input_guard *guard,
                 uint64_t target,
                 uint32_t mods,
                 uint32_t code,
                 uint32_t ch,
                 char *msg)
{
	uint32_t rm = 0, rc = 0;
	enum u_desktop_result r = gnome_portal_resolve_chord(g->portal, mods, code, ch, &rm, &rc, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	uint32_t mc[4];
	uint32_t nm = gnome_chord_mod_codes(rm, mc);
	bool down[4] = {true, true, true, true}, up[4] = {false, false, false, false};
	uint32_t rev[4];
	for (uint32_t i = 0; i < nm; i++) {
		rev[i] = mc[nm - 1 - i];
	}
	// Before the first key-down.
	r = gnome_guard_focus(g, guard, target, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	if (nm > 0) {
		r = gnome_portal_keys(g->portal, mc, down, nm, msg);
		if (r != U_DESKTOP_OK) {
			char tmp[U_DESKTOP_MSG_SIZE];
			gnome_portal_keys(g->portal, rev, up, nm, tmp);
			return r;
		}
		if (rc == 0) {
			return gnome_portal_keys(g->portal, rev, up, nm, msg);
		}
		// Before the key-down of the key itself: release the modifiers on a stop.
		r = gnome_guard_focus(g, guard, target, msg);
		if (r != U_DESKTOP_OK) {
			char tmp[U_DESKTOP_MSG_SIZE];
			gnome_portal_keys(g->portal, rev, up, nm, tmp);
			return r;
		}
	}
	uint32_t codes[2 + 4];
	bool press[2 + 4];
	uint32_t n = 0;
	codes[n] = rc, press[n++] = true;
	codes[n] = rc, press[n++] = false;
	for (uint32_t i = 0; i < nm; i++) {
		codes[n] = rev[i], press[n++] = false;
	}
	r = gnome_portal_keys(g->portal, codes, press, n, msg);
	if (r != U_DESKTOP_OK && nm > 0) {
		char tmp[U_DESKTOP_MSG_SIZE];
		gnome_portal_keys(g->portal, rev, up, nm, tmp);
	}
	return r;
}

static enum u_desktop_result
gnome_prepare_input(struct u_desktop_backend *b, char *msg)
{
	struct gnome_backend *g = gnome(b);
	if (!gnome_can_input(g, msg)) {
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	return gnome_portal_prepare(g->portal, msg);
}

static enum u_desktop_result
gnome_pointer_move(struct u_desktop_backend *b, int32_t x, int32_t y, char *msg)
{
	struct gnome_backend *g = gnome(b);
	if (!gnome_can_input(g, msg)) {
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	return gnome_move_verified(g, x, y, msg);
}

static enum u_desktop_result
gnome_pointer_button(struct u_desktop_backend *b,
                     const struct u_desktop_input_guard *guard,
                     enum u_desktop_button button,
                     bool down,
                     int32_t x,
                     int32_t y,
                     char *msg)
{
	struct gnome_backend *g = gnome(b);
	if (!gnome_can_input(g, msg)) {
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	uint32_t code = button == U_DESKTOP_BUTTON_RIGHT    ? BTN_RIGHT
	                : button == U_DESKTOP_BUTTON_MIDDLE ? BTN_MIDDLE
	                                                    : BTN_LEFT;
	if (down) {
		// A press: the pointer is still on the verified point, the target is
		// still what a click there meets, and the guard holds.
		enum u_desktop_result r = gnome_guard_point(g, guard, guard ? guard->target_id : 0, x, y, msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
	}
	// A release is never refused: a button must not stay down.
	return gnome_portal_button(g->portal, code, down, msg);
}

static enum u_desktop_result
gnome_scroll(struct u_desktop_backend *b,
             const struct u_desktop_input_guard *guard,
             int32_t x,
             int32_t y,
             int32_t dx,
             int32_t dy,
             char *msg)
{
	struct gnome_backend *g = gnome(b);
	if (!gnome_can_input(g, msg)) {
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	enum u_desktop_result r = gnome_guard_point(g, guard, guard ? guard->target_id : 0, x, y, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	return gnome_portal_scroll(g->portal, dx, dy, msg);
}

//! Named (layout-independent) keys → evdev. Characters are resolved on the keymap.
static uint32_t
named_key(const char *k)
{
	static const struct
	{
		const char *name;
		uint32_t code;
	} t[] = {
	    {"enter", KEY_ENTER},
	    {"tab", KEY_TAB},
	    {"escape", KEY_ESC},
	    {"backspace", KEY_BACKSPACE},
	    {"delete", KEY_DELETE},
	    {"insert", KEY_INSERT},
	    {"home", KEY_HOME},
	    {"end", KEY_END},
	    {"pageup", KEY_PAGEUP},
	    {"pagedown", KEY_PAGEDOWN},
	    {"left", KEY_LEFT},
	    {"right", KEY_RIGHT},
	    {"up", KEY_UP},
	    {"down", KEY_DOWN},
	    {"space", KEY_SPACE},
	    {"capslock", KEY_CAPSLOCK},
	    {"printscreen", KEY_SYSRQ},
	    {"menu", KEY_COMPOSE},
	};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (strcmp(k, t[i].name) == 0) {
			return t[i].code;
		}
	}
	if (k[0] == 'f' && isdigit((unsigned char)k[1])) {
		int n = atoi(k + 1);
		if (n >= 1 && n <= 10) {
			return KEY_F1 + (uint32_t)(n - 1);
		}
		if (n == 11) {
			return KEY_F11;
		}
		if (n == 12) {
			return KEY_F12;
		}
		if (n >= 13 && n <= 24) {
			return KEY_F13 + (uint32_t)(n - 13);
		}
	}
	return 0;
}

//! Canonical character key names → the character.
static uint32_t
char_key(const char *k)
{
	if (strlen(k) == 1 && ((k[0] >= 'a' && k[0] <= 'z') || (k[0] >= '0' && k[0] <= '9'))) {
		return (uint32_t)k[0];
	}
	static const struct
	{
		const char *name;
		char ch;
	} t[] = {
	    {"minus", '-'},       {"equal", '='},        {"comma", ','},     {"period", '.'},
	    {"slash", '/'},       {"backslash", '\\'},   {"semicolon", ';'}, {"quote", '\''},
	    {"bracketleft", '['}, {"bracketright", ']'}, {"grave", '`'},
	};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (strcmp(k, t[i].name) == 0) {
			return (uint32_t)(unsigned char)t[i].ch;
		}
	}
	return 0;
}

static enum u_desktop_result
gnome_key_chord(struct u_desktop_backend *b,
                const struct u_desktop_input_guard *guard,
                const struct u_desktop_chord *chord,
                char *msg)
{
	struct gnome_backend *g = gnome(b);
	if (!gnome_can_input(g, msg)) {
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	uint32_t code = 0, ch = 0;
	if (chord->key[0] != '\0') {
		code = named_key(chord->key);
		if (code == 0) {
			ch = char_key(chord->key);
		}
		if (code == 0 && ch == 0) {
			gnome_set_msg(msg, "unknown key name"); // privacy: never echo the key
			return U_DESKTOP_ERR_FAILED;
		}
	}
	if (guard == NULL) {
		gnome_set_msg(msg, "no input guard");
		return U_DESKTOP_ERR_FAILED;
	}
	return gnome_send_chord(g, guard, guard->target_id, chord->mods, code, ch, msg);
}

//! A terminal pastes with Ctrl+Shift+V.
static bool
is_terminal(const char *app_id)
{
	char lower[U_DESKTOP_APP_ID_SIZE];
	size_t i = 0;
	for (; app_id[i] && i + 1 < sizeof(lower); i++) {
		lower[i] = (char)tolower((unsigned char)app_id[i]);
	}
	lower[i] = '\0';
	const char *last = strrchr(lower, '.');
	last = last ? last + 1 : lower;
	static const char *const terms[] = {
	    "gnome-terminal", "gnome-terminal-server",
	    "terminal",       "kgx",
	    "console",        "ptyxis",
	    "konsole",        "kitty",
	    "alacritty",      "wezterm",
	    "wezterm-gui",    "xterm",
	    "uxterm",         "foot",
	    "footclient",     "tilix",
	};
	for (size_t k = 0; k < sizeof(terms) / sizeof(terms[0]); k++) {
		if (strcmp(lower, terms[k]) == 0 || strcmp(last, terms[k]) == 0) {
			return true;
		}
	}
	return false;
}

static enum u_desktop_result
gnome_type_text(struct u_desktop_backend *b, const struct u_desktop_input_guard *guard, const char *utf8, char *msg)
{
	struct gnome_backend *g = gnome(b);
	if (!gnome_can_input(g, msg)) {
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	if (guard == NULL) {
		gnome_set_msg(msg, "no input guard");
		return U_DESKTOP_ERR_FAILED;
	}
	// Trailing newlines are sent as Enter key presses, each under its own check
	// (a pasted "\n" is up to the app; a Return must reach the verified target).
	size_t len = strlen(utf8);
	uint32_t enters = 0;
	while (len > 0 && utf8[len - 1] == '\n') {
		len--;
		enters++;
		if (len > 0 && utf8[len - 1] == '\r') {
			len--;
		}
	}
	if (len > 0) {
		char *body = strndup(utf8, len);
		if (body == NULL) {
			return U_DESKTOP_ERR_FAILED;
		}
		bool terminal = false;
		struct u_desktop_window *ws;
		uint32_t n;
		if (gnome_snapshot(g, &ws, &n, NULL) == U_DESKTOP_OK) {
			for (uint32_t i = 0; i < n; i++) {
				if (ws[i].state & U_DESKTOP_WINDOW_FOCUSED) {
					terminal = is_terminal(ws[i].app_id);
					break;
				}
			}
			free(ws);
		}
		// 1. Take the clipboard (slow: read the old text, offer ours, wait to own it).
		enum u_desktop_result r = gnome_portal_clip_take(g->portal, body, msg);
		memset(body, 0, len);
		free(body);
		if (r != U_DESKTOP_OK) {
			return r;
		}
		// 2. Right before the paste keystroke: the guard, and the focus.
		r = gnome_guard_focus(g, guard, guard->target_id, msg);
		if (r != U_DESKTOP_OK) {
			gnome_portal_clip_restore(g->portal);
			return r;
		}
		// 3. Paste (refused there if the clipboard changed hands meanwhile).
		r = gnome_portal_clip_paste(g->portal, terminal, msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
	}
	for (uint32_t i = 0; i < enters; i++) {
		enum u_desktop_result r = gnome_send_chord(g, guard, guard->target_id, 0, KEY_ENTER, 0, msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
	}
	return U_DESKTOP_OK;
}



/*
 * Lifetime
 */

static void
gnome_destroy(struct u_desktop_backend *b)
{
	struct gnome_backend *g = gnome(b);
	gnome_portal_destroy(g->portal);
	gnome_atspi_destroy(g->atspi);
	if (g->obs != NULL) {
		dbus_connection_close(g->obs);
		dbus_connection_unref(g->obs);
	}
	pthread_mutex_destroy(&g->obs_lock);
	free(g);
}

static bool
env_contains_ci(const char *var, const char *needle)
{
	const char *v = getenv(var);
	if (v == NULL) {
		return false;
	}
	size_t nl = strlen(needle);
	for (const char *s = v; *s; s++) {
		if (strncasecmp(s, needle, nl) == 0) {
			return true;
		}
	}
	return false;
}

struct u_desktop_backend *
u_desktop_backend_create_platform(void)
{
	const char *wl = getenv("WAYLAND_DISPLAY");
	const char *st = getenv("XDG_SESSION_TYPE");
	bool wayland = (wl != NULL && wl[0] != '\0') || (st != NULL && strcmp(st, "wayland") == 0);
	if (!wayland || !env_contains_ci("XDG_CURRENT_DESKTOP", "GNOME")) {
		return NULL;
	}
	DBusError e;
	dbus_error_init(&e);
	DBusConnection *obs = dbus_bus_get_private(DBUS_BUS_SESSION, &e);
	if (obs == NULL) {
		U_LOG_W("desktop/gnome: no session bus (%s) — no desktop control",
		        dbus_error_is_set(&e) ? e.message : "?");
		dbus_error_free(&e);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(obs, FALSE);

	struct gnome_backend *g = calloc(1, sizeof(*g));
	if (g == NULL) {
		dbus_connection_close(obs);
		dbus_connection_unref(obs);
		return NULL;
	}
	pthread_mutex_init(&g->obs_lock, NULL);
	g->obs = obs;

	// The extension: GetPointer exists from version 16.
	struct gdbus_err err;
	DBusMessage *reply = gdbus_call(obs, WLG_BUS, WLG_PATH, WLG_IFACE, "GetPointer", 1000, &err, DBUS_TYPE_INVALID);
	if (reply != NULL) {
		g->have_ext16 = true;
		dbus_message_unref(reply);
	} else if (strcmp(err.name, DBUS_ERROR_UNKNOWN_METHOD) == 0) {
		U_LOG_W(
		    "desktop/gnome: the DisplayXR GNOME extension is older than version 16 — no window list, pointer "
		    "or pick (update window-geometry@displayxr.org, then log out and in)");
	} else {
		U_LOG_W(
		    "desktop/gnome: the DisplayXR GNOME extension is not running (%s) — no window list, pointer or "
		    "pick",
		    err.name);
	}

	// The portals (property reads only: no portal method call on this connection).
	uint32_t rd_version = 0, clip_version = 0;
	gdbus_get_prop_u32(obs, PORTAL_DEST, PORTAL_PATH, "org.freedesktop.portal.RemoteDesktop", "version", 2000,
	                   &rd_version);
	bool have_clip = gdbus_get_prop_u32(obs, PORTAL_DEST, PORTAL_PATH, "org.freedesktop.portal.Clipboard",
	                                    "version", 1000, &clip_version);
	g->portal = gnome_portal_create(rd_version, have_clip);
	bool input = g->portal != NULL && gnome_portal_has_input(g->portal);
	if (rd_version == 0) {
		U_LOG_W(
		    "desktop/gnome: no RemoteDesktop portal — no way to ask for consent, desktop control stays off");
	} else if (!input) {
		U_LOG_W("desktop/gnome: no input (%s) — observation only",
		        rd_version < 2 ? "RemoteDesktop portal < v2" : "libei.so.1 missing");
	} else if (!have_clip) {
		U_LOG_W("desktop/gnome: no Clipboard portal — type_text unavailable");
	}

	if (g->have_ext16) {
		g->atspi = gnome_atspi_create(obs);
	}
	gdbus_drain(obs);

	uint32_t caps = 0;
	if (g->have_ext16) {
		caps |= U_DESKTOP_CAP_LIST_WINDOWS | U_DESKTOP_CAP_POINTER_POSITION | U_DESKTOP_CAP_PICK;
	}
	if (g->atspi != NULL) {
		caps |= U_DESKTOP_CAP_READ_TEXT;
	}
	if (input) {
		caps |= U_DESKTOP_CAP_INPUT;
		if (g->have_ext16) {
			caps |= U_DESKTOP_CAP_WINDOW_ACTIONS;
		}
	}

	g->base.name = "gnome";
	g->base.caps = caps;
	g->base.remembers_in_store = false; // the portal owns consent
	g->base.access_status = gnome_access_status;
	g->base.request_access = gnome_request_access;
	g->base.is_locked = gnome_is_locked;
	g->base.user_idle_ms = gnome_user_idle_ms;
	g->base.list_windows = gnome_list_windows;
	g->base.get_pointer = gnome_get_pointer;
	g->base.pick = gnome_pick;
	g->base.focused_window = gnome_focused_window;
	g->base.text_at = gnome_text_at;
	g->base.prepare_input = gnome_prepare_input;
	g->base.window_op = gnome_window_op;
	g->base.type_text = gnome_type_text;
	g->base.key_chord = gnome_key_chord;
	g->base.pointer_move = gnome_pointer_move;
	g->base.pointer_button = gnome_pointer_button;
	g->base.scroll = gnome_scroll;
	g->base.destroy = gnome_destroy;

	U_LOG_I("desktop/gnome: caps 0x%x (extension v16 %s, RemoteDesktop v%u, clipboard %s, libei %s, a11y %s)", caps,
	        g->have_ext16 ? "yes" : "no", rd_version, have_clip ? "yes" : "no",
	        gnome_portal_libei_available() ? "yes" : "no", g->atspi ? "yes" : "no");
	return &g->base;
}
