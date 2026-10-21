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
#include "u_desktop_capture.h"
#include "u_desktop_chord.h"
#include "u_desktop_gnome_logic.h"
#include "u_desktop_policy.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include <ctype.h>
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

/*!
 * One request in flight: the context handed to the backend's request_access /
 * request_scopes. Holds a reference on the request_ctx and remembers which scopes
 * THIS request asked for (two requests for different scopes may overlap).
 */
struct request_ticket
{
	struct request_ctx *rq;
	uint32_t asked; //!< enum u_desktop_scope bits
};

//! Indices into u_desktop_control::sc.
enum
{
	SC_CONTROL = 0,
	SC_CAPTURE = 1,
	SC_COUNT = 2,
};

//! One consent scope's request bookkeeping (spec v2 §10.2: each scope has its own state).
struct scope_state
{
	bool requested;
	bool pending;
	bool revoked; //!< the backend reported NOT_GRANTED: DENIED until a new request
	enum u_desktop_access req_result;
	bool store_write_failed;
	enum u_desktop_control_state last_state;
	//! state + 1 of the last submit refused for state (0 = none since the last accepted
	//! submit): refusals are audited once per state, so a spamming app cannot flood the log.
	int refused_audit_state_plus1;
};

//! The audit record of the current capture burst (spec §10.4), capture worker only.
struct capture_burst
{
	bool open;
	uint32_t count;
	uint64_t last_ns;
	char first_t[32], last_t[32];
	uint32_t target;
	uint64_t window_id;
	char app[U_DESKTOP_APP_ID_SIZE];
	char title[U_DESKTOP_TITLE_SIZE];
	struct u_desktop_rect rect;
	uint32_t w, h;
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
	bool dying; //!< set before backend->destroy: consent answers are ignored from here on
	struct u_desktop_policy *policy;
	struct scope_state sc[SC_COUNT];
	//! The app has touched the CAPTURE scope (asked, queried, captured): only then
	//! are its state changes announced (a v1 app never sees a capture event).
	bool capture_seen;
	//! Last backend->is_locked answer (probed with the mutex released, see probe_locked)
	//! and the ticket of the probe that produced it: an older probe finishing late never
	//! overwrites a newer answer.
	bool locked_snapshot;
	uint64_t locked_seq_issued, locked_seq_stored;

	// --- capture (spec v2 §10), under mutex ---
	struct os_cond cap_cond;
	struct os_thread cap_thread;
	bool cap_thread_started;
	bool cap_job_pending; //!< accepted, not started yet
	bool cap_in_flight;   //!< accepted, completion not delivered yet
	struct u_desktop_capture_info cap_job;
	uint64_t cap_job_id;
	uint64_t cap_next_id;
	uint64_t cap_last_start_ns;
	struct u_desktop_image kept; //!< the most recent successful capture's image
	uint64_t kept_id;
	uint64_t kept_until_ns;
	uint64_t cap_retention_ns; //!< U_DESKTOP_CAPTURE_RETENTION_MS (tests may shorten it)
	uint64_t cap_burst_gap_ns; //!< U_DESKTOP_CAPTURE_BURST_GAP_MS (tests may shorten it)
	// --- capture worker only ---
	struct capture_burst burst;

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

static uint32_t
scope_bit(int idx)
{
	return idx == SC_CAPTURE ? U_DESKTOP_SCOPE_CAPTURE : U_DESKTOP_SCOPE_CONTROL;
}

static const char *
scope_name(int idx)
{
	return idx == SC_CAPTURE ? "capture" : "control";
}

//! The §3 table for scope @p idx (spec v2 §10.2: rows 1–7 are the same checks; rows 4
//! and 8–10 read that scope's own store answer and grant).
static enum u_desktop_control_state
evaluate_scope_locked(struct u_desktop_control *c, int idx, enum reason *out_reason)
{
	enum reason r;
	enum u_desktop_control_state s;
	struct scope_state *sc = &c->sc[idx];

	if (c->b == NULL || (idx == SC_CAPTURE && ((c->b->caps & U_DESKTOP_CAP_CAPTURE) == 0 || c->b->capture == NULL ||
	                                           c->b->monitors == NULL))) { // 1
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
	enum u_desktop_app_decision d = u_desktop_policy_app_scope(c->policy, c->exe, scope_bit(idx));
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
	if (!sc->requested) { // 6
		r = R_NOT_REQUESTED;
		s = U_DESKTOP_STATE_NOT_REQUESTED;
		goto out;
	}
	if (sc->pending) { // 7
		r = R_PENDING;
		s = U_DESKTOP_STATE_PENDING;
		goto out;
	}
	if (sc->revoked) {
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
		if (d == U_DESKTOP_APP_ALLOW ||
		    (sc->store_write_failed && sc->req_result == U_DESKTOP_ACCESS_GRANTED)) {
			r = R_STORE_ALLOW;
			s = U_DESKTOP_STATE_GRANTED;
			goto out;
		}
	} else {
		// 9. The OS's view, else (when it cannot tell without prompting) the
		// answer to this session's request.
		enum u_desktop_access (*status)(struct u_desktop_backend *) =
		    idx == SC_CAPTURE ? c->b->capture_access_status : c->b->access_status;
		enum u_desktop_access a = status != NULL ? status(c->b) : U_DESKTOP_ACCESS_UNKNOWN;
		if (a == U_DESKTOP_ACCESS_GRANTED ||
		    (a == U_DESKTOP_ACCESS_UNKNOWN && sc->req_result == U_DESKTOP_ACCESS_GRANTED)) {
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

static enum u_desktop_control_state
evaluate_locked(struct u_desktop_control *c, enum reason *out_reason)
{
	return evaluate_scope_locked(c, SC_CONTROL, out_reason);
}

//! Evaluate scope @p idx; fire its state callback on a change (or always, with @p force).
static enum u_desktop_control_state
update_scope_locked(struct u_desktop_control *c, int idx, enum reason *out_reason, bool force)
{
	struct scope_state *sc = &c->sc[idx];
	enum u_desktop_control_state s = evaluate_scope_locked(c, idx, out_reason);
	bool announce = idx == SC_CONTROL || c->capture_seen;
	if ((s != sc->last_state || force) && !c->stop && announce) {
		sc->last_state = s;
		u_desktop_state_fn fn = idx == SC_CAPTURE ? c->cb.capture_state_changed : c->cb.state_changed;
		if (fn != NULL) {
			fn(c->cb.ctx, s);
		}
	} else {
		sc->last_state = s;
	}
	return s;
}

//! The CONTROL scope (and, once the app touched it, the CAPTURE scope: a kill switch
//! or a lock is announced on both at the next call).
static enum u_desktop_control_state
update_state_locked(struct u_desktop_control *c, enum reason *out_reason, bool force)
{
	if (c->capture_seen) {
		update_scope_locked(c, SC_CAPTURE, NULL, false);
	}
	return update_scope_locked(c, SC_CONTROL, out_reason, force);
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

//! The CAPTURE scope, with a fresh lock probe (and the CONTROL scope's event too).
static enum u_desktop_control_state
update_capture_state(struct u_desktop_control *c, enum reason *out_reason)
{
	probe_locked(c);
	os_mutex_lock(&c->mutex);
	c->capture_seen = true;
	update_scope_locked(c, SC_CONTROL, NULL, false);
	enum u_desktop_control_state s = update_scope_locked(c, SC_CAPTURE, out_reason, false);
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
audit_consent(struct u_desktop_control *c, const char *what, const char *reason, const char *scope)
{
	struct u_desktop_audit_record r;
	memset(&r, 0, sizeof(r));
	r.exe = c->exe;
	r.action = what;
	r.chars = -1;
	r.reason = reason;
	r.scope = scope;
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
request_done_locked(struct u_desktop_control *c,
                    uint32_t asked,
                    enum u_desktop_access control,
                    enum u_desktop_access capture);

static void
ticket_done(struct request_ticket *t, enum u_desktop_access control, enum u_desktop_access capture)
{
	struct request_ctx *rq = t->rq;
	// Held across the handling: destroy clears rq->c under this mutex, so it
	// waits for an answer being handled and every later answer sees NULL.
	os_mutex_lock(&rq->mutex);
	struct u_desktop_control *c = rq->c;
	if (c != NULL) {
		os_mutex_lock(&c->mutex);
		request_done_locked(c, t->asked, control, capture);
		os_mutex_unlock(&c->mutex);
	}
	os_mutex_unlock(&rq->mutex);
	request_ctx_unref(rq); // the in-flight request's reference
	free(t);
}

//! u_desktop_access_done_fn: a CONTROL-only request through request_access.
static void
request_done(void *ctx, enum u_desktop_access result)
{
	ticket_done((struct request_ticket *)ctx, result, U_DESKTOP_ACCESS_UNKNOWN);
}

//! u_desktop_scopes_done_fn: a request through request_scopes.
static void
request_scopes_done(void *ctx, enum u_desktop_access control, enum u_desktop_access capture)
{
	ticket_done((struct request_ticket *)ctx, control, capture);
}

static void
request_done_locked(struct u_desktop_control *c,
                    uint32_t asked,
                    enum u_desktop_access control,
                    enum u_desktop_access capture)
{
	for (int idx = 0; idx < SC_COUNT; idx++) {
		if ((asked & scope_bit(idx)) != 0) {
			c->sc[idx].pending = false;
		}
	}
	if (c->dying || c->stop) {
		// Owed answer delivered from the backend's destroy: nobody is asking.
		return;
	}
	for (int idx = 0; idx < SC_COUNT; idx++) {
		struct scope_state *sc = &c->sc[idx];
		enum u_desktop_access result = idx == SC_CAPTURE ? capture : control;
		bool was_asked = (asked & scope_bit(idx)) != 0;
		bool definite = result == U_DESKTOP_ACCESS_GRANTED || result == U_DESKTOP_ACCESS_DENIED;
		if (!was_asked && !definite) {
			continue; // the prompt said nothing about this scope
		}
		if (!was_asked) {
			// One OS dialog decided a scope the app did not list (GNOME: the Start
			// response decides input and screens together): take its answer.
			sc->requested = true;
			sc->revoked = false;
		}
		sc->req_result = result;
		if (c->b != NULL && c->b->remembers_in_store && definite) {
			// Windows: DisplayXR's own prompt — the answer is remembered in the store.
			bool ok = u_desktop_policy_set_app_scope(
			    c->exe, scope_bit(idx),
			    result == U_DESKTOP_ACCESS_GRANTED ? U_DESKTOP_APP_ALLOW : U_DESKTOP_APP_DENY, "prompt");
			sc->store_write_failed = !ok;
			if (!ok) {
				U_LOG_W("desktop_control: could not persist the %s consent answer in the store",
				        scope_name(idx));
			}
		}
		audit_consent(c, result == U_DESKTOP_ACCESS_GRANTED ? "granted" : "denied", NULL,
		              idx == SC_CAPTURE || asked != U_DESKTOP_SCOPE_CONTROL ? scope_name(idx) : NULL);
		U_LOG_W("desktop_control: %s consent %s for %s", scope_name(idx),
		        result == U_DESKTOP_ACCESS_GRANTED ? "granted" : "denied", c->name);
		if (idx == SC_CAPTURE) {
			c->capture_seen = true;
		}
		// Always announce the outcome, even when it equals the previous state.
		update_scope_locked(c, idx, NULL, true);
	}
}

//! Rows that resolve a request without prompting (§3 rows 1–5).
static bool
blocks_request(enum reason why)
{
	return why == R_NO_BACKEND || why == R_ENV_OFF || why == R_STORE_UNREADABLE || why == R_KILL_SWITCH ||
	       why == R_STORE_DENY || why == R_LOCKED;
}

bool
u_desktop_parent_window_valid(const char *pw)
{
	if (pw == NULL || pw[0] == '\0') {
		return true;
	}
	size_t n = strnlen(pw, U_DESKTOP_PARENT_WINDOW_SIZE);
	if (n >= U_DESKTOP_PARENT_WINDOW_SIZE) {
		return false;
	}
	if (strncmp(pw, "wayland:", 8) == 0) {
		const char *h = pw + 8;
		if (*h == '\0') {
			return false;
		}
		// Printable ASCII 0x20..0x7E, SPACE INCLUDED: mutter's xdg-foreign handles are
		// 32 random printable characters and the space is one of them (half of the
		// handles in the GNOME §11 run had one). Control characters and non-ASCII are
		// refused.
		for (; *h != '\0'; h++) {
			if ((unsigned char)*h < 0x20 || (unsigned char)*h >= 0x7f) {
				return false;
			}
		}
		return true;
	}
	if (strncmp(pw, "x11:", 4) == 0) {
		const char *h = pw + 4;
		if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) {
			h += 2;
		}
		size_t digits = 0;
		for (; *h != '\0'; h++, digits++) {
			if (!isxdigit((unsigned char)*h)) {
				return false;
			}
		}
		return digits >= 1 && digits <= 16;
	}
	return false;
}

int32_t
u_desktop_control_request_scopes(struct u_desktop_control *c, const char *reason, uint32_t scopes)
{
	return u_desktop_control_request_ex(c, reason, scopes, NULL);
}

int32_t
u_desktop_control_request_ex(struct u_desktop_control *c,
                             const char *reason,
                             uint32_t scopes,
                             const char *parent_window)
{
	const uint32_t known = U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE;
	bool reask = (scopes & U_DESKTOP_SCOPE_REASK) != 0;
	scopes &= ~(uint32_t)U_DESKTOP_SCOPE_REASK;
	if (scopes == 0 || (scopes & ~known) != 0 || !u_desktop_parent_window_valid(parent_window)) {
		return U_DESKTOP_XR_ERROR_VALIDATION_FAILURE;
	}
	probe_locked(c); // row 5 needs a fresh answer: a locked session is never prompted
	os_mutex_lock(&c->mutex);
	if ((scopes & U_DESKTOP_SCOPE_CAPTURE) != 0) {
		c->capture_seen = true;
	}

	// A prompt is already up for a listed scope: never stack a second one.
	for (int idx = 0; idx < SC_COUNT; idx++) {
		if ((scopes & scope_bit(idx)) != 0 && evaluate_scope_locked(c, idx, NULL) == U_DESKTOP_STATE_PENDING) {
			os_mutex_unlock(&c->mutex);
			return U_DESKTOP_XR_SUCCESS;
		}
	}

	uint32_t ask = 0;
	for (int idx = 0; idx < SC_COUNT; idx++) {
		if ((scopes & scope_bit(idx)) == 0) {
			continue;
		}
		enum reason why;
		enum u_desktop_control_state s = evaluate_scope_locked(c, idx, &why);
		if (blocks_request(why) || (s == U_DESKTOP_STATE_GRANTED && !reask)) {
			// Rows 1–5: resolved without prompting; a scope already GRANTED is not
			// asked again. Either way the state is announced.
			update_scope_locked(c, idx, NULL, true);
			continue;
		}
		ask |= scope_bit(idx);
	}
	if (ask == 0) {
		os_mutex_unlock(&c->mutex);
		return U_DESKTOP_XR_SUCCESS;
	}

	audit_consent(c, "request", reason, ask == U_DESKTOP_SCOPE_CONTROL ? NULL : u_desktop_scope_str(ask));
	for (int idx = 0; idx < SC_COUNT; idx++) {
		if ((ask & scope_bit(idx)) != 0) {
			c->sc[idx].requested = true;
			c->sc[idx].revoked = false; // a new request on a user gesture re-asks the OS
		}
	}

	// Remembered grant: no prompt (row 8) — unless the app asks again.
	if (c->b->remembers_in_store && !reask) {
		for (int idx = 0; idx < SC_COUNT; idx++) {
			if ((ask & scope_bit(idx)) != 0 &&
			    u_desktop_policy_app_scope(c->policy, c->exe, scope_bit(idx)) == U_DESKTOP_APP_ALLOW) {
				ask &= ~scope_bit(idx);
				update_scope_locked(c, idx, NULL, true);
			}
		}
		if (ask == 0) {
			os_mutex_unlock(&c->mutex);
			return U_DESKTOP_XR_SUCCESS;
		}
	}

	struct u_desktop_backend *b = c->b;
	// CONTROL alone goes through request_access, exactly as in spec v1.
	bool via_scopes = b->request_scopes != NULL && ((ask & U_DESKTOP_SCOPE_CAPTURE) != 0 || reask);
	if (b->request_scopes == NULL && (ask & U_DESKTOP_SCOPE_CAPTURE) != 0) {
		// A capture-capable backend implements request_scopes; one that does not
		// cannot ask for CAPTURE (never reached: CAPTURE is UNSUPPORTED there).
		c->sc[SC_CAPTURE].req_result = U_DESKTOP_ACCESS_DENIED;
		update_scope_locked(c, SC_CAPTURE, NULL, true);
		ask &= ~(uint32_t)U_DESKTOP_SCOPE_CAPTURE;
		if (ask == 0) {
			os_mutex_unlock(&c->mutex);
			return U_DESKTOP_XR_SUCCESS;
		}
	}
	for (int idx = 0; idx < SC_COUNT; idx++) {
		if ((ask & scope_bit(idx)) != 0) {
			c->sc[idx].pending = true;
			update_scope_locked(c, idx, NULL, true);
		}
	}
	struct request_ctx *rq = c->rq;
	os_mutex_unlock(&c->mutex);

	struct request_ticket *t = (struct request_ticket *)calloc(1, sizeof(*t));
	if (t == NULL) {
		os_mutex_lock(&c->mutex);
		request_done_locked(c, ask, U_DESKTOP_ACCESS_DENIED, U_DESKTOP_ACCESS_DENIED);
		os_mutex_unlock(&c->mutex);
		return U_DESKTOP_XR_SUCCESS;
	}
	// After releasing c->mutex: ticket_done takes rq->mutex THEN c->mutex.
	os_mutex_lock(&rq->mutex);
	rq->refs++; // the in-flight request's reference, dropped by ticket_done
	os_mutex_unlock(&rq->mutex);
	t->rq = rq;
	t->asked = ask;

	struct u_desktop_app_info app = {c->exe, c->name, reason, parent_window != NULL ? parent_window : ""};
	// Either may call the done function before returning.
	if (via_scopes) {
		b->request_scopes(b, &app, ask | (reask ? (uint32_t)U_DESKTOP_SCOPE_REASK : 0u), request_scopes_done,
		                  t);
	} else {
		b->request_access(b, &app, request_done, t);
	}
	return U_DESKTOP_XR_SUCCESS;
}

int32_t
u_desktop_control_request(struct u_desktop_control *c, const char *reason)
{
	return u_desktop_control_request_scopes(c, reason, U_DESKTOP_SCOPE_CONTROL);
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
		bool first = c->sc[SC_CONTROL].refused_audit_state_plus1 != (int)st + 1;
		c->sc[SC_CONTROL].refused_audit_state_plus1 = (int)st + 1;
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
	c->sc[SC_CONTROL].refused_audit_state_plus1 = 0;
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
	if (!c->sc[SC_CONTROL].revoked) {
		c->sc[SC_CONTROL].revoked = true;
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
		} else if (c->sc[SC_CONTROL].revoked) {
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
		// "ok", or "ok (unverified: …)" when the backend could not confirm the
		// effect (U_DESKTOP_MSG_UNVERIFIED) — same success code either way.
		char ok[sizeof(msg)];
		u_desktop_success_message(msg, ok, sizeof(ok));
		snprintf(msg, sizeof(msg), "%s", ok);
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
 * Screen capture (spec v2 §10) — its own worker per session, never queued
 * behind actions and never subject to the idle wait (capture injects nothing).
 *
 */

enum u_desktop_control_state
u_desktop_control_get_capture_state(struct u_desktop_control *c)
{
	return update_capture_state(c, NULL);
}

bool
u_desktop_control_capture_validate(const struct u_desktop_capture_info *info, char *why, size_t why_cap)
{
	char dummy[8];
	if (why == NULL || why_cap == 0) {
		why = dummy;
		why_cap = sizeof(dummy);
	}
	why[0] = '\0';
	if (info == NULL) {
		snprintf(why, why_cap, "no capture info");
		return false;
	}
	if (info->target > U_DESKTOP_CAPTURE_WINDOW) {
		snprintf(why, why_cap, "unknown target %u", info->target);
		return false;
	}
	if (info->max_w < 1 || info->max_w > U_DESKTOP_CAPTURE_MAX_DIM || info->max_h < 1 ||
	    info->max_h > U_DESKTOP_CAPTURE_MAX_DIM) {
		snprintf(why, why_cap, "maxWidth / maxHeight must be 1..%u", U_DESKTOP_CAPTURE_MAX_DIM);
		return false;
	}
	if (info->target == U_DESKTOP_CAPTURE_REGION) {
		const int32_t L = U_DESKTOP_CONTROL_MAX_COORD;
		const struct u_desktop_rect *r = &info->region;
		if (r->w <= 0 || r->h <= 0) {
			snprintf(why, why_cap, "region width / height must be > 0");
			return false;
		}
		if (r->x < -L || r->x > L || r->y < -L || r->y > L || r->w > L || r->h > L) {
			snprintf(why, why_cap, "coordinates are limited to +/-%d", L);
			return false;
		}
	}
	if (info->target == U_DESKTOP_CAPTURE_WINDOW && info->window_id == 0) {
		snprintf(why, why_cap, "windowId is required");
		return false;
	}
	return true;
}

static void *
capture_worker(void *ptr);

int32_t
u_desktop_control_capture(struct u_desktop_control *c, const struct u_desktop_capture_info *info, uint64_t *out_id)
{
	if (out_id != NULL) {
		*out_id = 0;
	}
	if (!u_desktop_control_capture_validate(info, NULL, 0)) {
		return U_DESKTOP_XR_ERROR_VALIDATION_FAILURE;
	}
	if (c->b == NULL || (c->b->caps & U_DESKTOP_CAP_CAPTURE) == 0 || c->b->capture == NULL ||
	    c->b->monitors == NULL) {
		return U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED;
	}
	enum u_desktop_control_state st = update_capture_state(c, NULL);
	if (st != U_DESKTOP_STATE_GRANTED) {
		// Audited once per state, like a refused action submit (spec §5 step 6).
		os_mutex_lock(&c->mutex);
		bool first = c->sc[SC_CAPTURE].refused_audit_state_plus1 != (int)st + 1;
		c->sc[SC_CAPTURE].refused_audit_state_plus1 = (int)st + 1;
		os_mutex_unlock(&c->mutex);
		if (first) {
			struct u_desktop_audit_record r;
			memset(&r, 0, sizeof(r));
			r.exe = c->exe;
			r.action = "capture";
			r.chars = -1;
			r.result = "not_granted";
			r.message = u_desktop_control_state_str(st);
			u_desktop_audit_append(&r);
		}
		return U_DESKTOP_XR_ERROR_NOT_GRANTED;
	}

	os_mutex_lock(&c->mutex);
	c->sc[SC_CAPTURE].refused_audit_state_plus1 = 0;
	if (c->stop) {
		os_mutex_unlock(&c->mutex);
		return U_DESKTOP_XR_ERROR_RUNTIME_FAILURE;
	}
	if (c->cap_in_flight) {
		os_mutex_unlock(&c->mutex);
		return U_DESKTOP_XR_ERROR_LIMIT_REACHED;
	}
	if (!c->cap_thread_started) {
		os_thread_init(&c->cap_thread);
		if (os_thread_start(&c->cap_thread, capture_worker, c) != 0) {
			os_thread_destroy(&c->cap_thread);
			os_mutex_unlock(&c->mutex);
			return U_DESKTOP_XR_ERROR_RUNTIME_FAILURE;
		}
		c->cap_thread_started = true;
		os_thread_name(&c->cap_thread, "dxr: capture");
	}
	c->cap_job = *info;
	c->cap_job_id = ++c->cap_next_id;
	c->cap_job_pending = true;
	c->cap_in_flight = true;
	uint64_t id = c->cap_job_id;
	os_cond_broadcast(&c->cap_cond);
	os_mutex_unlock(&c->mutex);

	if (out_id != NULL) {
		*out_id = id;
	}
	return U_DESKTOP_XR_SUCCESS;
}

//! Drop the retained image if its time is up (mutex held).
static void
expire_kept_locked(struct u_desktop_control *c, uint64_t now_ns)
{
	if (c->kept.rgba != NULL && now_ns >= c->kept_until_ns) {
		u_desktop_image_free(&c->kept);
		c->kept_id = 0;
	}
}

void
u_desktop_control_set_capture_timing_for_tests(struct u_desktop_control *c,
                                               uint64_t retention_ns,
                                               uint64_t burst_gap_ns)
{
	os_mutex_lock(&c->mutex);
	if (retention_ns != 0) {
		c->cap_retention_ns = retention_ns;
	}
	if (burst_gap_ns != 0) {
		c->cap_burst_gap_ns = burst_gap_ns;
	}
	os_cond_broadcast(&c->cap_cond);
	os_mutex_unlock(&c->mutex);
}

int32_t
u_desktop_control_capture_image(struct u_desktop_control *c, uint64_t id, uint32_t cap, uint32_t *count, uint8_t *rgba)
{
	if (count == NULL || (cap > 0 && rgba == NULL)) {
		return U_DESKTOP_XR_ERROR_VALIDATION_FAILURE;
	}
	*count = 0;
	os_mutex_lock(&c->mutex);
	expire_kept_locked(c, os_monotonic_get_ns());
	if (id == 0 || c->kept.rgba == NULL || c->kept_id != id) {
		os_mutex_unlock(&c->mutex);
		return U_DESKTOP_XR_ERROR_VALIDATION_FAILURE;
	}
	uint32_t size = c->kept.w * c->kept.h * 4u;
	*count = size;
	if (cap == 0) {
		os_mutex_unlock(&c->mutex); // a size query: the image stays
		return U_DESKTOP_XR_SUCCESS;
	}
	if (cap < size) {
		os_mutex_unlock(&c->mutex);
		return U_DESKTOP_XR_ERROR_SIZE_INSUFFICIENT;
	}
	// Copied out: retention ends. Take ownership so the copy runs unlocked.
	struct u_desktop_image img = c->kept;
	memset(&c->kept, 0, sizeof(c->kept));
	c->kept_id = 0;
	os_mutex_unlock(&c->mutex);
	memcpy(rgba, img.rgba, size);
	u_desktop_image_free(&img);
	return U_DESKTOP_XR_SUCCESS;
}

static const char *
capture_kind(uint32_t target)
{
	switch (target) {
	case U_DESKTOP_CAPTURE_REGION: return "region";
	case U_DESKTOP_CAPTURE_WINDOW: return "window";
	default: return "desktop";
	}
}

//! Write the open burst's record (one per burst, spec §10.4). Capture worker / destroy.
static void
flush_burst(struct u_desktop_control *c)
{
	struct capture_burst *bu = &c->burst;
	if (!bu->open) {
		return;
	}
	struct u_desktop_audit_record r;
	memset(&r, 0, sizeof(r));
	r.exe = c->exe;
	r.action = "capture";
	r.chars = -1;
	r.scope = "capture";
	r.target_kind = capture_kind(bu->target);
	r.has_target = bu->target == U_DESKTOP_CAPTURE_WINDOW;
	r.target_id = bu->window_id;
	r.target_app = bu->target == U_DESKTOP_CAPTURE_WINDOW ? bu->app : NULL;
	r.target_title = bu->target == U_DESKTOP_CAPTURE_WINDOW ? bu->title : NULL;
	r.has_rect = true;
	r.rx = bu->rect.x;
	r.ry = bu->rect.y;
	r.rw = bu->rect.w;
	r.rh = bu->rect.h;
	r.has_size = true;
	r.w = bu->w;
	r.h = bu->h;
	r.count = bu->count;
	r.first_t = bu->first_t;
	r.last_t = bu->last_t;
	r.result = "ok";
	u_desktop_audit_append(&r);
	memset(bu, 0, sizeof(*bu));
}

//! A successful capture joins the open burst, or opens one (a different target closes it).
static void
audit_capture_ok(struct u_desktop_control *c,
                 const struct u_desktop_capture_info *info,
                 const struct u_desktop_window *win,
                 const struct u_desktop_rect *src,
                 uint32_t w,
                 uint32_t h)
{
	struct capture_burst *bu = &c->burst;
	bool same = bu->open && bu->target == info->target &&
	            (info->target != U_DESKTOP_CAPTURE_WINDOW || bu->window_id == info->window_id) &&
	            (info->target == U_DESKTOP_CAPTURE_WINDOW || memcmp(&bu->rect, src, sizeof(*src)) == 0);
	if (!same) {
		flush_burst(c);
		bu->open = true;
		bu->target = info->target;
		bu->window_id = info->target == U_DESKTOP_CAPTURE_WINDOW ? info->window_id : 0;
		u_desktop_audit_utc_now(bu->first_t, sizeof(bu->first_t));
	}
	if (win != NULL) {
		snprintf(bu->app, sizeof(bu->app), "%s", win->app_id);
		snprintf(bu->title, sizeof(bu->title), "%s", win->title);
	}
	bu->rect = *src;
	bu->w = w;
	bu->h = h;
	bu->count++;
	bu->last_ns = os_monotonic_get_ns();
	u_desktop_audit_utc_now(bu->last_t, sizeof(bu->last_t));
}

//! A failure closes the open burst and gets its own record (spec §10.4).
static void
audit_capture_fail(struct u_desktop_control *c,
                   const struct u_desktop_capture_info *info,
                   const struct u_desktop_window *win,
                   int32_t result,
                   const char *msg)
{
	flush_burst(c);
	struct u_desktop_audit_record r;
	memset(&r, 0, sizeof(r));
	r.exe = c->exe;
	r.action = "capture";
	r.chars = -1;
	r.scope = "capture";
	r.target_kind = capture_kind(info->target);
	r.has_target = info->target == U_DESKTOP_CAPTURE_WINDOW;
	r.target_id = info->window_id;
	if (win != NULL) {
		r.target_app = win->app_id;
		r.target_title = win->title;
	}
	if (info->target == U_DESKTOP_CAPTURE_REGION) {
		r.has_rect = true;
		r.rx = info->region.x;
		r.ry = info->region.y;
		r.rw = info->region.w;
		r.rh = info->region.h;
	}
	r.count = 1;
	r.result = result_str(result);
	r.message = msg;
	u_desktop_audit_append(&r);
}

//! The backend took the CAPTURE permission back: DENIED (event) until a new request.
static void
mark_capture_revoked(struct u_desktop_control *c)
{
	os_mutex_lock(&c->mutex);
	if (!c->sc[SC_CAPTURE].revoked) {
		c->sc[SC_CAPTURE].revoked = true;
		U_LOG_W("desktop_control: the OS revoked screen capture for %s", c->name);
	}
	update_scope_locked(c, SC_CAPTURE, NULL, false);
	// On GNOME both scopes share one portal session: the CONTROL scope's own
	// access_status says so, and is announced here too.
	update_scope_locked(c, SC_CONTROL, NULL, false);
	os_mutex_unlock(&c->mutex);
}

//! Spec §10.3 "Checks": state GRANTED, kill switch off, not locked, no secure desktop.
static bool
capture_allowed(struct u_desktop_control *c, int32_t *result, char *msg)
{
	enum reason why;
	enum u_desktop_control_state s = update_capture_state(c, &why);
	if (s == U_DESKTOP_STATE_GRANTED) {
		return true;
	}
	*result = refusal_for_state(s, why, msg, MSG_CAP);
	return false;
}

//! The window list (malloc'd, caller frees) — the capture worker's own copy.
static enum u_desktop_result
capture_list(struct u_desktop_control *c, struct u_desktop_window **out, uint32_t *n)
{
	*out = NULL;
	*n = 0;
	if ((c->b->caps & U_DESKTOP_CAP_LIST_WINDOWS) == 0 || c->b->list_windows == NULL) {
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	uint32_t cap = 64;
	for (int attempt = 0; attempt < 3; attempt++) {
		struct u_desktop_window *ws = (struct u_desktop_window *)calloc(cap, sizeof(*ws));
		if (ws == NULL) {
			return U_DESKTOP_ERR_FAILED;
		}
		uint32_t count = 0;
		enum u_desktop_result r = c->b->list_windows(c->b, ws, cap, &count);
		if (r != U_DESKTOP_OK) {
			free(ws);
			return r;
		}
		if (count <= cap) {
			*out = ws;
			*n = count;
			return U_DESKTOP_OK;
		}
		free(ws);
		cap = count + 16;
	}
	return U_DESKTOP_ERR_FAILED;
}

#define CAPTURE_MAX_MONITORS 16

static void
run_capture(struct u_desktop_control *c, const struct u_desktop_capture_info *info, uint64_t id)
{
	int32_t result = U_DESKTOP_XR_SUCCESS;
	char msg[MSG_CAP] = {0};
	struct u_desktop_rect src = {0, 0, 0, 0};
	struct u_desktop_window win;
	bool have_win = false;
	struct u_desktop_image img = {0};
	struct u_desktop_image out = {0};
	struct u_desktop_window *ws = NULL;
	uint32_t nws = 0;
	enum u_desktop_result br;

	// Checks before anything is resolved.
	if (!capture_allowed(c, &result, msg)) {
		goto done;
	}

	// Target resolution (§10.3).
	struct u_desktop_rect mons[CAPTURE_MAX_MONITORS];
	uint32_t nmon = 0;
	br = c->b->monitors(c->b, mons, CAPTURE_MAX_MONITORS, &nmon, msg);
	if (br != U_DESKTOP_OK) {
		result = map_backend_result(br, msg, MSG_CAP);
		goto done;
	}
	nmon = nmon > CAPTURE_MAX_MONITORS ? CAPTURE_MAX_MONITORS : nmon;
	struct u_desktop_rect bounds = u_desktop_rect_bounds(mons, nmon);
	if (info->target == U_DESKTOP_CAPTURE_WINDOW) {
		br = capture_list(c, &ws, &nws);
		if (br != U_DESKTOP_OK) {
			result = map_backend_result(br, msg, MSG_CAP);
			if (br == U_DESKTOP_ERR_UNSUPPORTED) {
				snprintf(msg, sizeof(msg),
				         "cannot find the window: this desktop backend cannot list windows");
			}
			goto done;
		}
		const struct u_desktop_window *w = NULL;
		for (uint32_t i = 0; i < nws; i++) {
			if (ws[i].id == info->window_id) {
				w = &ws[i];
				break;
			}
		}
		if (w == NULL) {
			result = U_DESKTOP_XR_ERROR_WINDOW_LOST;
			snprintf(msg, sizeof(msg), "the target window no longer exists");
			goto done;
		}
		win = *w;
		have_win = true;
		if ((w->state & U_DESKTOP_WINDOW_MINIMIZED) != 0) {
			result = U_DESKTOP_XR_ERROR_REFUSED;
			snprintf(msg, sizeof(msg), "the window is minimized");
			goto done;
		}
		if (!u_desktop_rect_intersect(&w->frame, &bounds, &src)) {
			result = U_DESKTOP_XR_ERROR_REFUSED;
			snprintf(msg, sizeof(msg), "found nothing: outside every monitor");
			goto done;
		}
	} else if (info->target == U_DESKTOP_CAPTURE_REGION) {
		if (!u_desktop_rect_intersect(&info->region, &bounds, &src)) {
			result = U_DESKTOP_XR_ERROR_REFUSED;
			snprintf(msg, sizeof(msg), "found nothing: outside every monitor");
			goto done;
		}
	} else {
		src = bounds;
		if (src.w <= 0 || src.h <= 0) {
			result = U_DESKTOP_XR_ERROR_REFUSED;
			snprintf(msg, sizeof(msg), "found nothing: no monitor");
			goto done;
		}
	}

	// Re-checked just before the OS capture.
	if (stopping(c)) {
		return;
	}
	if (!capture_allowed(c, &result, msg)) {
		goto done;
	}
	msg[0] = '\0';
	uint64_t t_call = os_monotonic_get_ns();
	br = c->b->capture(c->b, &src, &img, msg);
	if (br != U_DESKTOP_OK || img.rgba == NULL || img.w == 0 || img.h == 0) {
		u_desktop_image_free(&img);
		if (br == U_DESKTOP_OK) {
			br = U_DESKTOP_ERR_FAILED;
			snprintf(msg, sizeof(msg), "the backend returned no image");
		}
		if (br == U_DESKTOP_ERR_NOT_GRANTED) {
			mark_capture_revoked(c);
		}
		result = map_backend_result(br, msg, MSG_CAP);
		goto done;
	}
	if (img.frame_ns == 0 || img.frame_ns < t_call) {
		// Freshness (§10.3): never older than the call.
		img.frame_ns = t_call;
	}

	// The app's own windows are always withheld, on every OS (decision 7).
	free(ws);
	ws = NULL;
	br = capture_list(c, &ws, &nws);
	if (br != U_DESKTOP_OK) {
		result = U_DESKTOP_XR_ERROR_REFUSED;
		snprintf(msg, sizeof(msg), "cannot honour capture exclusion (the app's own windows)");
		goto done;
	}
	for (uint32_t i = 0; i < nws; i++) {
		if ((ws[i].state & U_DESKTOP_WINDOW_OWN) != 0 && (ws[i].state & U_DESKTOP_WINDOW_MINIMIZED) == 0) {
			u_desktop_image_withhold(&img, &src, &ws[i].frame);
		}
	}

	// Scale down to fit.
	uint32_t ow = 0, oh = 0;
	u_desktop_capture_fit(img.w, img.h, info->max_w, info->max_h, &ow, &oh);
	if (!u_desktop_image_downscale(&img, ow, oh, &out)) {
		result = U_DESKTOP_XR_ERROR_RUNTIME_FAILURE;
		snprintf(msg, sizeof(msg), "out of memory");
		goto done;
	}
	u_desktop_image_free(&img);

	// Re-checked before the hand-over: a frame finished as the screen locks (or the
	// kill switch lands) is dropped.
	if (stopping(c)) {
		u_desktop_image_free(&out);
		free(ws);
		return;
	}
	if (!capture_allowed(c, &result, msg)) {
		goto done;
	}

	os_mutex_lock(&c->mutex);
	u_desktop_image_free(&c->kept);
	c->kept = out;
	memset(&out, 0, sizeof(out));
	c->kept_id = id;
	c->kept_until_ns = os_monotonic_get_ns() + c->cap_retention_ns;
	struct u_desktop_capture_done d = {id, U_DESKTOP_XR_SUCCESS, c->kept.w, c->kept.h, src, c->kept.frame_ns, "ok"};
	os_mutex_unlock(&c->mutex);

	audit_capture_ok(c, info, have_win ? &win : NULL, &src, d.w, d.h);
	free(ws);
	os_mutex_lock(&c->mutex);
	bool stop = c->stop;
	os_mutex_unlock(&c->mutex);
	if (!stop && c->cb.capture_completed != NULL) {
		c->cb.capture_completed(c->cb.ctx, &d);
	}
	return;

done:
	u_desktop_image_free(&img);
	u_desktop_image_free(&out);
	free(ws);
	if (msg[0] == '\0') {
		map_backend_result(U_DESKTOP_ERR_FAILED, msg, MSG_CAP);
	}
	audit_capture_fail(c, info, have_win ? &win : NULL, result, msg);
	os_mutex_lock(&c->mutex);
	bool stopped = c->stop;
	os_mutex_unlock(&c->mutex);
	if (!stopped && c->cb.capture_completed != NULL) {
		struct u_desktop_capture_done d = {id, result, 0, 0, src, 0, msg};
		c->cb.capture_completed(c->cb.ctx, &d);
	}
}

static void *
capture_worker(void *ptr)
{
	struct u_desktop_control *c = (struct u_desktop_control *)ptr;
	const uint64_t interval = (uint64_t)U_DESKTOP_CAPTURE_MIN_INTERVAL_MS * NS_PER_MS;
	os_mutex_lock(&c->mutex);
	for (;;) {
		if (c->stop) {
			break;
		}
		const uint64_t gap = c->cap_burst_gap_ns;
		uint64_t now = os_monotonic_get_ns();
		expire_kept_locked(c, now);
		if (c->burst.open && now - c->burst.last_ns >= gap) {
			os_mutex_unlock(&c->mutex);
			flush_burst(c);
			os_mutex_lock(&c->mutex);
			continue;
		}
		uint64_t wake = UINT64_MAX;
		if (c->cap_job_pending) {
			// 4 Hz per session: an earlier call is accepted and waits here.
			uint64_t due = c->cap_last_start_ns != 0 ? c->cap_last_start_ns + interval : 0;
			if (now >= due) {
				struct u_desktop_capture_info job = c->cap_job;
				uint64_t id = c->cap_job_id;
				c->cap_job_pending = false;
				c->cap_last_start_ns = now;
				os_mutex_unlock(&c->mutex);
				run_capture(c, &job, id);
				os_mutex_lock(&c->mutex);
				c->cap_in_flight = false;
				continue;
			}
			wake = due;
		}
		if (c->kept.rgba != NULL && c->kept_until_ns < wake) {
			wake = c->kept_until_ns;
		}
		if (c->burst.open && c->burst.last_ns + gap < wake) {
			wake = c->burst.last_ns + gap;
		}
		if (wake == UINT64_MAX) {
			os_cond_wait(&c->cap_cond, &c->mutex);
		} else if (wake > now) {
			os_cond_wait_timeout_ns(&c->cap_cond, &c->mutex, wake - now);
		}
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
	c->cap_retention_ns = (uint64_t)U_DESKTOP_CAPTURE_RETENTION_MS * NS_PER_MS;
	c->cap_burst_gap_ns = (uint64_t)U_DESKTOP_CAPTURE_BURST_GAP_MS * NS_PER_MS;
	c->sc[SC_CONTROL].req_result = U_DESKTOP_ACCESS_UNKNOWN;
	c->sc[SC_CAPTURE].req_result = U_DESKTOP_ACCESS_UNKNOWN;
	c->policy = u_desktop_policy_create();
	c->list_cap = 64;
	c->list_buf = (struct u_desktop_window *)calloc(c->list_cap, sizeof(struct u_desktop_window));

	os_mutex_init(&c->mutex);
	os_cond_init(&c->cond);
	os_cond_init(&c->cap_cond);

	// Initial state: no event (nothing has changed yet from the app's view).
	os_mutex_lock(&c->mutex);
	c->sc[SC_CONTROL].last_state = evaluate_locked(c, NULL);
	c->sc[SC_CAPTURE].last_state = evaluate_scope_locked(c, SC_CAPTURE, NULL);
	os_mutex_unlock(&c->mutex);

	os_thread_init(&c->thread);
	if (os_thread_start(&c->thread, worker, c) == 0) {
		c->thread_started = true;
		os_thread_name(&c->thread, "dxr: desktop");
	}

	U_LOG_I("desktop_control: created for %s (backend %s, state %s)", c->name, c->b != NULL ? c->b->name : "none",
	        u_desktop_control_state_str(c->sc[SC_CONTROL].last_state));
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
	os_cond_broadcast(&c->cap_cond);
	bool cap_started = c->cap_thread_started;
	os_mutex_unlock(&c->mutex);

	// An action mid-flight finishes its current backend call first (its guard
	// now says no, so a well-behaved backend stops at the next batch).
	if (c->thread_started) {
		os_thread_join(&c->thread);
	}
	os_thread_destroy(&c->thread);
	// A capture mid-flight finishes its backend call (bounded) and is dropped.
	if (cap_started) {
		os_thread_join(&c->cap_thread);
		os_thread_destroy(&c->cap_thread);
	}
	flush_burst(c);
	u_desktop_image_free(&c->kept); // retention ends with the session

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
	os_cond_destroy(&c->cap_cond);
	os_cond_destroy(&c->cond);
	os_mutex_destroy(&c->mutex);
	free(c->list_buf);
	free(c);
}
