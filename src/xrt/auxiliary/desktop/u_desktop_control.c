// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The desktop-control common layer (XR_DXR_desktop_control spec §3–§5,
 *         ADR-056 D3). See u_desktop_control.h.
 *
 * Locking: one mutex guards the consent state, the policy cache and the queue.
 * Only access_status (an in-memory answer on every backend) is called under it.
 * is_locked can be a D-Bus round trip (GNOME, up to 250 ms): it is always called
 * with the mutex RELEASED and its answer stored in a snapshot the state machine
 * reads (see probe_locked), so a slow lock query never blocks the app thread's
 * observation calls. Backend ACTION calls and request_access are always made with
 * it released too.
 */

#include "u_desktop_control.h"
#include "u_desktop_chord.h"
#include "u_desktop_policy.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_PER_MS 1000000ull
#define DEFAULT_POLL_NS (50ull * NS_PER_MS)
//! Input this soon after one of our injections ends is taken for ours (late OS
//! processing). Short on purpose: a user keystroke inside it is the residual blind
//! spot (spec §9); the other direction (our late event mistaken for the user) only
//! costs one idle wait.
#define DEFAULT_GRACE_NS (30ull * NS_PER_MS)
#define DEFAULT_MAX_WAIT_NS (10000ull * NS_PER_MS)
#define EXE_CAP 1024
#define NAME_CAP 256
#define MSG_CAP U_DESKTOP_MSG_SIZE
#define U_DESKTOP_CONTROL_INJECTION_HISTORY 32

static const char k_em_dash[] = "\xe2\x80\x94";

//! Why the state is what it is (finer than the state for SUSPENDED).
enum reason
{
	R_NO_BACKEND,
	R_ENV_OFF,
	R_STORE_UNREADABLE, //!< row 3, fail closed: the store exists but cannot be parsed
	R_KILL_SWITCH,
	R_STORE_DENY,
	R_LOCKED,
	R_NOT_REQUESTED,
	R_PENDING,
	R_STORE_ALLOW,
	R_OS_GRANTED,
	R_REVOKED, //!< row 10: the backend reported NOT_GRANTED mid-session
	R_DENIED,
};

/*!
 * The context handed to backend->request_access. It outlives the control object:
 * one reference belongs to the control, one to each request in flight, and the
 * callback only reaches the control through @ref c, which destroy clears under
 * @ref mutex. So an answer that arrives after destroy (a backend breaking the
 * Lifetime contract) is dropped instead of touching freed memory.
 */
struct request_ctx
{
	struct os_mutex mutex;
	int refs;
	struct u_desktop_control *c; //!< NULL once the control is dying
};

struct queued_action
{
	uint64_t id;
	struct u_desktop_action a; //!< text/keys point at the owned copies below
	char *text;
	char *keys;
	struct u_desktop_chord chord;
};

struct u_desktop_control
{
	struct u_desktop_backend *b;
	bool env_off;
	char exe[EXE_CAP];
	char name[NAME_CAP];
	struct u_desktop_control_callbacks cb;
	struct u_desktop_control_timing timing;

	struct os_mutex mutex;
	struct os_cond cond;
	struct request_ctx *rq; //!< see struct request_ctx

	// --- under mutex ---
	bool dying;   //!< set before backend->destroy: consent answers are ignored from here on
	bool revoked; //!< the backend reported NOT_GRANTED: DENIED until a new request
	struct u_desktop_policy *policy;
	bool requested;
	bool pending;
	enum u_desktop_access req_result;
	bool store_write_failed;
	enum u_desktop_control_state last_state;
	//! Last backend->is_locked answer (probed with the mutex released, see probe_locked)
	//! and the ticket of the probe that produced it: an older probe finishing late never
	//! overwrites a newer answer.
	bool locked_snapshot;
	uint64_t locked_seq_issued, locked_seq_stored;
	//! state + 1 of the last submit refused for state (0 = none since the last accepted
	//! submit): refusals are audited once per state, so a spamming app cannot flood the log.
	int refused_audit_state_plus1;

	struct queued_action q[U_DESKTOP_CONTROL_QUEUE_SIZE];
	uint32_t q_head, q_count;
	uint64_t next_id;
	bool stop;

	// --- worker only ---
	struct os_thread thread;
	bool thread_started;
	//! Our recent injections [start, end] (spec §5 step 2): input whose OS timestamp
	//! falls inside one of them (+ grace) is ours; anything else is the user's.
	struct
	{
		uint64_t start_ns, end_ns;
	} inj[U_DESKTOP_CONTROL_INJECTION_HISTORY];
	uint32_t inj_next, inj_count;
	struct u_desktop_window *list_buf;
	uint32_t list_cap;
};


/*
 *
 * Small helpers.
 *
 */

const char *
u_desktop_control_state_str(enum u_desktop_control_state s)
{
	switch (s) {
	case U_DESKTOP_STATE_UNSUPPORTED: return "UNSUPPORTED";
	case U_DESKTOP_STATE_NOT_REQUESTED: return "NOT_REQUESTED";
	case U_DESKTOP_STATE_PENDING: return "PENDING";
	case U_DESKTOP_STATE_GRANTED: return "GRANTED";
	case U_DESKTOP_STATE_DENIED: return "DENIED";
	case U_DESKTOP_STATE_DISABLED_BY_USER: return "DISABLED_BY_USER";
	case U_DESKTOP_STATE_SUSPENDED: return "SUSPENDED";
	default: return "?";
	}
}

static const char *
action_name(uint32_t type)
{
	switch (type) {
	case U_DESKTOP_ACTION_ACTIVATE: return "activate";
	case U_DESKTOP_ACTION_MOVE_RESIZE: return "move_resize";
	case U_DESKTOP_ACTION_MINIMIZE: return "minimize";
	case U_DESKTOP_ACTION_RESTORE: return "restore";
	case U_DESKTOP_ACTION_MAXIMIZE: return "maximize";
	case U_DESKTOP_ACTION_CLOSE: return "close";
	case U_DESKTOP_ACTION_TYPE_TEXT: return "type_text";
	case U_DESKTOP_ACTION_KEY_CHORD: return "key_chord";
	case U_DESKTOP_ACTION_POINTER_MOVE: return "pointer_move";
	case U_DESKTOP_ACTION_CLICK: return "click";
	case U_DESKTOP_ACTION_DRAG: return "drag";
	case U_DESKTOP_ACTION_SCROLL: return "scroll";
	default: return NULL;
	}
}

static bool
is_window_action(uint32_t t)
{
	return t <= U_DESKTOP_ACTION_CLOSE;
}

static bool
is_pointer_action(uint32_t t)
{
	return t == U_DESKTOP_ACTION_POINTER_MOVE || t == U_DESKTOP_ACTION_CLICK || t == U_DESKTOP_ACTION_DRAG ||
	       t == U_DESKTOP_ACTION_SCROLL;
}

static bool
is_keyboard_action(uint32_t t)
{
	return t == U_DESKTOP_ACTION_TYPE_TEXT || t == U_DESKTOP_ACTION_KEY_CHORD;
}

//! Code points in valid UTF-8, or -1 if invalid.
static int64_t
utf8_count(const char *s)
{
	int64_t n = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p != 0) {
		unsigned c = *p;
		int extra;
		if (c < 0x80) {
			extra = 0;
		} else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
			extra = 1;
		} else if ((c & 0xF0) == 0xE0) {
			extra = 2;
		} else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
			extra = 3;
		} else {
			return -1;
		}
		p++;
		for (int i = 0; i < extra; i++, p++) {
			if ((*p & 0xC0) != 0x80) {
				return -1;
			}
		}
		n++;
	}
	return n;
}

static int32_t
map_backend_result(enum u_desktop_result r, char *msg, size_t cap)
{
	const char *dflt = NULL;
	int32_t x = U_DESKTOP_XR_ERROR_RUNTIME_FAILURE;
	switch (r) {
	case U_DESKTOP_OK: return U_DESKTOP_XR_SUCCESS;
	case U_DESKTOP_ERR_UNSUPPORTED:
		x = U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED;
		dflt = "not supported by this desktop backend";
		break;
	case U_DESKTOP_ERR_NOT_GRANTED:
		x = U_DESKTOP_XR_ERROR_NOT_GRANTED;
		dflt = "the OS refused: permission missing or revoked";
		break;
	case U_DESKTOP_ERR_WINDOW_LOST:
		x = U_DESKTOP_XR_ERROR_WINDOW_LOST;
		dflt = "the target window no longer exists";
		break;
	case U_DESKTOP_ERR_REFUSED:
		x = U_DESKTOP_XR_ERROR_REFUSED;
		dflt = "the OS refused this operation";
		break;
	case U_DESKTOP_ERR_TIMEOUT: dflt = "the OS did not answer in time"; break;
	default: dflt = "the operation failed"; break;
	}
	if (msg != NULL && msg[0] == '\0' && dflt != NULL) {
		snprintf(msg, cap, "%s", dflt);
	}
	return x;
}

static const char *
result_str(int32_t r)
{
	switch (r) {
	case U_DESKTOP_XR_SUCCESS: return "ok";
	case U_DESKTOP_XR_ERROR_REFUSED: return "refused";
	case U_DESKTOP_XR_ERROR_NOT_GRANTED: return "not_granted";
	case U_DESKTOP_XR_ERROR_WINDOW_LOST: return "window_lost";
	case U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED: return "unsupported";
	case U_DESKTOP_XR_ERROR_VALIDATION_FAILURE: return "invalid";
	default: return "failed";
	}
}


/*
 *
 * Consent state machine (spec §3) — call with the mutex held.
 *
 */

static enum u_desktop_control_state
evaluate_locked(struct u_desktop_control *c, enum reason *out_reason)
{
	enum reason r;
	enum u_desktop_control_state s;

	if (c->b == NULL) { // 1
		r = R_NO_BACKEND;
		s = U_DESKTOP_STATE_UNSUPPORTED;
		goto out;
	}
	if (c->env_off) { // 2
		r = R_ENV_OFF;
		s = U_DESKTOP_STATE_UNSUPPORTED;
		goto out;
	}
	u_desktop_policy_refresh(c->policy);
	if (u_desktop_policy_is_unreadable(c->policy)) { // 3, fail closed (spec §6.1)
		r = R_STORE_UNREADABLE;
		s = U_DESKTOP_STATE_SUSPENDED;
		goto out;
	}
	if (u_desktop_policy_is_suspended(c->policy)) { // 3
		r = R_KILL_SWITCH;
		s = U_DESKTOP_STATE_SUSPENDED;
		goto out;
	}
	enum u_desktop_app_decision d = u_desktop_policy_app(c->policy, c->exe);
	if (d == U_DESKTOP_APP_DENY) { // 4
		r = R_STORE_DENY;
		s = U_DESKTOP_STATE_DISABLED_BY_USER;
		goto out;
	}
	if (c->locked_snapshot) { // 5 (probed outside the mutex: probe_locked)
		r = R_LOCKED;
		s = U_DESKTOP_STATE_SUSPENDED;
		goto out;
	}
	if (!c->requested) { // 6
		r = R_NOT_REQUESTED;
		s = U_DESKTOP_STATE_NOT_REQUESTED;
		goto out;
	}
	if (c->pending) { // 7
		r = R_PENDING;
		s = U_DESKTOP_STATE_PENDING;
		goto out;
	}
	if (c->revoked) {
		// The OS took the permission back mid-session (backend NOT_GRANTED):
		// refuse everything without reaching the OS until a new request.
		r = R_REVOKED;
		s = U_DESKTOP_STATE_DENIED;
		goto out;
	}
	if (c->b->remembers_in_store) {
		// 8. The DisplayXR store is the ONLY grant for a backend whose OS has
		// no consent of its own (Windows): an OS "granted" means nothing there.
		// The in-memory answer counts only if persisting it failed.
		if (d == U_DESKTOP_APP_ALLOW || (c->store_write_failed && c->req_result == U_DESKTOP_ACCESS_GRANTED)) {
			r = R_STORE_ALLOW;
			s = U_DESKTOP_STATE_GRANTED;
			goto out;
		}
	} else {
		// 9. The OS's view, else (when it cannot tell without prompting) the
		// answer to this session's request.
		enum u_desktop_access a =
		    c->b->access_status != NULL ? c->b->access_status(c->b) : U_DESKTOP_ACCESS_UNKNOWN;
		if (a == U_DESKTOP_ACCESS_GRANTED ||
		    (a == U_DESKTOP_ACCESS_UNKNOWN && c->req_result == U_DESKTOP_ACCESS_GRANTED)) {
			r = R_OS_GRANTED;
			s = U_DESKTOP_STATE_GRANTED;
			goto out;
		}
	}
	r = R_DENIED; // 10
	s = U_DESKTOP_STATE_DENIED;

out:
	if (out_reason != NULL) {
		*out_reason = r;
	}
	return s;
}

//! Evaluate; fire state_changed on a change (or always, with @p force).
static enum u_desktop_control_state
update_state_locked(struct u_desktop_control *c, enum reason *out_reason, bool force)
{
	enum u_desktop_control_state s = evaluate_locked(c, out_reason);
	if ((s != c->last_state || force) && !c->stop) {
		c->last_state = s;
		if (c->cb.state_changed != NULL) {
			c->cb.state_changed(c->cb.ctx, s);
		}
	} else {
		c->last_state = s;
	}
	return s;
}

/*!
 * Ask the backend whether the session is locked WITHOUT holding the mutex (GNOME:
 * a D-Bus call of up to 250 ms), then store the answer in the snapshot the state
 * machine reads. Call with the mutex released; returns the answer.
 */
static bool
probe_locked(struct u_desktop_control *c)
{
	if (c->b == NULL || c->b->is_locked == NULL) {
		return false;
	}
	os_mutex_lock(&c->mutex);
	uint64_t ticket = ++c->locked_seq_issued;
	os_mutex_unlock(&c->mutex);

	bool locked = c->b->is_locked(c->b);

	os_mutex_lock(&c->mutex);
	if (ticket > c->locked_seq_stored) {
		c->locked_seq_stored = ticket;
		c->locked_snapshot = locked;
	}
	os_mutex_unlock(&c->mutex);
	return locked;
}

static enum u_desktop_control_state
update_state(struct u_desktop_control *c, enum reason *out_reason)
{
	probe_locked(c);
	os_mutex_lock(&c->mutex);
	enum u_desktop_control_state s = update_state_locked(c, out_reason, false);
	os_mutex_unlock(&c->mutex);
	return s;
}

//! Result + message for an action/observation refused by the state (§5 step 1).
static int32_t
refusal_for_state(enum u_desktop_control_state s, enum reason r, char *msg, size_t cap)
{
	switch (r) {
	case R_STORE_UNREADABLE:
		snprintf(msg, cap, "the desktop-control settings file is unreadable");
		return U_DESKTOP_XR_ERROR_REFUSED;
	case R_KILL_SWITCH: snprintf(msg, cap, "kill switch"); return U_DESKTOP_XR_ERROR_REFUSED;
	case R_LOCKED: snprintf(msg, cap, "screen locked"); return U_DESKTOP_XR_ERROR_REFUSED;
	case R_STORE_DENY: snprintf(msg, cap, "disabled by the user"); return U_DESKTOP_XR_ERROR_REFUSED;
	default:
		snprintf(msg, cap, "desktop control is not granted (state %s)", u_desktop_control_state_str(s));
		return U_DESKTOP_XR_ERROR_NOT_GRANTED;
	}
}


/*
 *
 * Consent request.
 *
 */

static void
audit_consent(struct u_desktop_control *c, const char *what, const char *reason)
{
	struct u_desktop_audit_record r;
	memset(&r, 0, sizeof(r));
	r.exe = c->exe;
	r.action = what;
	r.chars = -1;
	r.reason = reason;
	r.result = "ok";
	u_desktop_audit_append(&r);
}

static void
request_ctx_unref(struct request_ctx *rq)
{
	os_mutex_lock(&rq->mutex);
	bool last = --rq->refs == 0;
	os_mutex_unlock(&rq->mutex);
	if (last) {
		os_mutex_destroy(&rq->mutex);
		free(rq);
	}
}

static void
request_done_locked(struct u_desktop_control *c, enum u_desktop_access result);

static void
request_done(void *ctx, enum u_desktop_access result)
{
	struct request_ctx *rq = (struct request_ctx *)ctx;
	// Held across the handling: destroy clears rq->c under this mutex, so it
	// waits for an answer being handled and every later answer sees NULL.
	os_mutex_lock(&rq->mutex);
	struct u_desktop_control *c = rq->c;
	if (c != NULL) {
		os_mutex_lock(&c->mutex);
		request_done_locked(c, result);
		os_mutex_unlock(&c->mutex);
	}
	os_mutex_unlock(&rq->mutex);
	request_ctx_unref(rq); // the in-flight request's reference
}

static void
request_done_locked(struct u_desktop_control *c, enum u_desktop_access result)
{
	c->pending = false;
	if (c->dying || c->stop) {
		// Owed answer delivered from the backend's destroy: nobody is asking.
		return;
	}
	c->req_result = result;
	if (c->b != NULL && c->b->remembers_in_store &&
	    (result == U_DESKTOP_ACCESS_GRANTED || result == U_DESKTOP_ACCESS_DENIED)) {
		// Windows: DisplayXR's own prompt — the answer is remembered in the store.
		bool ok = u_desktop_policy_set_app(
		    c->exe, result == U_DESKTOP_ACCESS_GRANTED ? U_DESKTOP_APP_ALLOW : U_DESKTOP_APP_DENY, "prompt");
		c->store_write_failed = !ok;
		if (!ok) {
			U_LOG_W("desktop_control: could not persist the consent answer in the store");
		}
	}
	audit_consent(c, result == U_DESKTOP_ACCESS_GRANTED ? "granted" : "denied", NULL);
	U_LOG_W("desktop_control: consent %s for %s", result == U_DESKTOP_ACCESS_GRANTED ? "granted" : "denied",
	        c->name);
	// Always announce the outcome, even when it equals the previous state.
	update_state_locked(c, NULL, true);
}

int32_t
u_desktop_control_request(struct u_desktop_control *c, const char *reason)
{
	probe_locked(c); // row 5 needs a fresh answer: a locked session is never prompted
	os_mutex_lock(&c->mutex);
	enum reason why;
	enum u_desktop_control_state s = evaluate_locked(c, &why);

	// Rows 1–5: resolved without prompting; announce the blocking state.
	if (why == R_NO_BACKEND || why == R_ENV_OFF || why == R_STORE_UNREADABLE || why == R_KILL_SWITCH ||
	    why == R_STORE_DENY || why == R_LOCKED) {
		update_state_locked(c, NULL, true);
		os_mutex_unlock(&c->mutex);
		return U_DESKTOP_XR_SUCCESS;
	}
	// A prompt is already up: never stack a second one.
	if (s == U_DESKTOP_STATE_PENDING) {
		os_mutex_unlock(&c->mutex);
		return U_DESKTOP_XR_SUCCESS;
	}

	audit_consent(c, "request", reason);
	c->requested = true;
	c->revoked = false; // a new request on a user gesture re-asks the OS

	// Remembered grant: no prompt (row 8).
	if (c->b->remembers_in_store && u_desktop_policy_app(c->policy, c->exe) == U_DESKTOP_APP_ALLOW) {
		update_state_locked(c, NULL, true);
		os_mutex_unlock(&c->mutex);
		return U_DESKTOP_XR_SUCCESS;
	}

	c->pending = true;
	update_state_locked(c, NULL, true);
	struct u_desktop_backend *b = c->b;
	struct request_ctx *rq = c->rq;
	os_mutex_unlock(&c->mutex);
	// After releasing c->mutex: request_done takes rq->mutex THEN c->mutex.
	os_mutex_lock(&rq->mutex);
	rq->refs++; // the in-flight request's reference, dropped by request_done
	os_mutex_unlock(&rq->mutex);

	struct u_desktop_app_info app = {c->exe, c->name, reason};
	b->request_access(b, &app, request_done, rq); // may call request_done before returning
	return U_DESKTOP_XR_SUCCESS;
}


/*
 *
 * Observation (§4).
 *
 */

static int32_t
observe_gate(struct u_desktop_control *c, uint32_t cap_bit)
{
	if (update_state(c, NULL) != U_DESKTOP_STATE_GRANTED) {
		return U_DESKTOP_XR_ERROR_NOT_GRANTED;
	}
	if ((c->b->caps & cap_bit) == 0) {
		return U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED;
	}
	return U_DESKTOP_XR_SUCCESS;
}

int32_t
u_desktop_control_list_windows(struct u_desktop_control *c, struct u_desktop_window *out, uint32_t cap, uint32_t *count)
{
	int32_t g = observe_gate(c, U_DESKTOP_CAP_LIST_WINDOWS);
	if (g != U_DESKTOP_XR_SUCCESS) {
		return g;
	}
	*count = 0;
	return map_backend_result(c->b->list_windows(c->b, out, cap, count), NULL, 0);
}

int32_t
u_desktop_control_get_pointer(struct u_desktop_control *c, struct u_desktop_pick *out)
{
	int32_t g = observe_gate(c, U_DESKTOP_CAP_POINTER_POSITION);
	if (g != U_DESKTOP_XR_SUCCESS) {
		return g;
	}
	int32_t x = 0, y = 0;
	enum u_desktop_result r = c->b->get_pointer(c->b, &x, &y);
	if (r != U_DESKTOP_OK) {
		return map_backend_result(r, NULL, 0);
	}
	memset(out, 0, sizeof(*out));
	out->x = x;
	out->y = y;
	if ((c->b->caps & U_DESKTOP_CAP_PICK) == 0) {
		// Position only: say "outside" rather than invent a window.
		out->kind = U_DESKTOP_PICK_OUTSIDE;
		return U_DESKTOP_XR_SUCCESS;
	}
	return map_backend_result(c->b->pick(c->b, x, y, out), NULL, 0);
}

int32_t
u_desktop_control_pick(struct u_desktop_control *c, int32_t x, int32_t y, struct u_desktop_pick *out)
{
	int32_t g = observe_gate(c, U_DESKTOP_CAP_PICK);
	if (g != U_DESKTOP_XR_SUCCESS) {
		return g;
	}
	memset(out, 0, sizeof(*out));
	return map_backend_result(c->b->pick(c->b, x, y, out), NULL, 0);
}

int32_t
u_desktop_control_text_at(struct u_desktop_control *c, int32_t x, int32_t y, char *buf, size_t cap)
{
	if (buf == NULL || cap == 0) {
		return U_DESKTOP_XR_ERROR_VALIDATION_FAILURE;
	}
	buf[0] = '\0';
	int32_t g = observe_gate(c, U_DESKTOP_CAP_READ_TEXT);
	if (g != U_DESKTOP_XR_SUCCESS) {
		return g;
	}
	char msg[MSG_CAP] = {0};
	int32_t r = map_backend_result(c->b->text_at(c->b, x, y, buf, cap, msg), NULL, 0);
	buf[cap - 1] = '\0';
	if (r != U_DESKTOP_XR_SUCCESS) {
		buf[0] = '\0';
		return r;
	}
	// A backend that filled the whole buffer may have cut a code point in half.
	size_t len = strlen(buf);
	buf[u_desktop_utf8_trim_partial(buf, len)] = '\0';
	return r;
}

enum u_desktop_control_state
u_desktop_control_get_state(struct u_desktop_control *c, uint32_t *out_caps)
{
	enum u_desktop_control_state s = update_state(c, NULL);
	if (out_caps != NULL) {
		*out_caps = c->b != NULL ? c->b->caps : 0;
	}
	return s;
}


/*
 *
 * Validation + queue (§4).
 *
 */

bool
u_desktop_control_validate(const struct u_desktop_action *a, char *why, size_t why_cap)
{
	char dummy[8];
	if (why == NULL || why_cap == 0) {
		why = dummy;
		why_cap = sizeof(dummy);
	}
	why[0] = '\0';
	if (a == NULL) {
		snprintf(why, why_cap, "no action");
		return false;
	}
	if (action_name(a->type) == NULL) {
		snprintf(why, why_cap, "unknown actionType %u", a->type);
		return false;
	}
	if (a->target_window_id == 0) {
		snprintf(why, why_cap, "targetWindowId is required");
		return false;
	}
	// Coordinates far outside any desktop are a bug (or an overflow waiting to
	// happen in a backend's arithmetic): reject them for every action type.
	const int32_t L = U_DESKTOP_CONTROL_MAX_COORD;
	if (a->x < -L || a->x > L || a->y < -L || a->y > L || a->x2 < -L || a->x2 > L || a->y2 < -L || a->y2 > L ||
	    a->rect.x < -L || a->rect.x > L || a->rect.y < -L || a->rect.y > L || a->rect.w > L || a->rect.h > L) {
		snprintf(why, why_cap, "coordinates are limited to +/-%d", L);
		return false;
	}
	switch (a->type) {
	case U_DESKTOP_ACTION_MOVE_RESIZE:
		if (a->rect.w < 0 || a->rect.h < 0) {
			snprintf(why, why_cap, "rect width/height must be >= 0 (0 = keep size)");
			return false;
		}
		break;
	case U_DESKTOP_ACTION_TYPE_TEXT: {
		if (a->text == NULL || a->text[0] == '\0') {
			snprintf(why, why_cap, "text is required");
			return false;
		}
		size_t len = strlen(a->text);
		if (len > U_DESKTOP_CONTROL_MAX_TEXT_BYTES) {
			snprintf(why, why_cap, "text is longer than %u bytes", U_DESKTOP_CONTROL_MAX_TEXT_BYTES);
			return false;
		}
		if (utf8_count(a->text) < 0) {
			snprintf(why, why_cap, "text is not valid UTF-8");
			return false;
		}
		break;
	}
	case U_DESKTOP_ACTION_KEY_CHORD: {
		struct u_desktop_chord ch;
		if (a->keys == NULL || a->keys[0] == '\0') {
			snprintf(why, why_cap, "keys is required");
			return false;
		}
		if (!u_desktop_chord_parse(a->keys, &ch)) {
			// Never echo the input: a chord can be a character the user typed.
			snprintf(why, why_cap, "keys is not a valid chord");
			return false;
		}
		break;
	}
	case U_DESKTOP_ACTION_CLICK:
		if (a->click_count > U_DESKTOP_CONTROL_MAX_CLICKS) {
			snprintf(why, why_cap, "clickCount must be 0..%u", U_DESKTOP_CONTROL_MAX_CLICKS);
			return false;
		}
		// fallthrough
	case U_DESKTOP_ACTION_DRAG:
		if (a->button > U_DESKTOP_BUTTON_MIDDLE) {
			snprintf(why, why_cap, "unknown button %u", a->button);
			return false;
		}
		break;
	case U_DESKTOP_ACTION_SCROLL:
		if (a->scroll_x == 0 && a->scroll_y == 0) {
			snprintf(why, why_cap, "scrollX or scrollY is required");
			return false;
		}
		if (a->scroll_x > U_DESKTOP_CONTROL_MAX_SCROLL || a->scroll_x < -U_DESKTOP_CONTROL_MAX_SCROLL ||
		    a->scroll_y > U_DESKTOP_CONTROL_MAX_SCROLL || a->scroll_y < -U_DESKTOP_CONTROL_MAX_SCROLL) {
			snprintf(why, why_cap, "scroll is limited to %d notches", U_DESKTOP_CONTROL_MAX_SCROLL);
			return false;
		}
		break;
	default: break;
	}
	return true;
}

static void
free_queued(struct queued_action *qa)
{
	free(qa->text);
	free(qa->keys);
	qa->text = NULL;
	qa->keys = NULL;
}

int32_t
u_desktop_control_submit(struct u_desktop_control *c, const struct u_desktop_action *a, uint64_t *out_id)
{
	if (out_id != NULL) {
		*out_id = 0;
	}
	enum u_desktop_control_state st = update_state(c, NULL);
	if (st != U_DESKTOP_STATE_GRANTED) {
		// An attempt while suspended / disabled / not granted is what a user reading
		// the audit log most wants to see (spec §5 step 6) — once per state.
		os_mutex_lock(&c->mutex);
		bool first = c->refused_audit_state_plus1 != (int)st + 1;
		c->refused_audit_state_plus1 = (int)st + 1;
		os_mutex_unlock(&c->mutex);
		if (first) {
			struct u_desktop_audit_record r;
			memset(&r, 0, sizeof(r));
			r.exe = c->exe;
			const char *name = a != NULL ? action_name(a->type) : NULL;
			r.action = name != NULL ? name : "unknown"; // never drop the record
			r.chars = -1;
			r.has_target = a != NULL;
			r.target_id = a != NULL ? a->target_window_id : 0;
			r.result = "not_granted";
			r.message = u_desktop_control_state_str(st);
			u_desktop_audit_append(&r);
		}
		return U_DESKTOP_XR_ERROR_NOT_GRANTED;
	}
	os_mutex_lock(&c->mutex);
	c->refused_audit_state_plus1 = 0;
	os_mutex_unlock(&c->mutex);
	if (!u_desktop_control_validate(a, NULL, 0)) {
		if (a != NULL && action_name(a->type) == NULL) {
			// An action type this runtime does not know: app-controlled garbage, but
			// a user reading the audit log should still see the attempt (once per
			// submit, like every action).
			struct u_desktop_audit_record r;
			memset(&r, 0, sizeof(r));
			r.exe = c->exe;
			r.action = "unknown";
			r.chars = -1;
			r.has_target = true;
			r.target_id = a->target_window_id;
			r.result = result_str(U_DESKTOP_XR_ERROR_VALIDATION_FAILURE);
			r.message = "unknown actionType";
			u_desktop_audit_append(&r);
		}
		return U_DESKTOP_XR_ERROR_VALIDATION_FAILURE;
	}

	struct queued_action qa;
	memset(&qa, 0, sizeof(qa));
	qa.a = *a;
	qa.a.text = NULL;
	qa.a.keys = NULL;
	if (a->type == U_DESKTOP_ACTION_TYPE_TEXT) {
		size_t len = strlen(a->text);
		qa.text = (char *)malloc(len + 1);
		if (qa.text == NULL) {
			return U_DESKTOP_XR_ERROR_RUNTIME_FAILURE;
		}
		memcpy(qa.text, a->text, len + 1);
		qa.a.text = qa.text;
	}
	if (a->type == U_DESKTOP_ACTION_KEY_CHORD) {
		size_t len = strlen(a->keys);
		qa.keys = (char *)malloc(len + 1);
		if (qa.keys == NULL) {
			return U_DESKTOP_XR_ERROR_RUNTIME_FAILURE;
		}
		memcpy(qa.keys, a->keys, len + 1);
		qa.a.keys = qa.keys;
		u_desktop_chord_parse(qa.keys, &qa.chord);
	}
	if (a->type == U_DESKTOP_ACTION_CLICK && qa.a.click_count == 0) {
		qa.a.click_count = 1;
	}

	os_mutex_lock(&c->mutex);
	if (c->stop) {
		os_mutex_unlock(&c->mutex);
		free_queued(&qa);
		return U_DESKTOP_XR_ERROR_RUNTIME_FAILURE;
	}
	if (c->q_count >= U_DESKTOP_CONTROL_QUEUE_SIZE) {
		os_mutex_unlock(&c->mutex);
		free_queued(&qa);
		return U_DESKTOP_XR_ERROR_LIMIT_REACHED;
	}
	qa.id = ++c->next_id;
	c->q[(c->q_head + c->q_count) % U_DESKTOP_CONTROL_QUEUE_SIZE] = qa;
	c->q_count++;
	os_cond_broadcast(&c->cond);
	os_mutex_unlock(&c->mutex);

	if (out_id != NULL) {
		*out_id = qa.id;
	}
	return U_DESKTOP_XR_SUCCESS;
}


/*
 *
 * The safety layer (§5) — worker thread.
 *
 */

size_t
u_desktop_utf8_trim_partial(const char *s, size_t len)
{
	if (s == NULL || len == 0) {
		return 0;
	}
	// Walk back over continuation bytes to the lead byte of the last sequence.
	size_t i = len;
	size_t cont = 0;
	while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80 && cont < 3) {
		i--;
		cont++;
	}
	if (i == 0) {
		return 0; // only continuation bytes: nothing complete
	}
	unsigned char lead = (unsigned char)s[i - 1];
	size_t need;
	if (lead < 0x80) {
		need = 0;
	} else if ((lead & 0xE0) == 0xC0) {
		need = 1;
	} else if ((lead & 0xF0) == 0xE0) {
		need = 2;
	} else if ((lead & 0xF8) == 0xF0) {
		need = 3;
	} else {
		return i - 1; // a stray byte: drop it and what follows
	}
	return cont >= need ? len : i - 1;
}

static bool
stopping(struct u_desktop_control *c)
{
	os_mutex_lock(&c->mutex);
	bool s = c->stop;
	os_mutex_unlock(&c->mutex);
	return s;
}

static uint64_t
injection_begin(void)
{
	return os_monotonic_get_ns();
}

//! Record one of our injections (any backend action call, window ops included).
static void
injection_end(struct u_desktop_control *c, uint64_t start_ns)
{
	uint32_t i = c->inj_next;
	c->inj[i].start_ns = start_ns;
	c->inj[i].end_ns = os_monotonic_get_ns();
	c->inj_next = (i + 1) % U_DESKTOP_CONTROL_INJECTION_HISTORY;
	if (c->inj_count < U_DESKTOP_CONTROL_INJECTION_HISTORY) {
		c->inj_count++;
	}
}

/*!
 * True when input with OS timestamp @p ts_ns falls inside one of our injection
 * windows [start, end + grace]; @p out_closes_ns is when that window closes.
 */
static bool
input_is_ours(struct u_desktop_control *c, uint64_t ts_ns, uint64_t *out_closes_ns)
{
	// The OS reports idle time in whole ms: allow 1 ms of rounding at the start.
	for (uint32_t k = 0; k < c->inj_count; k++) {
		uint64_t start = c->inj[k].start_ns > NS_PER_MS ? c->inj[k].start_ns - NS_PER_MS : 0;
		uint64_t end = c->inj[k].end_ns + c->timing.own_grace_ns;
		if (ts_ns >= start && ts_ns <= end) {
			*out_closes_ns = end;
			return true;
		}
	}
	return false;
}

//! The backend took the permission back: DENIED (event), no more backend calls.
static void
mark_revoked(struct u_desktop_control *c)
{
	os_mutex_lock(&c->mutex);
	if (!c->revoked) {
		c->revoked = true;
		U_LOG_W("desktop_control: the OS revoked desktop control for %s", c->name);
	}
	update_state_locked(c, NULL, false);
	os_mutex_unlock(&c->mutex);
}

enum step_outcome
{
	STEP_OK,
	STEP_FAIL,  //!< result + message set
	STEP_ABORT, //!< the object is being destroyed: no event
};

//! §5 step 1, re-run at every poll of the idle wait too.
static enum step_outcome
check_state(struct u_desktop_control *c, int32_t *result, char *msg)
{
	enum reason why;
	enum u_desktop_control_state s = update_state(c, &why);
	if (s == U_DESKTOP_STATE_GRANTED) {
		return STEP_OK;
	}
	*result = refusal_for_state(s, why, msg, MSG_CAP);
	return STEP_FAIL;
}

//! §5 step 2: wait until the user has been idle for idle_ms (our own input excluded).
static enum step_outcome
wait_for_idle(struct u_desktop_control *c, int32_t *result, char *msg)
{
	uint64_t start = os_monotonic_get_ns();
	for (;;) {
		if (stopping(c)) {
			return STEP_ABORT;
		}
		enum step_outcome so = check_state(c, result, msg);
		if (so != STEP_OK) {
			return so;
		}

		os_mutex_lock(&c->mutex);
		uint64_t need_ms = u_desktop_policy_idle_ms(c->policy);
		os_mutex_unlock(&c->mutex);
		if (need_ms == 0) {
			return STEP_OK;
		}

		uint64_t idle_ms = 0;
		if (c->b->user_idle_ms == NULL || !c->b->user_idle_ms(c->b, &idle_ms)) {
			return STEP_OK; // unknown: cannot wait on what we cannot see
		}
		uint64_t now = os_monotonic_get_ns();
		uint64_t idle_ns = idle_ms * NS_PER_MS;
		uint64_t last_input_ns = now > idle_ns ? now - idle_ns : 0;
		if (idle_ms >= need_ms) {
			return STEP_OK;
		}
		// The latest input is inside one of our injection windows. Decide only
		// once that window has CLOSED: until then a user keystroke in the grace
		// part is indistinguishable from ours, and accepting it at once would let
		// back-to-back actions chain past a user who keeps typing. After the close,
		// a still-active user shows a newer timestamp (outside the window).
		uint64_t closes_ns = 0;
		if (input_is_ours(c, last_input_ns, &closes_ns) && now > closes_ns) {
			return STEP_OK;
		}
		if (now - start >= c->timing.idle_max_wait_ns) {
			*result = U_DESKTOP_XR_ERROR_REFUSED;
			snprintf(msg, MSG_CAP, "the user is active");
			return STEP_FAIL;
		}

		uint64_t left = c->timing.idle_max_wait_ns - (now - start);
		uint64_t wait = c->timing.idle_poll_ns < left ? c->timing.idle_poll_ns : left;
		os_mutex_lock(&c->mutex);
		if (!c->stop) {
			os_cond_wait_timeout_ns(&c->cond, &c->mutex, wait);
		}
		os_mutex_unlock(&c->mutex);
	}
}

//! Window list into the worker's buffer. Returns the count, or a negative XR result.
static int32_t
refresh_window_list(struct u_desktop_control *c, uint32_t *out_count, char *msg)
{
	for (int attempt = 0; attempt < 3; attempt++) {
		uint32_t count = 0;
		enum u_desktop_result r = c->b->list_windows(c->b, c->list_buf, c->list_cap, &count);
		if (r != U_DESKTOP_OK) {
			return map_backend_result(r, msg, MSG_CAP);
		}
		if (count <= c->list_cap) {
			*out_count = count;
			return U_DESKTOP_XR_SUCCESS;
		}
		struct u_desktop_window *nb =
		    (struct u_desktop_window *)realloc(c->list_buf, sizeof(struct u_desktop_window) * (count + 16));
		if (nb == NULL) {
			return U_DESKTOP_XR_ERROR_RUNTIME_FAILURE;
		}
		c->list_buf = nb;
		c->list_cap = count + 16;
	}
	*out_count = c->list_cap;
	return U_DESKTOP_XR_SUCCESS;
}

static const struct u_desktop_window *
find_window(struct u_desktop_control *c, uint32_t count, uint64_t id)
{
	for (uint32_t i = 0; i < count; i++) {
		if (c->list_buf[i].id == id) {
			return &c->list_buf[i];
		}
	}
	return NULL;
}

//! §5 step 4, pointer half: the pick at (x, y) is the target or its dialog.
static enum step_outcome
verify_pointer_target(struct u_desktop_control *c, uint64_t target, int32_t x, int32_t y, int32_t *result, char *msg)
{
	if ((c->b->caps & U_DESKTOP_CAP_PICK) == 0 || c->b->pick == NULL) {
		*result = U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED;
		snprintf(msg, MSG_CAP, "cannot verify the target: this desktop backend has no hit test");
		return STEP_FAIL;
	}
	struct u_desktop_pick p;
	memset(&p, 0, sizeof(p));
	enum u_desktop_result r = c->b->pick(c->b, x, y, &p);
	if (r != U_DESKTOP_OK) {
		*result = map_backend_result(r, msg, MSG_CAP);
		return STEP_FAIL;
	}
	if (p.kind == U_DESKTOP_PICK_WINDOW && (p.window.id == target || p.window.parent_id == target)) {
		return STEP_OK;
	}
	*result = U_DESKTOP_XR_ERROR_REFUSED;
	switch (p.kind) {
	case U_DESKTOP_PICK_WINDOW:
		snprintf(msg, MSG_CAP, "target is not under the point (found %.60s: %.120s)", p.window.app_id,
		         p.window.title);
		break;
	case U_DESKTOP_PICK_DESKTOP: snprintf(msg, MSG_CAP, "target is not under the point (found the desktop)"); break;
	case U_DESKTOP_PICK_SHELL: snprintf(msg, MSG_CAP, "target is not under the point (found the shell)"); break;
	default: snprintf(msg, MSG_CAP, "target is not under the point (found nothing: outside every monitor)"); break;
	}
	return STEP_FAIL;
}

//! §5 step 4, keyboard half: the focused window is the target or its dialog.
static enum step_outcome
verify_keyboard_target(struct u_desktop_control *c, uint64_t target, uint32_t count, int32_t *result, char *msg)
{
	uint64_t focused = 0;
	enum u_desktop_result r =
	    c->b->focused_window != NULL ? c->b->focused_window(c->b, &focused) : U_DESKTOP_ERR_UNSUPPORTED;
	if (r == U_DESKTOP_ERR_UNSUPPORTED) {
		*result = U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED;
		snprintf(msg, MSG_CAP, "cannot verify the target: this desktop backend cannot report keyboard focus");
		return STEP_FAIL;
	}
	if (r != U_DESKTOP_OK) {
		*result = map_backend_result(r, msg, MSG_CAP);
		return STEP_FAIL;
	}
	if (focused != 0 && focused == target) {
		return STEP_OK;
	}
	if (focused != 0) {
		const struct u_desktop_window *fw = find_window(c, count, focused);
		if (fw != NULL && fw->parent_id == target) {
			return STEP_OK;
		}
	}
	*result = U_DESKTOP_XR_ERROR_REFUSED;
	snprintf(msg, MSG_CAP, "target window does not have focus %s ACTIVATE it first", k_em_dash);
	return STEP_FAIL;
}


/*
 *
 * The input guard (u_desktop.h "Delivery guarantee").
 *
 */

struct guard_ctx
{
	struct u_desktop_control *c;
	bool stopped;    //!< still_allowed returned false at least once
	enum reason why; //!< ... and why
	uint32_t calls;  //!< how often the backend asked (tests / diagnostics)
};

//! Kill switch (store stamp), unreadable store, deny, lock, revocation, destroy.
static bool
guard_still_allowed(void *ctx)
{
	struct guard_ctx *g = (struct guard_ctx *)ctx;
	struct u_desktop_control *c = g->c;
	g->calls++;
	// The lock query first, with the mutex released (it can be a D-Bus round trip):
	// probe_locked stores the answer for the state machine below.
	bool locked = probe_locked(c);
	os_mutex_lock(&c->mutex);
	enum reason why = R_OS_GRANTED;
	bool ok = true;
	if (c->stop) {
		ok = false;
		why = R_NOT_REQUESTED;
	} else {
		u_desktop_policy_refresh(c->policy);
		if (u_desktop_policy_is_unreadable(c->policy)) {
			ok = false;
			why = R_STORE_UNREADABLE;
		} else if (u_desktop_policy_is_suspended(c->policy)) {
			ok = false;
			why = R_KILL_SWITCH;
		} else if (u_desktop_policy_app(c->policy, c->exe) == U_DESKTOP_APP_DENY) {
			ok = false;
			why = R_STORE_DENY;
		} else if (locked) {
			ok = false;
			why = R_LOCKED;
		} else if (c->revoked) {
			ok = false;
			why = R_REVOKED;
		}
	}
	if (!ok) {
		if (!g->stopped) {
			g->stopped = true;
			g->why = why;
		}
		update_state_locked(c, NULL, false); // announce the change mid-action
	}
	os_mutex_unlock(&c->mutex);
	return ok;
}

/*!
 * Result of an injecting backend call, with a message the common layer chose.
 * For text and chords a backend's own message is never passed on (it could name
 * a character): every outcome maps to a fixed string. A guard stop always reads
 * as its reason ("kill switch", "screen locked", ...).
 */
static int32_t
injection_result(
    struct u_desktop_control *c, const struct guard_ctx *g, uint32_t type, enum u_desktop_result r, char *msg)
{
	if (r == U_DESKTOP_OK) {
		return U_DESKTOP_XR_SUCCESS;
	}
	if (r == U_DESKTOP_ERR_NOT_GRANTED) {
		mark_revoked(c);
	}
	if (g != NULL && g->stopped && r == U_DESKTOP_ERR_REFUSED) {
		if (g->why == R_NOT_REQUESTED) {
			snprintf(msg, MSG_CAP, "the session is ending");
			return U_DESKTOP_XR_ERROR_REFUSED;
		}
		if (g->why == R_REVOKED) {
			snprintf(msg, MSG_CAP, "the OS refused: permission missing or revoked");
			return U_DESKTOP_XR_ERROR_NOT_GRANTED;
		}
		return refusal_for_state(U_DESKTOP_STATE_SUSPENDED, g->why, msg, MSG_CAP);
	}
	if (is_keyboard_action(type)) {
		const char *fixed;
		int32_t x;
		switch (r) {
		case U_DESKTOP_ERR_REFUSED:
			fixed = "focus moved";
			x = U_DESKTOP_XR_ERROR_REFUSED;
			break;
		case U_DESKTOP_ERR_NOT_GRANTED:
			fixed = "the OS refused: permission missing or revoked";
			x = U_DESKTOP_XR_ERROR_NOT_GRANTED;
			break;
		case U_DESKTOP_ERR_UNSUPPORTED:
			fixed = "this desktop backend cannot deliver this input";
			x = U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED;
			break;
		case U_DESKTOP_ERR_WINDOW_LOST:
			fixed = "the target window no longer exists";
			x = U_DESKTOP_XR_ERROR_WINDOW_LOST;
			break;
		default:
			fixed = "the input could not be delivered";
			x = U_DESKTOP_XR_ERROR_RUNTIME_FAILURE;
			break;
		}
		snprintf(msg, MSG_CAP, "%s", fixed);
		return x;
	}
	return map_backend_result(r, msg, MSG_CAP);
}

//! Move the pointer, then re-pick before anything is pressed (§5 step 4, last item).
static enum step_outcome
move_and_reverify(struct u_desktop_control *c, uint64_t target, int32_t x, int32_t y, int32_t *result, char *msg)
{
	uint64_t t0 = injection_begin();
	enum u_desktop_result r = c->b->pointer_move(c->b, x, y, msg);
	injection_end(c, t0);
	if (r != U_DESKTOP_OK) {
		*result = injection_result(c, NULL, U_DESKTOP_ACTION_POINTER_MOVE, r, msg);
		return STEP_FAIL;
	}
	if (stopping(c)) {
		return STEP_ABORT;
	}
	return verify_pointer_target(c, target, x, y, result, msg);
}

//! Release a button whatever else happened (never leave it held).
static void
release_button(struct u_desktop_control *c,
               const struct u_desktop_input_guard *guard,
               enum u_desktop_button button,
               int32_t x,
               int32_t y)
{
	char m[MSG_CAP] = {0};
	uint64_t t0 = injection_begin();
	c->b->pointer_button(c->b, guard, button, false, x, y, m);
	injection_end(c, t0);
}

//! §5 step 5. Called only after steps 1–4 passed.
static enum step_outcome
execute(struct u_desktop_control *c,
        const struct queued_action *qa,
        const struct u_desktop_input_guard *guard,
        const struct guard_ctx *g,
        int32_t *result,
        char *msg)
{
	const struct u_desktop_action *a = &qa->a;
	struct u_desktop_backend *b = c->b;
	enum u_desktop_result r = U_DESKTOP_OK;
	enum step_outcome so;
	enum u_desktop_button btn = (enum u_desktop_button)a->button;
	uint64_t t0;

	switch (a->type) {
	case U_DESKTOP_ACTION_ACTIVATE:
	case U_DESKTOP_ACTION_MOVE_RESIZE:
	case U_DESKTOP_ACTION_MINIMIZE:
	case U_DESKTOP_ACTION_RESTORE:
	case U_DESKTOP_ACTION_MAXIMIZE:
	case U_DESKTOP_ACTION_CLOSE:
		t0 = injection_begin();
		r = b->window_op(b, guard, (enum u_desktop_window_op)a->type, a->target_window_id,
		                 a->type == U_DESKTOP_ACTION_MOVE_RESIZE ? &a->rect : NULL, msg);
		injection_end(c, t0);
		break;
	case U_DESKTOP_ACTION_TYPE_TEXT:
		t0 = injection_begin();
		r = b->type_text(b, guard, a->text, msg);
		injection_end(c, t0);
		break;
	case U_DESKTOP_ACTION_KEY_CHORD:
		t0 = injection_begin();
		r = b->key_chord(b, guard, &qa->chord, msg);
		injection_end(c, t0);
		break;
	case U_DESKTOP_ACTION_POINTER_MOVE:
		t0 = injection_begin();
		r = b->pointer_move(b, a->x, a->y, msg);
		injection_end(c, t0);
		break;
	case U_DESKTOP_ACTION_CLICK:
		so = move_and_reverify(c, a->target_window_id, a->x, a->y, result, msg);
		if (so != STEP_OK) {
			return so;
		}
		for (uint32_t i = 0; i < a->click_count && r == U_DESKTOP_OK; i++) {
			t0 = injection_begin();
			r = b->pointer_button(b, guard, btn, true, a->x, a->y, msg);
			injection_end(c, t0);
			if (r != U_DESKTOP_OK) {
				break; // the press did not happen: nothing to release
			}
			t0 = injection_begin();
			r = b->pointer_button(b, guard, btn, false, a->x, a->y, msg);
			injection_end(c, t0);
		}
		break;
	case U_DESKTOP_ACTION_DRAG: {
		so = move_and_reverify(c, a->target_window_id, a->x, a->y, result, msg);
		if (so != STEP_OK) {
			return so;
		}
		t0 = injection_begin();
		r = b->pointer_button(b, guard, btn, true, a->x, a->y, msg);
		injection_end(c, t0);
		if (r != U_DESKTOP_OK) {
			break;
		}
		// The button is down: the kill switch / lock / deny may have arrived since the
		// press. Stopping here drops the drag where it started (a click, not a move).
		if (!guard->still_allowed(guard->ctx)) {
			release_button(c, guard, btn, a->x, a->y);
			r = U_DESKTOP_ERR_REFUSED;
			break;
		}
		t0 = injection_begin();
		enum u_desktop_result rm = b->pointer_move(b, a->x2, a->y2, msg);
		injection_end(c, t0);
		// Always release, whatever the move did (at the end point if it got there).
		release_button(c, guard, btn, rm == U_DESKTOP_OK ? a->x2 : a->x, rm == U_DESKTOP_OK ? a->y2 : a->y);
		r = rm;
		break;
	}
	case U_DESKTOP_ACTION_SCROLL:
		so = move_and_reverify(c, a->target_window_id, a->x, a->y, result, msg);
		if (so != STEP_OK) {
			return so;
		}
		t0 = injection_begin();
		r = b->scroll(b, guard, a->x, a->y, a->scroll_x, a->scroll_y, msg);
		injection_end(c, t0);
		break;
	default: r = U_DESKTOP_ERR_UNSUPPORTED; break;
	}

	*result = injection_result(c, g, a->type, r, msg);
	return *result == U_DESKTOP_XR_SUCCESS ? STEP_OK : STEP_FAIL;
}

//! A chord is a shortcut (loggable) only with ctrl or super held and NOT ctrl+alt
//! (AltGr): option/alt alone, shift, AltGr combinations and bare keys produce text.
static bool
chord_is_shortcut(const struct u_desktop_chord *ch)
{
	bool ctrl = (ch->mods & U_DESKTOP_MOD_CTRL) != 0;
	bool alt = (ch->mods & U_DESKTOP_MOD_ALT) != 0;
	bool super = (ch->mods & U_DESKTOP_MOD_SUPER) != 0;
	return (ctrl || super) && !(ctrl && alt);
}

static void
audit_action(struct u_desktop_control *c,
             const struct queued_action *qa,
             const struct u_desktop_window *target,
             int32_t result,
             const char *msg,
             bool aborted)
{
	const struct u_desktop_action *a = &qa->a;
	struct u_desktop_audit_record r;
	memset(&r, 0, sizeof(r));
	r.exe = c->exe;
	const char *name = action_name(a->type);
	r.action = name != NULL ? name : "unknown";
	r.has_target = true;
	r.target_id = a->target_window_id;
	if (target != NULL) {
		r.target_app = target->app_id;
		r.target_title = target->title;
	}
	if (is_pointer_action(a->type)) {
		r.has_point = true;
		r.x = a->x;
		r.y = a->y;
	}
	r.chars = a->type == U_DESKTOP_ACTION_TYPE_TEXT ? utf8_count(a->text) : -1;
	char keys[64];
	if (a->type == U_DESKTOP_ACTION_KEY_CHORD && chord_is_shortcut(&qa->chord) &&
	    u_desktop_chord_format(&qa->chord, keys, sizeof(keys))) {
		r.keys = keys;
	}
	r.result = aborted ? "aborted" : result_str(result);
	r.message = result == U_DESKTOP_XR_SUCCESS ? NULL : msg;
	u_desktop_audit_append(&r);
}

static void
run_action(struct u_desktop_control *c, const struct queued_action *qa)
{
	const struct u_desktop_action *a = &qa->a;
	int32_t result = U_DESKTOP_XR_SUCCESS;
	char msg[MSG_CAP] = {0};
	struct u_desktop_window target_copy;
	const struct u_desktop_window *target = NULL;
	enum step_outcome so;

	// 1. State.
	so = check_state(c, &result, msg);
	if (so != STEP_OK) {
		goto done;
	}

	// The backend must be able to do it at all.
	uint32_t need = is_window_action(a->type) ? U_DESKTOP_CAP_WINDOW_ACTIONS : U_DESKTOP_CAP_INPUT;
	if ((c->b->caps & need) == 0) {
		result = U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED;
		snprintf(msg, sizeof(msg), "this desktop backend cannot %s",
		         is_window_action(a->type) ? "act on windows" : "inject input");
		so = STEP_FAIL;
		goto done;
	}

	// Slow setup FIRST (GNOME: restoring the portal session, 0.3–9 s): before the
	// idle wait, so a user who starts typing during it is still seen by step 2, and
	// before verification, never between it and the injection.
	if (c->b->prepare_input != NULL) {
		char pmsg[MSG_CAP] = {0};
		enum u_desktop_result pr = c->b->prepare_input(c->b, pmsg);
		if (pr != U_DESKTOP_OK) {
			result = injection_result(c, NULL, a->type, pr, pmsg);
			snprintf(msg, sizeof(msg), "%s", pmsg); // fixed for text / chords (injection_result)
			so = STEP_FAIL;
			goto done;
		}
		if (stopping(c)) {
			so = STEP_ABORT;
			goto done;
		}
	}

	// 2. Never fight the user — EVERY action (window ops move focus, and on GNOME
	// they are input gestures). Re-runs step 1 at every poll.
	so = wait_for_idle(c, &result, msg);
	if (so != STEP_OK) {
		goto done;
	}

	// 3. Target exists.
	uint32_t count = 0;
	result = refresh_window_list(c, &count, msg);
	if (result != U_DESKTOP_XR_SUCCESS) {
		if (result == U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED) {
			snprintf(msg, sizeof(msg),
			         "cannot verify the target: this desktop backend cannot list windows");
		}
		so = STEP_FAIL;
		goto done;
	}
	const struct u_desktop_window *w = find_window(c, count, a->target_window_id);
	if (w == NULL) {
		result = U_DESKTOP_XR_ERROR_WINDOW_LOST;
		snprintf(msg, sizeof(msg), "the target window no longer exists");
		so = STEP_FAIL;
		goto done;
	}
	target_copy = *w;
	target = &target_copy;

	if (stopping(c)) {
		so = STEP_ABORT;
		goto done;
	}

	// 4. Target verification, immediately before delivery.
	if (is_pointer_action(a->type)) {
		so = verify_pointer_target(c, a->target_window_id, a->x, a->y, &result, msg);
	} else if (is_keyboard_action(a->type)) {
		so = verify_keyboard_target(c, a->target_window_id, count, &result, msg);
	} else {
		so = STEP_OK;
	}
	if (so != STEP_OK) {
		goto done;
	}

	// 5. Execute, under the input guard: the backend repeats the verification
	// and the kill-switch / lock check before every injection batch.
	struct guard_ctx g = {c, false, R_OS_GRANTED, 0};
	struct u_desktop_input_guard guard = {a->target_window_id, guard_still_allowed, &g};
	msg[0] = '\0';
	so = execute(c, qa, &guard, &g, &result, msg);

done:
	// 6. Audit — every action, whatever its result.
	audit_action(c, qa, target, result, msg, so == STEP_ABORT);
	if (so == STEP_ABORT) {
		return; // destroyed: the session's event queue is going away
	}
	if (result == U_DESKTOP_XR_SUCCESS) {
		snprintf(msg, sizeof(msg), "ok");
	}
	os_mutex_lock(&c->mutex);
	bool stop = c->stop;
	os_mutex_unlock(&c->mutex);
	if (!stop && c->cb.action_completed != NULL) {
		c->cb.action_completed(c->cb.ctx, qa->id, result, msg);
	}
}

static void *
worker(void *ptr)
{
	struct u_desktop_control *c = (struct u_desktop_control *)ptr;
	os_mutex_lock(&c->mutex);
	for (;;) {
		while (!c->stop && c->q_count == 0) {
			os_cond_wait(&c->cond, &c->mutex);
		}
		if (c->stop) {
			break;
		}
		struct queued_action qa = c->q[c->q_head];
		memset(&c->q[c->q_head], 0, sizeof(c->q[c->q_head]));
		c->q_head = (c->q_head + 1) % U_DESKTOP_CONTROL_QUEUE_SIZE;
		c->q_count--;
		os_mutex_unlock(&c->mutex);

		run_action(c, &qa);
		free_queued(&qa);

		os_mutex_lock(&c->mutex);
	}
	os_mutex_unlock(&c->mutex);
	return NULL;
}


/*
 *
 * Lifecycle.
 *
 */

int
u_desktop_control_create(struct u_desktop_backend *backend,
                         const struct u_desktop_app_info *app,
                         const struct u_desktop_control_callbacks *cb,
                         const struct u_desktop_control_timing *timing,
                         struct u_desktop_control **out)
{
	*out = NULL;
	struct u_desktop_control *c = (struct u_desktop_control *)calloc(1, sizeof(*c));
	struct request_ctx *rq = (struct request_ctx *)calloc(1, sizeof(*rq));
	if (c == NULL || rq == NULL) {
		free(c);
		free(rq);
		if (backend != NULL && backend->destroy != NULL) {
			backend->destroy(backend);
		}
		return -1;
	}
	os_mutex_init(&rq->mutex);
	rq->refs = 1; // the control's own reference
	rq->c = c;
	c->rq = rq;

	c->b = backend;
	const char *env = getenv("DXR_DESKTOP_CONTROL");
	c->env_off = env != NULL && strcmp(env, "0") == 0;
	if (app != NULL && app->exe_path != NULL) {
		snprintf(c->exe, sizeof(c->exe), "%s", app->exe_path);
	}
	if (app != NULL && app->display_name != NULL) {
		snprintf(c->name, sizeof(c->name), "%s", app->display_name);
	} else {
		u_desktop_display_name_from_exe(c->exe, c->name, sizeof(c->name));
	}
	if (cb != NULL) {
		c->cb = *cb;
	}
	c->timing.idle_poll_ns = timing != NULL && timing->idle_poll_ns != 0 ? timing->idle_poll_ns : DEFAULT_POLL_NS;
	c->timing.own_grace_ns = timing != NULL && timing->own_grace_ns != 0 ? timing->own_grace_ns : DEFAULT_GRACE_NS;
	c->timing.idle_max_wait_ns =
	    timing != NULL && timing->idle_max_wait_ns != 0 ? timing->idle_max_wait_ns : DEFAULT_MAX_WAIT_NS;
	c->req_result = U_DESKTOP_ACCESS_UNKNOWN;
	c->policy = u_desktop_policy_create();
	c->list_cap = 64;
	c->list_buf = (struct u_desktop_window *)calloc(c->list_cap, sizeof(struct u_desktop_window));

	os_mutex_init(&c->mutex);
	os_cond_init(&c->cond);

	// Initial state: no event (nothing has changed yet from the app's view).
	os_mutex_lock(&c->mutex);
	c->last_state = evaluate_locked(c, NULL);
	os_mutex_unlock(&c->mutex);

	os_thread_init(&c->thread);
	if (os_thread_start(&c->thread, worker, c) == 0) {
		c->thread_started = true;
		os_thread_name(&c->thread, "dxr: desktop");
	}

	U_LOG_I("desktop_control: created for %s (backend %s, state %s)", c->name, c->b != NULL ? c->b->name : "none",
	        u_desktop_control_state_str(c->last_state));
	*out = c;
	return 0;
}

void
u_desktop_control_destroy(struct u_desktop_control **c_ptr)
{
	if (c_ptr == NULL || *c_ptr == NULL) {
		return;
	}
	struct u_desktop_control *c = *c_ptr;
	*c_ptr = NULL;

	os_mutex_lock(&c->mutex);
	c->stop = true;
	// Drop queued actions without events (spec §4).
	while (c->q_count > 0) {
		free_queued(&c->q[c->q_head]);
		c->q_head = (c->q_head + 1) % U_DESKTOP_CONTROL_QUEUE_SIZE;
		c->q_count--;
	}
	os_cond_broadcast(&c->cond);
	os_mutex_unlock(&c->mutex);

	// An action mid-flight finishes its current backend call first (its guard
	// now says no, so a well-behaved backend stops at the next batch).
	if (c->thread_started) {
		os_thread_join(&c->thread);
	}
	os_thread_destroy(&c->thread);

	// From here on a consent answer is ignored ...
	os_mutex_lock(&c->mutex);
	c->dying = true;
	os_mutex_unlock(&c->mutex);

	// ... and the backend's destroy must deliver (or cancel) any owed answer
	// before it returns (u_desktop.h "Lifetime").
	if (c->b != NULL && c->b->destroy != NULL) {
		c->b->destroy(c->b);
	}
	c->b = NULL;

	// Belt and braces for a backend that breaks that contract: detach the
	// request context (waits for an answer being handled right now), so a later
	// answer finds no control instead of freed memory.
	struct request_ctx *rq = c->rq;
	os_mutex_lock(&rq->mutex);
	rq->c = NULL;
	os_mutex_unlock(&rq->mutex);
	request_ctx_unref(rq);

	u_desktop_policy_destroy(&c->policy);
	os_cond_destroy(&c->cond);
	os_mutex_destroy(&c->mutex);
	free(c->list_buf);
	free(c);
}
