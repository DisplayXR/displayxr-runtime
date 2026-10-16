// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  An in-memory desktop for XR_DXR_desktop_control tests. See u_desktop_fake.h.
 */

#include "u_desktop_fake.h"
#include "u_desktop_chord.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_MAX_WINDOWS 32
#define FAKE_CONTENT_SIZE 1024
#define FAKE_LOG_SIZE (64 * 1024)
#define SCREEN_W 1920
#define SCREEN_H 1080
#define PANEL_H 32

struct fake_window
{
	struct u_desktop_window w; //!< z is recomputed from the array order
	char content[FAKE_CONTENT_SIZE];
};

struct u_desktop_fake
{
	struct u_desktop_backend base;
	struct os_mutex mutex;

	struct fake_window windows[FAKE_MAX_WINDOWS]; //!< index 0 = top-most
	uint32_t window_count;
	uint64_t focus;
	int32_t px, py;

	bool pick_override_set;
	struct u_desktop_pick pick_override;

	enum u_desktop_access access;
	enum u_desktop_access answer;
	bool deferred;
	u_desktop_access_done_fn pending_done;
	void *pending_ctx;
	uint32_t request_count;

	bool locked;
	bool user_active;
	bool idle_unknown;
	uint64_t last_input_ns;

	uint32_t action_delay_ms;
	char fail_op[24];
	bool fail_any;
	enum u_desktop_result fail_result;
	int32_t skew_x, skew_y;
	uint64_t raise_on_move;

	char log[FAKE_LOG_SIZE];
	size_t log_len;
	uint32_t op_count;
};

static struct u_desktop_fake *
fake(struct u_desktop_backend *b)
{
	return (struct u_desktop_fake *)b;
}


/*
 *
 * Helpers (call with the mutex held).
 *
 */

static void
log_line(struct u_desktop_fake *f, const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	size_t len = strlen(line);
	if (f->log_len + len + 2 < sizeof(f->log)) {
		memcpy(f->log + f->log_len, line, len);
		f->log_len += len;
		f->log[f->log_len++] = '\n';
		f->log[f->log_len] = '\0';
	}
	f->op_count++;
}

static void
recompute_z(struct u_desktop_fake *f)
{
	for (uint32_t i = 0; i < f->window_count; i++) {
		f->windows[i].w.z = i;
		uint32_t st = f->windows[i].w.state & ~(uint32_t)U_DESKTOP_WINDOW_FOCUSED;
		if (f->windows[i].w.id == f->focus) {
			st |= U_DESKTOP_WINDOW_FOCUSED;
		}
		f->windows[i].w.state = st;
	}
}

static int
find_index(struct u_desktop_fake *f, uint64_t id)
{
	for (uint32_t i = 0; i < f->window_count; i++) {
		if (f->windows[i].w.id == id) {
			return (int)i;
		}
	}
	return -1;
}

static void
raise_index(struct u_desktop_fake *f, int idx)
{
	if (idx <= 0) {
		return;
	}
	struct fake_window tmp = f->windows[idx];
	memmove(&f->windows[1], &f->windows[0], sizeof(struct fake_window) * (size_t)idx);
	f->windows[0] = tmp;
}

static void
add_window_locked(struct u_desktop_fake *f, const struct u_desktop_window *w)
{
	if (f->window_count >= FAKE_MAX_WINDOWS) {
		return;
	}
	memmove(&f->windows[1], &f->windows[0], sizeof(struct fake_window) * f->window_count);
	memset(&f->windows[0], 0, sizeof(f->windows[0]));
	f->windows[0].w = *w;
	f->window_count++;
	if (w->state & U_DESKTOP_WINDOW_FOCUSED) {
		f->focus = w->id;
	}
	recompute_z(f);
}

static void
default_desktop(struct u_desktop_fake *f)
{
	struct u_desktop_window w;

	// Added bottom-most first: each add becomes the new top.
	memset(&w, 0, sizeof(w));
	w.id = 103;
	w.pid = 1003;
	snprintf(w.app_id, sizeof(w.app_id), "nautilus");
	snprintf(w.title, sizeof(w.title), "Home");
	w.frame = (struct u_desktop_rect){50, 720, 600, 300};
	add_window_locked(f, &w);

	memset(&w, 0, sizeof(w));
	w.id = 102;
	w.pid = 1002;
	snprintf(w.app_id, sizeof(w.app_id), "gnome-terminal");
	snprintf(w.title, sizeof(w.title), "Terminal");
	w.frame = (struct u_desktop_rect){1000, 200, 800, 600};
	add_window_locked(f, &w);

	memset(&w, 0, sizeof(w));
	w.id = 101;
	w.pid = 1001;
	snprintf(w.app_id, sizeof(w.app_id), "gedit");
	snprintf(w.title, sizeof(w.title), "notes.txt - Text Editor");
	w.frame = (struct u_desktop_rect){100, 100, 800, 600};
	w.state = U_DESKTOP_WINDOW_FOCUSED;
	add_window_locked(f, &w);

	f->px = 960;
	f->py = 540;
}

static bool
contains(const struct u_desktop_rect *r, int32_t x, int32_t y)
{
	return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static void
pick_locked(struct u_desktop_fake *f, int32_t x, int32_t y, struct u_desktop_pick *out)
{
	if (f->pick_override_set) {
		*out = f->pick_override;
		out->x = x;
		out->y = y;
		return;
	}
	memset(out, 0, sizeof(*out));
	out->x = x;
	out->y = y;
	if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) {
		out->kind = U_DESKTOP_PICK_OUTSIDE;
		return;
	}
	if (y < PANEL_H) {
		out->kind = U_DESKTOP_PICK_SHELL;
		return;
	}
	for (uint32_t i = 0; i < f->window_count; i++) {
		const struct u_desktop_window *w = &f->windows[i].w;
		if ((w->state & U_DESKTOP_WINDOW_MINIMIZED) == 0 && contains(&w->frame, x, y)) {
			out->kind = U_DESKTOP_PICK_WINDOW;
			out->window = *w;
			return;
		}
	}
	out->kind = U_DESKTOP_PICK_DESKTOP;
}

//! Returns the forced failure for @p op, or U_DESKTOP_OK.
static enum u_desktop_result
forced_failure(struct u_desktop_fake *f, const char *op, char *msg)
{
	if (f->fail_result == U_DESKTOP_OK) {
		return U_DESKTOP_OK;
	}
	if (f->fail_any || strcmp(f->fail_op, op) == 0) {
		if (msg != NULL) {
			snprintf(msg, U_DESKTOP_MSG_SIZE, "fake: forced failure of %s", op);
		}
		return f->fail_result;
	}
	return U_DESKTOP_OK;
}

static void
action_delay(struct u_desktop_fake *f)
{
	os_mutex_lock(&f->mutex);
	uint32_t ms = f->action_delay_ms;
	os_mutex_unlock(&f->mutex);
	if (ms > 0) {
		os_nanosleep((int64_t)ms * 1000000);
	}
}

static void
note_input_locked(struct u_desktop_fake *f)
{
	// The backend reports idle time for ANY input, ours included (u_desktop.h).
	f->last_input_ns = os_monotonic_get_ns();
}

static const char *
button_name(enum u_desktop_button b)
{
	switch (b) {
	case U_DESKTOP_BUTTON_RIGHT: return "right";
	case U_DESKTOP_BUTTON_MIDDLE: return "middle";
	default: return "left";
	}
}


/*
 *
 * Backend vtable.
 *
 */

static enum u_desktop_access
fake_access_status(struct u_desktop_backend *b)
{
	struct u_desktop_fake *f = fake(b);
	os_mutex_lock(&f->mutex);
	enum u_desktop_access a = f->access;
	os_mutex_unlock(&f->mutex);
	return a;
}

static void
fake_request_access(struct u_desktop_backend *b,
                    const struct u_desktop_app_info *app,
                    u_desktop_access_done_fn done,
                    void *ctx)
{
	(void)app;
	struct u_desktop_fake *f = fake(b);
	os_mutex_lock(&f->mutex);
	f->request_count++;
	enum u_desktop_access answer = f->answer;
	if (f->deferred) {
		f->pending_done = done;
		f->pending_ctx = ctx;
		f->access = U_DESKTOP_ACCESS_PENDING;
		os_mutex_unlock(&f->mutex);
		return;
	}
	if (!f->base.remembers_in_store) {
		f->access = answer;
	}
	os_mutex_unlock(&f->mutex);
	done(ctx, answer);
}

static bool
fake_is_locked(struct u_desktop_backend *b)
{
	struct u_desktop_fake *f = fake(b);
	os_mutex_lock(&f->mutex);
	bool l = f->locked;
	os_mutex_unlock(&f->mutex);
	return l;
}

static bool
fake_user_idle_ms(struct u_desktop_backend *b, uint64_t *out_ms)
{
	struct u_desktop_fake *f = fake(b);
	os_mutex_lock(&f->mutex);
	uint64_t now = os_monotonic_get_ns();
	if (f->user_active) {
		f->last_input_ns = now;
	}
	bool known = !f->idle_unknown;
	uint64_t ms = (now - f->last_input_ns) / 1000000;
	os_mutex_unlock(&f->mutex);
	if (known) {
		*out_ms = ms;
	}
	return known;
}

static enum u_desktop_result
fake_list_windows(struct u_desktop_backend *b, struct u_desktop_window *out, uint32_t cap, uint32_t *count)
{
	struct u_desktop_fake *f = fake(b);
	os_mutex_lock(&f->mutex);
	recompute_z(f);
	for (uint32_t i = 0; i < f->window_count && i < cap && out != NULL; i++) {
		out[i] = f->windows[i].w;
	}
	*count = f->window_count;
	os_mutex_unlock(&f->mutex);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
fake_get_pointer(struct u_desktop_backend *b, int32_t *x, int32_t *y)
{
	struct u_desktop_fake *f = fake(b);
	os_mutex_lock(&f->mutex);
	*x = f->px;
	*y = f->py;
	os_mutex_unlock(&f->mutex);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
fake_pick(struct u_desktop_backend *b, int32_t x, int32_t y, struct u_desktop_pick *out)
{
	struct u_desktop_fake *f = fake(b);
	os_mutex_lock(&f->mutex);
	recompute_z(f);
	pick_locked(f, x, y, out);
	os_mutex_unlock(&f->mutex);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
fake_focused_window(struct u_desktop_backend *b, uint64_t *out_id)
{
	struct u_desktop_fake *f = fake(b);
	os_mutex_lock(&f->mutex);
	*out_id = f->focus;
	os_mutex_unlock(&f->mutex);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
fake_text_at(struct u_desktop_backend *b, int32_t x, int32_t y, char *buf, size_t cap, char *msg)
{
	(void)msg;
	struct u_desktop_fake *f = fake(b);
	if (buf == NULL || cap == 0) {
		return U_DESKTOP_ERR_FAILED;
	}
	buf[0] = '\0';
	os_mutex_lock(&f->mutex);
	struct u_desktop_pick p;
	pick_locked(f, x, y, &p);
	if (p.kind == U_DESKTOP_PICK_WINDOW) {
		int idx = find_index(f, p.window.id);
		if (idx >= 0 && f->windows[idx].content[0] != '\0') {
			snprintf(buf, cap, "%s", f->windows[idx].content);
		} else {
			snprintf(buf, cap, "%s", p.window.title);
		}
	} else if (p.kind == U_DESKTOP_PICK_SHELL) {
		snprintf(buf, cap, "Activities");
	}
	os_mutex_unlock(&f->mutex);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
fake_window_op(struct u_desktop_backend *b,
               enum u_desktop_window_op op,
               uint64_t window_id,
               const struct u_desktop_rect *rect,
               char *msg)
{
	struct u_desktop_fake *f = fake(b);
	action_delay(f);
	os_mutex_lock(&f->mutex);
	enum u_desktop_result r = forced_failure(f, "window_op", msg);
	if (r != U_DESKTOP_OK) {
		os_mutex_unlock(&f->mutex);
		return r;
	}
	int idx = find_index(f, window_id);
	if (idx < 0) {
		os_mutex_unlock(&f->mutex);
		return U_DESKTOP_ERR_WINDOW_LOST;
	}
	struct u_desktop_window *w = &f->windows[idx].w;
	static const char *names[] = {"activate", "move_resize", "minimize", "restore", "maximize", "close"};
	const char *name = (unsigned)op < 6 ? names[op] : "?";
	switch (op) {
	case U_DESKTOP_OP_ACTIVATE:
		w->state &= ~(uint32_t)U_DESKTOP_WINDOW_MINIMIZED;
		f->focus = window_id;
		raise_index(f, idx);
		break;
	case U_DESKTOP_OP_MOVE_RESIZE:
		if (rect != NULL) {
			w->frame.x = rect->x;
			w->frame.y = rect->y;
			if (rect->w > 0 && rect->h > 0) {
				w->frame.w = rect->w;
				w->frame.h = rect->h;
			}
		}
		break;
	case U_DESKTOP_OP_MINIMIZE:
		w->state |= U_DESKTOP_WINDOW_MINIMIZED;
		if (f->focus == window_id) {
			f->focus = 0;
		}
		break;
	case U_DESKTOP_OP_RESTORE: w->state &= ~(uint32_t)(U_DESKTOP_WINDOW_MINIMIZED | U_DESKTOP_WINDOW_MAXIMIZED); break;
	case U_DESKTOP_OP_MAXIMIZE:
		w->state |= U_DESKTOP_WINDOW_MAXIMIZED;
		w->frame = (struct u_desktop_rect){0, PANEL_H, SCREEN_W, SCREEN_H - PANEL_H};
		break;
	case U_DESKTOP_OP_CLOSE:
		if (f->focus == window_id) {
			f->focus = 0;
		}
		memmove(&f->windows[idx], &f->windows[idx + 1],
		        sizeof(struct fake_window) * (f->window_count - (uint32_t)idx - 1));
		f->window_count--;
		break;
	default: os_mutex_unlock(&f->mutex); return U_DESKTOP_ERR_UNSUPPORTED;
	}
	recompute_z(f);
	log_line(f, "op %s %llu", name, (unsigned long long)window_id);
	os_mutex_unlock(&f->mutex);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
fake_type_text(struct u_desktop_backend *b, const char *utf8, char *msg)
{
	struct u_desktop_fake *f = fake(b);
	action_delay(f);
	os_mutex_lock(&f->mutex);
	enum u_desktop_result r = forced_failure(f, "type_text", msg);
	if (r == U_DESKTOP_OK) {
		int idx = find_index(f, f->focus);
		if (idx >= 0) {
			size_t have = strlen(f->windows[idx].content);
			snprintf(f->windows[idx].content + have, FAKE_CONTENT_SIZE - have, "%s", utf8);
		}
		log_line(f, "type %llu %s", (unsigned long long)f->focus, utf8);
		note_input_locked(f);
	}
	os_mutex_unlock(&f->mutex);
	return r;
}

static enum u_desktop_result
fake_key_chord(struct u_desktop_backend *b, const struct u_desktop_chord *chord, char *msg)
{
	struct u_desktop_fake *f = fake(b);
	action_delay(f);
	os_mutex_lock(&f->mutex);
	enum u_desktop_result r = forced_failure(f, "key_chord", msg);
	if (r == U_DESKTOP_OK) {
		char s[64];
		u_desktop_chord_format(chord, s, sizeof(s));
		log_line(f, "key %s", s);
		note_input_locked(f);
	}
	os_mutex_unlock(&f->mutex);
	return r;
}

static enum u_desktop_result
fake_pointer_move(struct u_desktop_backend *b, int32_t x, int32_t y, char *msg)
{
	struct u_desktop_fake *f = fake(b);
	action_delay(f);
	os_mutex_lock(&f->mutex);
	enum u_desktop_result r = forced_failure(f, "pointer_move", msg);
	if (r == U_DESKTOP_OK) {
		f->px = x + f->skew_x;
		f->py = y + f->skew_y;
		log_line(f, "move %d %d", x, y);
		note_input_locked(f);
		if (f->raise_on_move != 0) {
			raise_index(f, find_index(f, f->raise_on_move));
			f->raise_on_move = 0;
			recompute_z(f);
		}
		// Like a real backend: verify arrival within 2 px.
		int32_t dx = f->px - x, dy = f->py - y;
		if (dx < -2 || dx > 2 || dy < -2 || dy > 2) {
			if (msg != NULL) {
				snprintf(msg, U_DESKTOP_MSG_SIZE, "pointer landed at (%d,%d), not (%d,%d)", f->px, f->py,
				         x, y);
			}
			r = U_DESKTOP_ERR_FAILED;
		}
	}
	os_mutex_unlock(&f->mutex);
	return r;
}

static enum u_desktop_result
fake_pointer_button(struct u_desktop_backend *b, enum u_desktop_button button, bool down, char *msg)
{
	struct u_desktop_fake *f = fake(b);
	action_delay(f);
	os_mutex_lock(&f->mutex);
	enum u_desktop_result r = forced_failure(f, "pointer_button", msg);
	if (r == U_DESKTOP_OK) {
		if (down) {
			struct u_desktop_pick p;
			pick_locked(f, f->px, f->py, &p);
			if (p.kind == U_DESKTOP_PICK_WINDOW) {
				f->focus = p.window.id;
				raise_index(f, find_index(f, p.window.id));
				recompute_z(f);
			}
		}
		log_line(f, "button %s %s", button_name(button), down ? "down" : "up");
		note_input_locked(f);
	}
	os_mutex_unlock(&f->mutex);
	return r;
}

static enum u_desktop_result
fake_scroll(struct u_desktop_backend *b, int32_t dx, int32_t dy, char *msg)
{
	struct u_desktop_fake *f = fake(b);
	action_delay(f);
	os_mutex_lock(&f->mutex);
	enum u_desktop_result r = forced_failure(f, "scroll", msg);
	if (r == U_DESKTOP_OK) {
		log_line(f, "scroll %d %d", dx, dy);
		note_input_locked(f);
	}
	os_mutex_unlock(&f->mutex);
	return r;
}

static void
fake_destroy(struct u_desktop_backend *b)
{
	struct u_desktop_fake *f = fake(b);
	// A deferred answer still owed: the contract is "done exactly once".
	os_mutex_lock(&f->mutex);
	u_desktop_access_done_fn done = f->pending_done;
	void *ctx = f->pending_ctx;
	f->pending_done = NULL;
	os_mutex_unlock(&f->mutex);
	if (done != NULL) {
		done(ctx, U_DESKTOP_ACCESS_DENIED);
	}
	os_mutex_destroy(&f->mutex);
	free(f);
}


/*
 *
 * Public.
 *
 */

static bool
env_is(const char *name, const char *value)
{
	const char *v = getenv(name);
	return v != NULL && strcmp(v, value) == 0;
}

struct u_desktop_backend *
u_desktop_fake_create(void)
{
	struct u_desktop_fake *f = (struct u_desktop_fake *)calloc(1, sizeof(*f));
	if (f == NULL) {
		return NULL;
	}
	os_mutex_init(&f->mutex);
	f->base.name = "fake";
	f->base.caps = U_DESKTOP_CAP_LIST_WINDOWS | U_DESKTOP_CAP_POINTER_POSITION | U_DESKTOP_CAP_PICK |
	               U_DESKTOP_CAP_READ_TEXT | U_DESKTOP_CAP_WINDOW_ACTIONS | U_DESKTOP_CAP_INPUT;
	f->base.remembers_in_store = env_is("DXR_DESKTOP_FAKE_STORE", "1");
	f->base.access_status = fake_access_status;
	f->base.request_access = fake_request_access;
	f->base.is_locked = fake_is_locked;
	f->base.user_idle_ms = fake_user_idle_ms;
	f->base.list_windows = fake_list_windows;
	f->base.get_pointer = fake_get_pointer;
	f->base.pick = fake_pick;
	f->base.focused_window = fake_focused_window;
	f->base.text_at = fake_text_at;
	f->base.window_op = fake_window_op;
	f->base.type_text = fake_type_text;
	f->base.key_chord = fake_key_chord;
	f->base.pointer_move = fake_pointer_move;
	f->base.pointer_button = fake_pointer_button;
	f->base.scroll = fake_scroll;
	f->base.destroy = fake_destroy;

	f->access = U_DESKTOP_ACCESS_UNKNOWN;
	f->answer = env_is("DXR_DESKTOP_FAKE_ANSWER", "deny") ? U_DESKTOP_ACCESS_DENIED : U_DESKTOP_ACCESS_GRANTED;
	f->locked = env_is("DXR_DESKTOP_FAKE_LOCKED", "1");
	// The user last touched the machine long ago: no idle wait by default.
	uint64_t now = os_monotonic_get_ns();
	f->last_input_ns = now > 60ull * 1000000000ull ? now - 60ull * 1000000000ull : 0;
	default_desktop(f);
	return &f->base;
}

struct u_desktop_fake *
u_desktop_fake_from_backend(struct u_desktop_backend *b)
{
	if (b == NULL || b->destroy != fake_destroy) {
		return NULL;
	}
	return fake(b);
}

void
u_desktop_fake_add_window(struct u_desktop_fake *f, const struct u_desktop_window *w)
{
	os_mutex_lock(&f->mutex);
	add_window_locked(f, w);
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_remove_window(struct u_desktop_fake *f, uint64_t id)
{
	os_mutex_lock(&f->mutex);
	int idx = find_index(f, id);
	if (idx >= 0) {
		memmove(&f->windows[idx], &f->windows[idx + 1],
		        sizeof(struct fake_window) * (f->window_count - (uint32_t)idx - 1));
		f->window_count--;
		if (f->focus == id) {
			f->focus = 0;
		}
		recompute_z(f);
	}
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_set_focus(struct u_desktop_fake *f, uint64_t id)
{
	os_mutex_lock(&f->mutex);
	f->focus = id;
	recompute_z(f);
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_set_pick_override(struct u_desktop_fake *f, const struct u_desktop_pick *pick)
{
	os_mutex_lock(&f->mutex);
	f->pick_override_set = pick != NULL;
	if (pick != NULL) {
		f->pick_override = *pick;
	}
	os_mutex_unlock(&f->mutex);
}

bool
u_desktop_fake_window_content(struct u_desktop_fake *f, uint64_t id, char *out, size_t cap)
{
	os_mutex_lock(&f->mutex);
	int idx = find_index(f, id);
	if (idx >= 0) {
		snprintf(out, cap, "%s", f->windows[idx].content);
	}
	os_mutex_unlock(&f->mutex);
	return idx >= 0;
}

void
u_desktop_fake_set_access(struct u_desktop_fake *f, enum u_desktop_access a)
{
	os_mutex_lock(&f->mutex);
	f->access = a;
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_set_request_answer(struct u_desktop_fake *f, enum u_desktop_access answer, bool deferred)
{
	os_mutex_lock(&f->mutex);
	f->answer = answer;
	f->deferred = deferred;
	os_mutex_unlock(&f->mutex);
}

bool
u_desktop_fake_complete_request(struct u_desktop_fake *f)
{
	os_mutex_lock(&f->mutex);
	u_desktop_access_done_fn done = f->pending_done;
	void *ctx = f->pending_ctx;
	enum u_desktop_access answer = f->answer;
	f->pending_done = NULL;
	if (done != NULL && !f->base.remembers_in_store) {
		f->access = answer;
	}
	os_mutex_unlock(&f->mutex);
	if (done == NULL) {
		return false;
	}
	done(ctx, answer);
	return true;
}

uint32_t
u_desktop_fake_request_count(struct u_desktop_fake *f)
{
	os_mutex_lock(&f->mutex);
	uint32_t n = f->request_count;
	os_mutex_unlock(&f->mutex);
	return n;
}

void
u_desktop_fake_set_remembers_in_store(struct u_desktop_fake *f, bool remembers)
{
	os_mutex_lock(&f->mutex);
	f->base.remembers_in_store = remembers;
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_set_caps(struct u_desktop_fake *f, uint32_t caps)
{
	os_mutex_lock(&f->mutex);
	f->base.caps = caps;
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_set_locked(struct u_desktop_fake *f, bool locked)
{
	os_mutex_lock(&f->mutex);
	f->locked = locked;
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_set_user_active(struct u_desktop_fake *f, bool active)
{
	os_mutex_lock(&f->mutex);
	f->user_active = active;
	if (active) {
		f->last_input_ns = os_monotonic_get_ns();
	}
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_set_idle_unknown(struct u_desktop_fake *f, bool unknown)
{
	os_mutex_lock(&f->mutex);
	f->idle_unknown = unknown;
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_set_action_delay_ms(struct u_desktop_fake *f, uint32_t ms)
{
	os_mutex_lock(&f->mutex);
	f->action_delay_ms = ms;
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_set_action_result(struct u_desktop_fake *f, const char *op, enum u_desktop_result r)
{
	os_mutex_lock(&f->mutex);
	f->fail_result = r;
	f->fail_any = op == NULL;
	snprintf(f->fail_op, sizeof(f->fail_op), "%s", op != NULL ? op : "");
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_set_pointer_skew(struct u_desktop_fake *f, int32_t dx, int32_t dy)
{
	os_mutex_lock(&f->mutex);
	f->skew_x = dx;
	f->skew_y = dy;
	os_mutex_unlock(&f->mutex);
}

void
u_desktop_fake_raise_on_next_move(struct u_desktop_fake *f, uint64_t id)
{
	os_mutex_lock(&f->mutex);
	f->raise_on_move = id;
	os_mutex_unlock(&f->mutex);
}

size_t
u_desktop_fake_log(struct u_desktop_fake *f, char *out, size_t cap)
{
	os_mutex_lock(&f->mutex);
	size_t n = f->log_len < cap - 1 ? f->log_len : cap - 1;
	memcpy(out, f->log, n);
	out[n] = '\0';
	os_mutex_unlock(&f->mutex);
	return n;
}

uint32_t
u_desktop_fake_op_count(struct u_desktop_fake *f)
{
	os_mutex_lock(&f->mutex);
	uint32_t n = f->op_count;
	os_mutex_unlock(&f->mutex);
	return n;
}

void
u_desktop_fake_clear_log(struct u_desktop_fake *f)
{
	os_mutex_lock(&f->mutex);
	f->log_len = 0;
	f->log[0] = '\0';
	f->op_count = 0;
	os_mutex_unlock(&f->mutex);
}
