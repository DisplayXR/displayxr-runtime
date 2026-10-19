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
press_escape(struct gnome_backend *g, const struct u_desktop_input_guard *guard)
{
	// Our own cleanup (an overview / popup our gesture opened): no target to check,
	// but the kill switch / lock still applies to this keystroke.
	if (!guard->still_allowed(guard->ctx)) {
		return;
	}
	char m[U_DESKTOP_MSG_SIZE];
	gnome_send_chord(g, NULL, 0, 0, KEY_ESC, 0, m);
	gdbus_sleep_ms(200);
}

/*!
 * After a SUCCESSFUL Super gesture: no overview left open, no tiling popup holding
 * the keyboard. Never called after a refused / failed gesture: then nothing is sent
 * beyond releasing what the gesture pressed.
 */
static void
tidy_after_gesture(struct gnome_backend *g, const struct u_desktop_input_guard *guard, bool expect_focus)
{
	if (overview_active(g)) {
		U_LOG_I("desktop/gnome: a Super gesture opened the overview — closing it");
		press_escape(g, guard);
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
				press_escape(g, guard);
			}
		}
	}
}

static bool
rect_has(const struct u_desktop_rect *r, int32_t x, int32_t y, int32_t inset)
{
	return x >= r->x + inset && y >= r->y + inset && x < r->x + r->w - inset && y < r->y + r->h - inset;
}

/*
 * Monitors and pointer paths.
 *
 * A Super+drag must never let the pointer touch the zones where GNOME acts on a
 * moving window or blocks the pointer: mutter's edge tiling / maximize (the top
 * edge, the sides of each monitor's work area — also where two monitors meet),
 * Ubuntu's Tiling Assistant (its edge + corner zones: a quarter tile at a
 * monitor's top-left was seen on hardware), and the pointer barriers of the hot
 * corner and the dock (an absolute motion is constrained along its segment, so
 * a jump from one monitor to another near the top stopped AT the boundary).
 * So: the drop point is kept DROP_MARGIN_PX inside its monitor, and a path that
 * changes monitors goes through waypoints WAYPOINT_MARGIN_PX inside each one,
 * crossing between them through the middle of their shared edge.
 */

#define DROP_MARGIN_PX 48
#define WAYPOINT_MARGIN_PX 96
#define MAX_WAYPOINTS 2

struct monitors
{
	struct u_desktop_rect r[GNOME_MAX_MONITORS];
	uint32_t n;
};

//! The monitors, from the session's pointer regions; false when unknown (then
//! the pick-based fallback is used).
static bool
get_monitors(struct gnome_backend *g, struct monitors *m)
{
	char tmp[U_DESKTOP_MSG_SIZE];
	m->n = 0;
	return gnome_portal_monitors(g->portal, m->r, GNOME_MAX_MONITORS, &m->n, tmp) == U_DESKTOP_OK && m->n > 0;
}

static int
monitor_of(const struct monitors *m, int32_t x, int32_t y)
{
	for (uint32_t i = 0; i < m->n; i++) {
		if (rect_has(&m->r[i], x, y, 0)) {
			return (int)i;
		}
	}
	return -1;
}

static int32_t
clamp_i(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

//! (x, y) moved to the nearest point of @p r at least @p margin inside (its centre
//! on an axis too small for the margin).
static void
clamp_into(const struct u_desktop_rect *r, int32_t margin, int32_t *x, int32_t *y)
{
	if (r->w > 2 * margin) {
		*x = clamp_i(*x, r->x + margin, r->x + r->w - 1 - margin);
	} else {
		*x = r->x + r->w / 2;
	}
	if (r->h > 2 * margin) {
		*y = clamp_i(*y, r->y + margin, r->y + r->h - 1 - margin);
	} else {
		*y = r->y + r->h / 2;
	}
}

//! (x, y) is on a monitor and at least @p margin from that monitor's edges.
static bool
safe_on_monitor(const struct monitors *m, int32_t x, int32_t y, int32_t margin)
{
	int i = monitor_of(m, x, y);
	return i >= 0 && rect_has(&m->r[i], x, y, margin);
}

/*!
 * Waypoints from (fx, fy) to (tx, ty) when they are on different monitors (none
 * when on the same one, or when either is on no monitor — the motion itself then
 * refuses). Each waypoint is WAYPOINT_MARGIN_PX inside its monitor; the leg
 * between them crosses the monitors' shared span at a constant y (side by side)
 * or x (stacked), away from the corners.
 */
static uint32_t
route(const struct monitors *m, int32_t fx, int32_t fy, int32_t tx, int32_t ty, int32_t out[MAX_WAYPOINTS][2])
{
	int ia = monitor_of(m, fx, fy), ib = monitor_of(m, tx, ty);
	if (ia < 0 || ib < 0 || ia == ib) {
		return 0;
	}
	const struct u_desktop_rect *a = &m->r[ia], *b = &m->r[ib];
	const int32_t mg = WAYPOINT_MARGIN_PX;
	int32_t ax = fx, ay = fy, bx = tx, by = ty;
	clamp_into(a, mg, &ax, &ay);
	clamp_into(b, mg, &bx, &by);
	int32_t ylo = (a->y > b->y ? a->y : b->y) + mg;
	int32_t yhi = (a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h) - 1 - mg;
	int32_t xlo = (a->x > b->x ? a->x : b->x) + mg;
	int32_t xhi = (a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w) - 1 - mg;
	if (ylo <= yhi) {
		ay = by = clamp_i(ay, ylo, yhi); // side by side: cross at one y
	} else if (xlo <= xhi) {
		ax = bx = clamp_i(ax, xlo, xhi); // stacked: cross at one x
	} else {
		ax = a->x + a->w / 2, ay = a->y + a->h / 2; // diagonal: centre to centre
		bx = b->x + b->w / 2, by = b->y + b->h / 2;
	}
	out[0][0] = ax, out[0][1] = ay;
	out[1][0] = bx, out[1][1] = by;
	return 2;
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

//! Candidate grid per axis: 0.05, 0.15 … 0.95 of the frame (finer than the old 5x5,
//! so a sliver of a mostly-covered window is still found).
#define GRID_N 10

/*!
 * A point on window @p idx (of @p ws, top-most first) that a click would meet —
 * confirmed with GetWindowAt. With @p drag, the point displaced by (dx, dy) — the
 * drop point — must also be DROP_MARGIN_PX inside a monitor (@p mon, when known;
 * else probed with picks): no edge tiling, no pointer barrier, no clamp.
 */
static bool
visible_point(struct gnome_backend *g,
              const struct monitors *mon,
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
	// The title-bar area first (a click there does nothing in the app), then the centre, then
	// a grid. The centre early: a transparent, click-through window (a DisplayXR avatar
	// overlay) is clickable only where it draws, and that is mostly in the middle.
	int32_t cand[2 + GRID_N * GRID_N][2];
	int nc = 0;
	cand[nc][0] = f->x + f->w / 2;
	cand[nc][1] = f->y + (f->h / 4 < 16 ? f->h / 4 : 16);
	nc++;
	cand[nc][0] = f->x + f->w / 2;
	cand[nc][1] = f->y + f->h / 2;
	nc++;
	for (int j = 0; j < GRID_N; j++) {
		for (int i = 0; i < GRID_N; i++) {
			cand[nc][0] = f->x + (int32_t)((double)f->w * (0.05 + 0.1 * i));
			cand[nc][1] = f->y + (int32_t)((double)f->h * (0.05 + 0.1 * j));
			nc++;
		}
	}
	// Uncovered (by the frames above) first; the pick is the judge either way.
	int order[2 + GRID_N * GRID_N], no = 0;
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
	// Every candidate may be probed (one GetWindowAt each): on a click-through window most of
	// them land on the window behind, so a short budget gave up before reaching its content.
	for (int o = 0; o < no; o++) {
		int32_t x = cand[order[o]][0], y = cand[order[o]][1];
		if (!rect_has(f, x, y, 4)) {
			continue;
		}
		if (drag) {
			bool ok = mon->n > 0 ? safe_on_monitor(mon, x + dx, y + dy, DROP_MARGIN_PX)
			                     : inside_screen(g, x + dx, y + dy, DROP_MARGIN_PX);
			if (!ok) {
				continue;
			}
		}
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
              const struct monitors *mon,
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
	// Bring the pointer onto the window's monitor first, through the middle of the
	// shared edge (a straight jump from another monitor can stop at a barrier).
	int32_t wp[MAX_WAYPOINTS][2];
	int32_t px = 0, py = 0;
	uint32_t nwp =
	    mon->n > 0 && gnome_get_pointer_msg(g, &px, &py, NULL) == U_DESKTOP_OK ? route(mon, px, py, x, y, wp) : 0;
	for (uint32_t i = 0; i < nwp; i++) {
		enum u_desktop_result r = gnome_move_verified(g, wp[i][0], wp[i][1], msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
	}
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
			// Jumps, never a slow drag (that would sweep the tiling zones): to the
			// drop point directly on one monitor, else through the waypoints —
			// each verified, so a pointer stopped by a barrier fails the move here
			// instead of dropping the window where it stopped. Past verification:
			// a closed session is never reopened (that would put seconds between
			// the check and the drop) — it refuses.
			nwp = mon->n > 0 ? route(mon, x, y, x + dx, y + dy, wp) : 0;
			for (uint32_t i = 0; i < nwp && r == U_DESKTOP_OK; i++) {
				r = gnome_portal_motion_ex(g->portal, wp[i][0], wp[i][1], false, msg);
				gdbus_sleep_ms(30);
				if (r == U_DESKTOP_OK) {
					r = gnome_move_verified_ex(g, wp[i][0], wp[i][1], false, msg);
				}
			}
			if (r == U_DESKTOP_OK) {
				r = gnome_portal_motion_ex(g->portal, x + dx, y + dy, false, msg);
				gdbus_sleep_ms(60);
			}
			if (r == U_DESKTOP_OK) {
				r = gnome_move_verified_ex(g, x + dx, y + dy, false, msg);
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

//! Put the pointer back after a SUCCESSFUL gesture (never after a refusal), and
//! never by reopening a session that closed meanwhile.
static void
restore_pointer(struct gnome_backend *g, bool have, int32_t x, int32_t y)
{
	if (have) {
		char tmp[U_DESKTOP_MSG_SIZE];
		gnome_portal_motion_ex(g->portal, x, y, false, tmp);
	}
}

//! GNOME's unminimize animation is 250 ms; the pick needs the window settled on screen.
#define OWN_RESTORE_SETTLE_MS 400

/*!
 * After a gesture: mutter's grab from the Super+click / Super+drag (keys sent
 * before it ends are lost, and a frame read during it is not final). At least
 * GRAB_SETTLE_MS, then until the extension no longer reports the window as
 * "moving" (bounded).
 */
static void
wait_grab_end(struct gnome_backend *g, uint64_t id)
{
	gdbus_sleep_ms(GRAB_SETTLE_MS);
	uint64_t deadline = gdbus_now_ms() + 1000;
	bool moving = false;
	double scale = 1.0;
	while (gnome_window_info(g, id, &moving, &scale) && moving && gdbus_now_ms() < deadline) {
		gdbus_sleep_ms(30);
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
	struct monitors mon;
	get_monitors(g, &mon);
	int32_t x, y;
	bool found = visible_point(g, &mon, ws, idx, false, 0, 0, &x, &y);
	free(ws);
	if (!found) {
		gnome_set_msg(msg, "no visible part of the window to click (covered, or on another workspace)");
		return U_DESKTOP_ERR_REFUSED;
	}
	int32_t ox = 0, oy = 0;
	bool have_orig = gnome_get_pointer_msg(g, &ox, &oy, NULL) == U_DESKTOP_OK;
	r = super_gesture(g, guard, &mon, id, x, y, false, 0, 0, msg);
	if (r != U_DESKTOP_OK) {
		// Refused / failed: super_gesture released what it pressed; send nothing more
		// (no Escape, no pointer restore).
		return r;
	}
	wait_grab_end(g, id);
	tidy_after_gesture(g, guard, false);
	restore_pointer(g, have_orig, ox, oy);
	if (!wait_focus(g, id, 1000)) {
		gnome_set_msg(msg, "the window did not take focus");
		return U_DESKTOP_ERR_TIMEOUT;
	}
	return U_DESKTOP_OK;
}

//! How far a frame size may drift (after normalising for the monitor scale) and
//! still be "the same size": a size change beyond it is edge tiling's doing.
#define SIZE_TOLERANCE_PX 4

//! Window @p id's frame size in units comparable across monitors (see gnome_window_info).
static void
norm_size(struct gnome_backend *g, uint64_t id, const struct u_desktop_rect *f, double *w, double *h)
{
	bool moving = false;
	double scale = 1.0;
	gnome_window_info(g, id, &moving, &scale);
	*w = f->w / scale;
	*h = f->h / scale;
}

static bool
size_changed(double w0, double h0, double w1, double h1)
{
	return w1 - w0 > SIZE_TOLERANCE_PX || w0 - w1 > SIZE_TOLERANCE_PX || h1 - h0 > SIZE_TOLERANCE_PX ||
	       h0 - h1 > SIZE_TOLERANCE_PX;
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
	bool gesture_failed = false; //!< then nothing more is sent (no Escape, no pointer restore)
	struct u_desktop_rect frame = {0, 0, 0, 0};
	struct u_desktop_rect orig = {0, 0, 0, 0}; //!< the frame before the first gesture
	double orig_w = 0, orig_h = 0;             //!< its size, normalised (norm_size)
	struct monitors mon;
	get_monitors(g, &mon);
	// One gesture + one read-back correction; one more when a gesture got snapped
	// by edge tiling (the next drag un-tiles it, which restores its size and so
	// lands off by the rescale; the last one corrects that).
	int attempts = 2;
	bool snapped = false;
	for (int attempt = 0; attempt < attempts; attempt++) {
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
		double now_w = 0, now_h = 0;
		norm_size(g, id, &frame, &now_w, &now_h);
		if (attempt == 0) {
			orig = frame;
			orig_w = now_w;
			orig_h = now_h;
		} else if (!snapped &&
		           ((st & U_DESKTOP_WINDOW_MAXIMIZED) || size_changed(orig_w, orig_h, now_w, now_h))) {
			// The window changed size: edge tiling / the Tiling Assistant took
			// the drop. Correct it (drags only — no keybinding that a tiling
			// extension may have rebound).
			snapped = true;
			attempts = 3;
			U_LOG_I("desktop/gnome: a move was snapped by edge tiling (now %dx%d, was %dx%d) — correcting",
			        frame.w, frame.h, orig.w, orig.h);
		}
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
		bool found = visible_point(g, &mon, ws, idx, true, dx, dy, &x, &y);
		free(ws);
		if (!found) {
			gnome_set_msg(
			    msg,
			    "no visible point of the window can be dragged to that position (target off-screen, "
			    "or the window is covered)");
			r = U_DESKTOP_ERR_REFUSED;
			break;
		}
		r = super_gesture(g, guard, &mon, id, x, y, true, dx, dy, msg);
		if (r != U_DESKTOP_OK) {
			gesture_failed = true;
			break;
		}
		wait_grab_end(g, id);
		tidy_after_gesture(g, guard, true);
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
			double now_w = 0, now_h = 0;
			if (w != NULL) {
				norm_size(g, id, &w->frame, &now_w, &now_h);
			}
			bool tiled = w != NULL && ((w->state & U_DESKTOP_WINDOW_MAXIMIZED) ||
			                           size_changed(orig_w, orig_h, now_w, now_h));
			if (w == NULL) {
				r = U_DESKTOP_ERR_WINDOW_LOST;
			} else if (!tiled && llabs((int64_t)w->frame.x - rect->x) <= POINTER_TOLERANCE_PX &&
			           llabs((int64_t)w->frame.y - rect->y) <= POINTER_TOLERANCE_PX) {
				r = U_DESKTOP_OK;
			} else if (tiled) {
				gnome_set_msg(
				    msg,
				    "edge tiling snapped the window (now %d,%d %dx%d, was %dx%d) and it could not be "
				    "corrected",
				    w->frame.x, w->frame.y, w->frame.w, w->frame.h, orig.w, orig.h);
			} else {
				gnome_set_msg(msg, "the window landed at (%d,%d), not (%d,%d) — mutter constrained it",
				              w->frame.x, w->frame.y, rect->x, rect->y);
			}
			free(ws);
		}
	}
	if (!gesture_failed) {
		restore_pointer(g, have_orig, ox, oy);
	}
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
		if (strcmp(method, "ActivateWindow") == 0) {
			gnome_set_msg(msg,
			              "the GNOME extension did not give the window focus (extension 17+: another app "
			              "has the user's latest input; or a move / pointer drag, or the screen is locked)");
		} else {
			gnome_set_msg(
			    msg, "the GNOME extension refused %s (a move / pointer drag in progress, or the screen is locked)",
			    method);
		}
		return U_DESKTOP_ERR_REFUSED;
	}
	return U_DESKTOP_OK;
}

/*!
 * The app's own window to the front with focus. Extension 17+ gives focus only when the app
 * had the user's latest input (ActivateWindow's focus policy), so after the agent has typed
 * into another app it refuses and only flags the window. The agent acts with the user's
 * consent: it then does what the user would, a Super+click on the window, as for any other
 * window — restored first if @p minimized (SetMinimized never takes focus), and kept above
 * for the click if it is not already, so a window covered by another one can be clicked
 * (put back afterwards; never a keep-above the app or the user set). A window on another
 * workspace still has no visible part to click.
 */
static enum u_desktop_result
own_activate(struct gnome_backend *g, const struct u_desktop_input_guard *guard, uint64_t id, bool minimized, char *msg)
{
	enum u_desktop_result r = placement_call(g, "ActivateWindow", false, false, msg);
	if (r != U_DESKTOP_ERR_REFUSED || !gnome_can_input(g, NULL)) {
		return r;
	}
	if (minimized) {
		r = placement_call(g, "SetMinimized", true, false, msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
		// Let the restored window map and GNOME's unminimize animation end before the
		// gesture looks for a visible part of it.
		gdbus_sleep_ms(OWN_RESTORE_SETTLE_MS);
	}
	bool raised = false;
	if (gnome_window_above(g, id) == 0) {
		raised = placement_call(g, "SetKeepAbove", true, true, NULL) == U_DESKTOP_OK;
		if (raised) {
			// The pick can lag the restack until the output repaints: on a 3D panel at
			// 23.976 Hz the covered case failed twice with no wait, at 60 Hz it passed.
			gdbus_sleep_ms(GRAB_SETTLE_MS);
		}
	}
	r = activate_other(g, guard, id, msg);
	if (raised) {
		placement_call(g, "SetKeepAbove", true, false, NULL);
	}
	return r;
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
	bool minimized = (target.state & U_DESKTOP_WINDOW_MINIMIZED) != 0;

	if (own) {
		switch (op) {
		case U_DESKTOP_OP_ACTIVATE: return own_activate(g, guard, window_id, minimized, msg);
		case U_DESKTOP_OP_MINIMIZE: return placement_call(g, "SetMinimized", true, true, msg);
		case U_DESKTOP_OP_RESTORE:
			r = placement_call(g, "SetMinimized", true, false, msg);
			if (r != U_DESKTOP_OK) {
				return r;
			}
			// Restored. Focus is best effort (extension 17+ may keep it with the app the
			// user is in); an action that needs focus checks it before typing anyway.
			own_activate(g, guard, window_id, false, NULL);
			return U_DESKTOP_OK;
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
			r = own_activate(g, guard, window_id, minimized, msg);
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
		// Still open, and no separate dialog: libadwaita (AdwAlertDialog) and other
		// toolkits put the "Save changes?" prompt INSIDE the window. If the app is
		// alive (it answers AT-SPI from its main loop), that is the app asking for
		// confirmation — the same outcome as a dialog window. A hung app, or one we
		// cannot ask, stays a timeout.
		int alive = g->atspi != NULL ? gnome_atspi_app_responds(g->atspi, target.pid, 800) : -1;
		if (alive == 1) {
			gnome_set_msg(msg,
			              "the app asked for confirmation (its window stayed open and is responding — an "
			              "in-window prompt such as \"save changes?\")");
			return U_DESKTOP_OK;
		}
		gnome_set_msg(msg, alive == 0
		                       ? "the window is still open after Alt+F4 and its app is not responding"
		                       : "the window is still open after Alt+F4 (its app exposes no accessibility "
		                         "tree, so an in-window confirmation prompt cannot be told apart)");
		return U_DESKTOP_ERR_TIMEOUT;
	}
	default: gnome_set_msg(msg, "unknown window op"); return U_DESKTOP_ERR_FAILED;
	}
}
