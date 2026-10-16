// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: window ops (ADR-056). Other apps' windows are
 *         operated as a user would — through the app's own portal session — since
 *         the extension never mutates them (ADR-056 D5); the app's own window goes
 *         through WindowPlacement1. See u_desktop_gnome.c's header.
 */

#include "u_desktop_gnome.h"

#include "util/u_logging.h"

#include <linux/input-event-codes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool
overview_active(struct gnome_backend *g)
{
	bool active = false;
	pthread_mutex_lock(&g->obs_lock);
	gdbus_get_prop_bool(g->obs, "org.gnome.Shell", "/org/gnome/Shell", "org.gnome.Shell", "OverviewActive",
	                    GNOME_OBS_TIMEOUT_MS, &active);
	pthread_mutex_unlock(&g->obs_lock);
	return active;
}

static void
press_escape(struct gnome_backend *g)
{
	char m[U_DESKTOP_MSG_SIZE];
	// Our own cleanup (an overview / popup our gesture opened): no target to check.
	gnome_send_chord(g, NULL, 0, 0, KEY_ESC, 0, m);
	gdbus_sleep_ms(200);
}

//! After a Super gesture: no overview left open, no tiling popup holding the keyboard.
static void
tidy_after_gesture(struct gnome_backend *g, bool expect_focus)
{
	if (overview_active(g)) {
		U_LOG_I("desktop/gnome: a Super gesture opened the overview — closing it");
		press_escape(g);
	}
	if (expect_focus) {
		struct u_desktop_window *ws;
		uint32_t n;
		if (gnome_snapshot(g, &ws, &n, NULL) == U_DESKTOP_OK) {
			bool any = false;
			for (uint32_t i = 0; i < n; i++) {
				any = any || (ws[i].state & U_DESKTOP_WINDOW_FOCUSED);
			}
			free(ws);
			if (!any) {
				// The keyboard went to shell UI (Ubuntu's tiling popup).
				U_LOG_I("desktop/gnome: no window has focus after a gesture — pressing Escape");
				press_escape(g);
			}
		}
	}
}

static bool
rect_has(const struct u_desktop_rect *r, int32_t x, int32_t y, int32_t inset)
{
	return x >= r->x + inset && y >= r->y + inset && x < r->x + r->w - inset && y < r->y + r->h - inset;
}

//! True when (x, y) is on a monitor at least @p margin px from its outer edges.
static bool
inside_screen(struct gnome_backend *g, int32_t x, int32_t y, int32_t margin)
{
	static const int d[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
	for (int i = 0; i < 5; i++) {
		struct u_desktop_pick p;
		if (gnome_pick_msg(g, x + d[i][0] * margin, y + d[i][1] * margin, &p, NULL) != U_DESKTOP_OK ||
		    p.kind == U_DESKTOP_PICK_OUTSIDE) {
			return false;
		}
	}
	return true;
}

/*!
 * A point on window @p idx (of @p ws, top-most first) that a click would meet —
 * confirmed with GetWindowAt. With @p drag, the point displaced by (dx, dy)
 * must also be well inside a monitor (no edge tiling, no pointer clamp).
 */
static bool
visible_point(struct gnome_backend *g,
              const struct u_desktop_window *ws,
              uint32_t idx,
              bool drag,
              int32_t dx,
              int32_t dy,
              int32_t *ox,
              int32_t *oy)
{
	const struct u_desktop_rect *f = &ws[idx].frame;
	if (f->w <= 8 || f->h <= 8) {
		return false;
	}
	// The title-bar area first (a click there does nothing in the app), then a grid.
	int32_t cand[1 + 25][2];
	int nc = 0;
	cand[nc][0] = f->x + f->w / 2;
	cand[nc][1] = f->y + (f->h / 4 < 16 ? f->h / 4 : 16);
	nc++;
	for (int j = 0; j < 5; j++) {
		for (int i = 0; i < 5; i++) {
			cand[nc][0] = f->x + (int32_t)((double)f->w * (0.1 + 0.2 * i));
			cand[nc][1] = f->y + (int32_t)((double)f->h * (0.1 + 0.2 * j));
			nc++;
		}
	}
	// Uncovered (by the frames above) first; the pick is the judge either way.
	int order[1 + 25], no = 0;
	for (int pass = 0; pass < 2; pass++) {
		for (int c = 0; c < nc; c++) {
			bool covered = false;
			for (uint32_t k = 0; k < idx && !covered; k++) {
				covered = !(ws[k].state & U_DESKTOP_WINDOW_MINIMIZED) &&
				          rect_has(&ws[k].frame, cand[c][0], cand[c][1], 0);
			}
			if (covered == (pass == 1)) {
				order[no++] = c;
			}
		}
	}
	int probes = 0;
	for (int o = 0; o < no && probes < 10; o++) {
		int32_t x = cand[order[o]][0], y = cand[order[o]][1];
		if (!rect_has(f, x, y, 4)) {
			continue;
		}
		if (drag && !inside_screen(g, x + dx, y + dy, 12)) {
			continue;
		}
		probes++;
		struct u_desktop_pick p;
		if (gnome_pick_msg(g, x, y, &p, NULL) == U_DESKTOP_OK && p.kind == U_DESKTOP_PICK_WINDOW &&
		    p.window.id == ws[idx].id) {
			*ox = x;
			*oy = y;
			return true;
		}
	}
	return false;
}

/*!
 * Super + left press at (x, y) (+ an optional drag to (x+dx, y+dy)) + releases,
 * under the guard. Super is held only across the button press (~20 ms), not the
 * whole drag: a key the user hits while Super is down becomes a Super shortcut
 * (Super+L locks the screen). mutter keeps the move grab it started on the press
 * until the button is released.
 */
static enum u_desktop_result
super_gesture(struct gnome_backend *g,
              const struct u_desktop_input_guard *guard,
              uint64_t target,
              int32_t x,
              int32_t y,
              bool drag,
              int32_t dx,
              int32_t dy,
              char *msg)
{
	const uint32_t super_key[1] = {KEY_LEFTMETA};
	const bool t[1] = {true}, f[1] = {false};
	char tmp[U_DESKTOP_MSG_SIZE];
	enum u_desktop_result r = gnome_move_verified(g, x, y, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	// The last look before the gesture: the guard, the pointer, the pick.
	r = gnome_guard_point(g, guard, target, x, y, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	r = gnome_portal_keys(g->portal, super_key, t, 1, msg);
	if (r != U_DESKTOP_OK) {
		gnome_portal_keys(g->portal, super_key, f, 1, tmp);
		return r;
	}
	r = gnome_portal_button(g->portal, BTN_LEFT, true, msg);
	gnome_portal_keys(g->portal, super_key, f, 1, tmp);
	if (r == U_DESKTOP_OK && drag) {
		gdbus_sleep_ms(40);
		if (!guard->still_allowed(guard->ctx)) {
			gnome_set_msg(msg, "stopped: desktop control was suspended, revoked or the screen locked");
			r = U_DESKTOP_ERR_REFUSED;
		} else {
			// Jump: a slow drag across a monitor edge pops the tiling assistant.
			r = gnome_portal_motion(g->portal, x + dx, y + dy, msg);
			gdbus_sleep_ms(60);
			if (r == U_DESKTOP_OK) {
				r = gnome_move_verified(g, x + dx, y + dy, msg);
			}
		}
	}
	gnome_portal_button(g->portal, BTN_LEFT, false, tmp);
	return r;
}

//! Wait (bounded) until window @p id has focus.
static bool
wait_focus(struct gnome_backend *g, uint64_t id, uint32_t timeout_ms)
{
	uint64_t deadline = gdbus_now_ms() + timeout_ms;
	do {
		uint64_t f = 0;
		if (gnome_focused_window(&g->base, &f) == U_DESKTOP_OK && f == id) {
			return true;
		}
		gdbus_sleep_ms(30);
	} while (gdbus_now_ms() < deadline);
	return false;
}

//! Wait (bounded) for @p pred on window @p id; @p gone: true when the window disappeared.
static bool
wait_state(struct gnome_backend *g, uint64_t id, uint32_t mask, bool set, uint32_t timeout_ms, bool *gone)
{
	uint64_t deadline = gdbus_now_ms() + timeout_ms;
	*gone = false;
	do {
		struct u_desktop_window *ws;
		uint32_t n;
		if (gnome_snapshot(g, &ws, &n, NULL) == U_DESKTOP_OK) {
			const struct u_desktop_window *w = gnome_find_window(ws, n, id);
			bool done = w == NULL || (((w->state & mask) != 0) == set);
			*gone = w == NULL;
			free(ws);
			if (done) {
				return !*gone || mask == 0;
			}
		}
		gdbus_sleep_ms(40);
	} while (gdbus_now_ms() < deadline);
	return false;
}

static void
restore_pointer(struct gnome_backend *g, bool have, int32_t x, int32_t y)
{
	if (have) {
		char tmp[U_DESKTOP_MSG_SIZE];
		gnome_portal_motion(g->portal, x, y, tmp);
	}
}

//! Super+click on a visible point of @p id, then wait for focus.
static enum u_desktop_result
activate_other(struct gnome_backend *g, const struct u_desktop_input_guard *guard, uint64_t id, char *msg)
{
	struct u_desktop_window *ws;
	uint32_t n;
	enum u_desktop_result r = gnome_snapshot(g, &ws, &n, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	uint32_t idx = 0;
	while (idx < n && ws[idx].id != id) {
		idx++;
	}
	if (idx == n) {
		free(ws);
		gnome_set_msg(msg, "the window is gone");
		return U_DESKTOP_ERR_WINDOW_LOST;
	}
	if (ws[idx].state & U_DESKTOP_WINDOW_FOCUSED) {
		free(ws);
		return U_DESKTOP_OK;
	}
	if (ws[idx].state & U_DESKTOP_WINDOW_MINIMIZED) {
		free(ws);
		gnome_set_msg(msg, "minimized windows of other apps cannot be restored on GNOME");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	int32_t x, y;
	bool found = visible_point(g, ws, idx, false, 0, 0, &x, &y);
	free(ws);
	if (!found) {
		gnome_set_msg(msg, "no visible part of the window to click (covered, or on another workspace)");
		return U_DESKTOP_ERR_REFUSED;
	}
	int32_t ox = 0, oy = 0;
	bool have_orig = gnome_get_pointer_msg(g, &ox, &oy, NULL) == U_DESKTOP_OK;
	r = super_gesture(g, guard, id, x, y, false, 0, 0, msg);
	// mutter's grab from the Super+click: keys sent before it ends are lost.
	gdbus_sleep_ms(GRAB_SETTLE_MS);
	tidy_after_gesture(g, false);
	restore_pointer(g, have_orig, ox, oy);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	if (!wait_focus(g, id, 1000)) {
		gnome_set_msg(msg, "the window did not take focus");
		return U_DESKTOP_ERR_TIMEOUT;
	}
	return U_DESKTOP_OK;
}

//! Keep window arithmetic far from int32 overflow (an app can pass any rect).
static int32_t
clamp_coord(int64_t v)
{
	const int64_t lim = 1000000;
	return (int32_t)(v < -lim ? -lim : v > lim ? lim : v);
}

static enum u_desktop_result
move_other(struct gnome_backend *g,
           const struct u_desktop_input_guard *guard,
           uint64_t id,
           const struct u_desktop_rect *rect_in,
           char *msg)
{
	struct u_desktop_rect rect_c = {clamp_coord(rect_in->x), clamp_coord(rect_in->y), clamp_coord(rect_in->w),
	                                clamp_coord(rect_in->h)};
	const struct u_desktop_rect *rect = &rect_c;
	int32_t ox = 0, oy = 0;
	bool have_orig = gnome_get_pointer_msg(g, &ox, &oy, NULL) == U_DESKTOP_OK;
	enum u_desktop_result r = U_DESKTOP_OK;
	struct u_desktop_rect frame = {0, 0, 0, 0};
	for (int attempt = 0; attempt < 2; attempt++) {
		struct u_desktop_window *ws;
		uint32_t n;
		r = gnome_snapshot(g, &ws, &n, msg);
		if (r != U_DESKTOP_OK) {
			break;
		}
		uint32_t idx = 0;
		while (idx < n && ws[idx].id != id) {
			idx++;
		}
		if (idx == n) {
			free(ws);
			gnome_set_msg(msg, "the window is gone");
			r = U_DESKTOP_ERR_WINDOW_LOST;
			break;
		}
		frame = ws[idx].frame;
		uint32_t st = ws[idx].state;
		if (attempt == 0 && (st & U_DESKTOP_WINDOW_MINIMIZED)) {
			free(ws);
			gnome_set_msg(msg, "the window is minimized");
			r = U_DESKTOP_ERR_REFUSED;
			break;
		}
		if (attempt == 0 && (st & (U_DESKTOP_WINDOW_MAXIMIZED | U_DESKTOP_WINDOW_FULLSCREEN))) {
			free(ws);
			gnome_set_msg(msg, "the window is maximized / fullscreen — RESTORE it first");
			r = U_DESKTOP_ERR_REFUSED;
			break;
		}
		if (attempt == 0 && ((rect->w > 0 && llabs((int64_t)rect->w - frame.w) > POINTER_TOLERANCE_PX) ||
		                     (rect->h > 0 && llabs((int64_t)rect->h - frame.h) > POINTER_TOLERANCE_PX))) {
			free(ws);
			gnome_set_msg(msg, "resizing other apps' windows is not possible on GNOME (move only)");
			r = U_DESKTOP_ERR_UNSUPPORTED;
			break;
		}
		int32_t dx = clamp_coord((int64_t)rect->x - frame.x), dy = clamp_coord((int64_t)rect->y - frame.y);
		if (abs(dx) <= POINTER_TOLERANCE_PX && abs(dy) <= POINTER_TOLERANCE_PX) {
			free(ws);
			r = U_DESKTOP_OK;
			break;
		}
		int32_t x, y;
		bool found = visible_point(g, ws, idx, true, dx, dy, &x, &y);
		free(ws);
		if (!found) {
			gnome_set_msg(
			    msg,
			    "no visible point of the window can be dragged to that position (target off-screen, "
			    "or the window is covered)");
			r = U_DESKTOP_ERR_REFUSED;
			break;
		}
		r = super_gesture(g, guard, id, x, y, true, dx, dy, msg);
		gdbus_sleep_ms(GRAB_SETTLE_MS);
		tidy_after_gesture(g, true);
		if (r != U_DESKTOP_OK) {
			break;
		}
		// FAILED = "verify by read-back": the next attempt re-reads the frame and
		// corrects by the residual; after the last one, the read-back below decides.
		r = U_DESKTOP_ERR_FAILED;
	}
	if (r == U_DESKTOP_ERR_FAILED) {
		// Read back after the last attempt.
		struct u_desktop_window *ws;
		uint32_t n;
		if (gnome_snapshot(g, &ws, &n, msg) == U_DESKTOP_OK) {
			const struct u_desktop_window *w = gnome_find_window(ws, n, id);
			if (w == NULL) {
				r = U_DESKTOP_ERR_WINDOW_LOST;
			} else if (llabs((int64_t)w->frame.x - rect->x) <= POINTER_TOLERANCE_PX &&
			           llabs((int64_t)w->frame.y - rect->y) <= POINTER_TOLERANCE_PX) {
				r = U_DESKTOP_OK;
			} else {
				gnome_set_msg(msg, "the window landed at (%d,%d), not (%d,%d) — mutter constrained it",
				              w->frame.x, w->frame.y, rect->x, rect->y);
			}
			free(ws);
		}
	}
	restore_pointer(g, have_orig, ox, oy);
	return r;
}

static enum u_desktop_result
placement_call(struct gnome_backend *g, const char *method, bool with_bool, bool flag, char *msg)
{
	dbus_uint32_t pid = 0; // 0 = the caller; the extension takes the pid from the bus
	dbus_bool_t b = flag;
	struct gdbus_err err;
	pthread_mutex_lock(&g->obs_lock);
	DBusMessage *reply = with_bool ? gdbus_call(g->obs, WLG_BUS, WLP_PATH, WLP_IFACE, method, 1000, &err,
	                                            DBUS_TYPE_UINT32, &pid, DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID)
	                               : gdbus_call(g->obs, WLG_BUS, WLP_PATH, WLP_IFACE, method, 1000, &err,
	                                            DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);
	pthread_mutex_unlock(&g->obs_lock);
	if (reply == NULL) {
		if (strcmp(err.name, DBUS_ERROR_UNKNOWN_METHOD) == 0) {
			gnome_set_msg(msg, "%s needs the DisplayXR GNOME extension version 15", method);
			return U_DESKTOP_ERR_UNSUPPORTED;
		}
		gnome_set_msg(msg, "WindowPlacement1.%s failed: %s", method, err.name);
		return U_DESKTOP_ERR_FAILED;
	}
	dbus_bool_t ok = FALSE;
	dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	if (!ok) {
		gnome_set_msg(
		    msg, "the GNOME extension refused %s (a move / pointer drag in progress, or the screen is locked)",
		    method);
		return U_DESKTOP_ERR_REFUSED;
	}
	return U_DESKTOP_OK;
}

static enum u_desktop_result
own_move(struct gnome_backend *g, int32_t x, int32_t y, char *msg)
{
	dbus_uint32_t pid = 0;
	dbus_int32_t ix = x, iy = y;
	struct gdbus_err err;
	pthread_mutex_lock(&g->obs_lock);
	DBusMessage *reply =
	    gdbus_call(g->obs, WLG_BUS, WLP_PATH, WLP_IFACE, "MoveWindow", 1000, &err, DBUS_TYPE_UINT32, &pid,
	               DBUS_TYPE_INT32, &ix, DBUS_TYPE_INT32, &iy, DBUS_TYPE_INVALID);
	pthread_mutex_unlock(&g->obs_lock);
	if (reply == NULL) {
		gnome_set_msg(msg, "WindowPlacement1.MoveWindow failed: %s", err.name);
		return U_DESKTOP_ERR_FAILED;
	}
	dbus_bool_t moved = FALSE;
	dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &moved, DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	if (!moved) {
		gnome_set_msg(msg, "the GNOME extension refused the move (the user is dragging the window)");
		return U_DESKTOP_ERR_REFUSED;
	}
	return U_DESKTOP_OK;
}

enum u_desktop_result
gnome_window_op(struct u_desktop_backend *b,
                const struct u_desktop_input_guard *guard,
                enum u_desktop_window_op op,
                uint64_t window_id,
                const struct u_desktop_rect *rect,
                char *msg)
{
	struct gnome_backend *g = gnome(b);
	if (guard == NULL) {
		gnome_set_msg(msg, "no input guard");
		return U_DESKTOP_ERR_FAILED;
	}
	struct u_desktop_window *ws;
	uint32_t n;
	enum u_desktop_result r = gnome_snapshot(g, &ws, &n, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	const struct u_desktop_window *w = gnome_find_window(ws, n, window_id);
	if (w == NULL) {
		free(ws);
		gnome_set_msg(msg, "no window %llu", (unsigned long long)window_id);
		return U_DESKTOP_ERR_WINDOW_LOST;
	}
	struct u_desktop_window target = *w;
	free(ws);
	bool own = (target.state & U_DESKTOP_WINDOW_OWN) != 0;

	if (own) {
		switch (op) {
		case U_DESKTOP_OP_ACTIVATE: return placement_call(g, "ActivateWindow", false, false, msg);
		case U_DESKTOP_OP_MINIMIZE: return placement_call(g, "SetMinimized", true, true, msg);
		case U_DESKTOP_OP_RESTORE:
			r = placement_call(g, "SetMinimized", true, false, msg);
			return r == U_DESKTOP_OK ? placement_call(g, "ActivateWindow", false, false, msg) : r;
		case U_DESKTOP_OP_MAXIMIZE:
			gnome_set_msg(msg, "maximizing the app's own window would fight DisplayXR's window placement");
			return U_DESKTOP_ERR_UNSUPPORTED;
		case U_DESKTOP_OP_MOVE_RESIZE:
			if ((rect->w > 0 && rect->w != target.frame.w) || (rect->h > 0 && rect->h != target.frame.h)) {
				gnome_set_msg(msg, "resizing is not available on GNOME (move only)");
				return U_DESKTOP_ERR_UNSUPPORTED;
			}
			return own_move(g, clamp_coord(rect->x), clamp_coord(rect->y), msg);
		case U_DESKTOP_OP_CLOSE:
			if (!gnome_can_input(g, msg)) {
				return U_DESKTOP_ERR_UNSUPPORTED;
			}
			r = placement_call(g, "ActivateWindow", false, false, msg);
			if (r == U_DESKTOP_OK && !wait_focus(g, window_id, 1000)) {
				gnome_set_msg(msg, "the window did not take focus");
				return U_DESKTOP_ERR_TIMEOUT;
			}
			return r == U_DESKTOP_OK
			           ? gnome_send_chord(g, guard, window_id, U_DESKTOP_MOD_ALT, KEY_F4, 0, msg)
			           : r;
		default: gnome_set_msg(msg, "unknown window op"); return U_DESKTOP_ERR_FAILED;
		}
	}

	if (!gnome_can_input(g, msg)) {
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	bool gone = false;
	switch (op) {
	case U_DESKTOP_OP_ACTIVATE: return activate_other(g, guard, window_id, msg);

	case U_DESKTOP_OP_MOVE_RESIZE: return move_other(g, guard, window_id, rect, msg);

	case U_DESKTOP_OP_MINIMIZE:
		if (target.state & U_DESKTOP_WINDOW_MINIMIZED) {
			return U_DESKTOP_OK;
		}
		r = activate_other(g, guard, window_id, msg);
		if (r == U_DESKTOP_OK) {
			// Keys go to the focused window: re-check it holds focus (and the guard).
			r = gnome_send_chord(g, guard, window_id, U_DESKTOP_MOD_SUPER, 0, 'h', msg);
		}
		if (r == U_DESKTOP_OK && !wait_state(g, window_id, U_DESKTOP_WINDOW_MINIMIZED, true, 1000, &gone)) {
			gnome_set_msg(msg,
			              gone ? "the window is gone" : "the window did not minimize (Super+H rebound?)");
			r = gone ? U_DESKTOP_ERR_WINDOW_LOST : U_DESKTOP_ERR_TIMEOUT;
		}
		return r;

	case U_DESKTOP_OP_MAXIMIZE:
		if (target.state & U_DESKTOP_WINDOW_MAXIMIZED) {
			return U_DESKTOP_OK;
		}
		r = activate_other(g, guard, window_id, msg);
		if (r == U_DESKTOP_OK) {
			r = gnome_send_chord(g, guard, window_id, U_DESKTOP_MOD_SUPER, KEY_UP, 0, msg);
		}
		if (r == U_DESKTOP_OK && !wait_state(g, window_id, U_DESKTOP_WINDOW_MAXIMIZED, true, 1000, &gone)) {
			gnome_set_msg(msg, gone ? "the window is gone"
			                        : "the window did not maximize (not resizable, or Super+Up rebound?)");
			r = gone ? U_DESKTOP_ERR_WINDOW_LOST : U_DESKTOP_ERR_TIMEOUT;
		}
		return r;

	case U_DESKTOP_OP_RESTORE:
		if (target.state & U_DESKTOP_WINDOW_MINIMIZED) {
			gnome_set_msg(msg, "minimized windows of other apps cannot be restored on GNOME");
			return U_DESKTOP_ERR_UNSUPPORTED;
		}
		r = activate_other(g, guard, window_id, msg);
		if (r == U_DESKTOP_OK && (target.state & U_DESKTOP_WINDOW_MAXIMIZED)) {
			r = gnome_send_chord(g, guard, window_id, U_DESKTOP_MOD_SUPER, KEY_DOWN, 0, msg);
			if (r == U_DESKTOP_OK &&
			    !wait_state(g, window_id, U_DESKTOP_WINDOW_MAXIMIZED, false, 1000, &gone)) {
				gnome_set_msg(msg, gone ? "the window is gone" : "the window did not unmaximize");
				r = gone ? U_DESKTOP_ERR_WINDOW_LOST : U_DESKTOP_ERR_TIMEOUT;
			}
		}
		return r;

	case U_DESKTOP_OP_CLOSE: {
		r = activate_other(g, guard, window_id, msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
		// Alt+F4 closes whatever has focus: it must still be the target.
		r = gnome_send_chord(g, guard, window_id, U_DESKTOP_MOD_ALT, KEY_F4, 0, msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
		// Gone, or a confirmation dialog of its own ("Save changes?") came up.
		uint64_t deadline = gdbus_now_ms() + 1500;
		do {
			if (gnome_snapshot(g, &ws, &n, NULL) == U_DESKTOP_OK) {
				bool still = gnome_find_window(ws, n, window_id) != NULL, dialog = false;
				for (uint32_t i = 0; i < n; i++) {
					dialog = dialog || ws[i].parent_id == window_id;
				}
				free(ws);
				if (!still) {
					return U_DESKTOP_OK;
				}
				if (dialog) {
					gnome_set_msg(msg, "the app asked for confirmation (a dialog is open)");
					return U_DESKTOP_OK;
				}
			}
			gdbus_sleep_ms(50);
		} while (gdbus_now_ms() < deadline);
		gnome_set_msg(msg, "the window is still open after Alt+F4");
		return U_DESKTOP_ERR_TIMEOUT;
	}
	default: gnome_set_msg(msg, "unknown window op"); return U_DESKTOP_ERR_FAILED;
	}
}
