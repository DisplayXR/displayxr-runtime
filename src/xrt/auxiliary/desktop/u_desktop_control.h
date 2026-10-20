// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The desktop-control common layer: consent state machine, observation
 *         gating, the action queue and the safety layer (XR_DXR_desktop_control
 *         spec §3–§5, ADR-056 D3). One object per XrSession.
 *
 * This is where ALL policy lives, identically on every OS; a backend
 * (u_desktop.h) is mechanism only.
 *
 * Results are XrResult-compatible int32 values (U_DESKTOP_XR_*), so the
 * state tracker passes them through unchanged; aux code does not include
 * OpenXR headers. oxr_desktop_control.c static-asserts the equality.
 *
 * Threading: every public function may be called from any thread. Actions run
 * in submission order on ONE worker thread per object. Callbacks are invoked
 * from the calling thread (state changes noticed during a call), the worker
 * (state changes, completions) or the backend's consent thread; they must be
 * quick and must NOT call back into this object.
 */

#pragma once

#include "u_desktop.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * XrResult-compatible values.
 */
#define U_DESKTOP_XR_SUCCESS 0
#define U_DESKTOP_XR_ERROR_VALIDATION_FAILURE (-1)
#define U_DESKTOP_XR_ERROR_RUNTIME_FAILURE (-2)
#define U_DESKTOP_XR_ERROR_FEATURE_UNSUPPORTED (-8)
#define U_DESKTOP_XR_ERROR_LIMIT_REACHED (-10)
#define U_DESKTOP_XR_ERROR_SIZE_INSUFFICIENT (-11)
#define U_DESKTOP_XR_ERROR_REFUSED (-1004999400)
#define U_DESKTOP_XR_ERROR_NOT_GRANTED (-1004999401)
#define U_DESKTOP_XR_ERROR_WINDOW_LOST (-1004999402)

//! Queue depth per object (spec §4).
#define U_DESKTOP_CONTROL_QUEUE_SIZE 64
//! TYPE_TEXT longer than this (bytes) fails validation.
#define U_DESKTOP_CONTROL_MAX_TEXT_BYTES 65536
//! CLICK clickCount above this fails validation (0 is treated as 1).
#define U_DESKTOP_CONTROL_MAX_CLICKS 3
//! |scrollX|, |scrollY| above this fail validation.
#define U_DESKTOP_CONTROL_MAX_SCROLL 100
//! |point|, |point2|, |rect.offset| and rect.extent above this fail validation.
#define U_DESKTOP_CONTROL_MAX_COORD 1000000

//! Same values as XrDesktopControlStateDXR.
enum u_desktop_control_state
{
	U_DESKTOP_STATE_UNSUPPORTED = 0,
	U_DESKTOP_STATE_NOT_REQUESTED = 1,
	U_DESKTOP_STATE_PENDING = 2,
	U_DESKTOP_STATE_GRANTED = 3,
	U_DESKTOP_STATE_DENIED = 4,
	U_DESKTOP_STATE_DISABLED_BY_USER = 5,
	U_DESKTOP_STATE_SUSPENDED = 6,
};

//! Same values as XrDesktopActionTypeDXR.
enum u_desktop_action_type
{
	U_DESKTOP_ACTION_ACTIVATE = 0,
	U_DESKTOP_ACTION_MOVE_RESIZE = 1,
	U_DESKTOP_ACTION_MINIMIZE = 2,
	U_DESKTOP_ACTION_RESTORE = 3,
	U_DESKTOP_ACTION_MAXIMIZE = 4,
	U_DESKTOP_ACTION_CLOSE = 5,
	U_DESKTOP_ACTION_TYPE_TEXT = 10,
	U_DESKTOP_ACTION_KEY_CHORD = 11,
	U_DESKTOP_ACTION_POINTER_MOVE = 12,
	U_DESKTOP_ACTION_CLICK = 13,
	U_DESKTOP_ACTION_DRAG = 14,
	U_DESKTOP_ACTION_SCROLL = 15,
};

//! Mirror of XrDesktopActionDXR. Strings are copied at submit.
struct u_desktop_action
{
	uint32_t type; //!< enum u_desktop_action_type (validated: may hold any value)
	uint64_t target_window_id;
	struct u_desktop_rect rect;
	int32_t x, y;    //!< point
	int32_t x2, y2;  //!< point2 (DRAG end)
	uint32_t button; //!< enum u_desktop_button (validated)
	uint32_t click_count;
	int32_t scroll_x, scroll_y;
	const char *text;
	const char *keys;
};

typedef void (*u_desktop_state_fn)(void *ctx, enum u_desktop_control_state state);
typedef void (*u_desktop_completed_fn)(void *ctx, uint64_t action_id, int32_t result, const char *message);

/*
 * Spec v2: screen capture (§10).
 */

//! Same values as XrDesktopCaptureTargetDXR.
enum u_desktop_capture_target
{
	U_DESKTOP_CAPTURE_DESKTOP = 0,
	U_DESKTOP_CAPTURE_REGION = 1,
	U_DESKTOP_CAPTURE_WINDOW = 2,
};

//! Mirror of XrDesktopCaptureInfoDXR.
struct u_desktop_capture_info
{
	uint32_t target; //!< enum u_desktop_capture_target (validated: may hold any value)
	uint64_t window_id;
	struct u_desktop_rect region;
	uint32_t max_w, max_h;
};

//! What XrEventDataDesktopCaptureCompletedDXR carries.
struct u_desktop_capture_done
{
	uint64_t capture_id;
	int32_t result;
	uint32_t w, h;                //!< 0 on failure
	struct u_desktop_rect source; //!< desktop coordinates
	uint64_t frame_ns;            //!< os_monotonic_get_ns() time the pixels were current (0 on failure)
	const char *message;
};

typedef void (*u_desktop_capture_done_fn)(void *ctx, const struct u_desktop_capture_done *done);

//! At least this long between two capture starts (4 Hz per session, spec §10.3).
#define U_DESKTOP_CAPTURE_MIN_INTERVAL_MS 250
//! The last successful image is kept this long at most (spec §10.3).
#define U_DESKTOP_CAPTURE_RETENTION_MS 10000
//! A capture burst's audit record is written after this long without a capture (spec §10.4).
#define U_DESKTOP_CAPTURE_BURST_GAP_MS 2000

struct u_desktop_control_callbacks
{
	u_desktop_state_fn state_changed;
	u_desktop_completed_fn action_completed;
	void *ctx;
	//! Spec v2 (may be NULL): the CAPTURE scope's state changed.
	u_desktop_state_fn capture_state_changed;
	//! Spec v2 (may be NULL): a capture completed (exactly one per accepted capture).
	u_desktop_capture_done_fn capture_completed;
};

//! The safety layer's timing (spec §5 step 2). Zero fields take the default.
struct u_desktop_control_timing
{
	uint64_t idle_poll_ns;     //!< default 50 ms
	uint64_t own_grace_ns;     //!< default 30 ms: our own injections are ignored this long after they end
	uint64_t idle_max_wait_ns; //!< default 10 s, then REFUSED "the user is active"
};

struct u_desktop_control;

/*!
 * The backend for this process: the in-memory fake when `DXR_DESKTOP_BACKEND`
 * is `fake`, nothing when it is `none`, else u_desktop_backend_create_platform().
 * Every caller (the state tracker, tests) goes through this, never the
 * platform function directly. Never prompts.
 */
struct u_desktop_backend *
u_desktop_backend_create(void);

/*!
 * Create the per-session object. Takes ownership of @p backend (may be NULL:
 * the state is then UNSUPPORTED forever). @p app's strings are copied (its
 * `reason` is ignored; pass one to u_desktop_control_request()). @p timing may
 * be NULL. `DXR_DESKTOP_CONTROL=0` in the environment at this call makes the
 * object UNSUPPORTED (spec §3 row 2). Starts the worker thread.
 */
int
u_desktop_control_create(struct u_desktop_backend *backend,
                         const struct u_desktop_app_info *app,
                         const struct u_desktop_control_callbacks *cb,
                         const struct u_desktop_control_timing *timing,
                         struct u_desktop_control **out);

/*!
 * Stop the worker (an action mid-flight finishes its current backend call,
 * then stops without an event), drop queued actions without events, destroy
 * the backend, free. No callback fires after this returns.
 */
void
u_desktop_control_destroy(struct u_desktop_control **c_ptr);

//! The state (§3 re-evaluated now; a change fires state_changed) + backend caps.
enum u_desktop_control_state
u_desktop_control_get_state(struct u_desktop_control *c, uint32_t *out_caps);

/*!
 * xrRequestDesktopControlDXR. Rows 1–5 resolve without prompting (the state
 * event is fired with the blocking state); a remembered store grant resolves
 * without prompting; otherwise the state goes PENDING and the backend asks.
 * The outcome is always announced through state_changed. Returns SUCCESS.
 */
int32_t
u_desktop_control_request(struct u_desktop_control *c, const char *reason);

// --- observation: GRANTED required, synchronous ---------------------------

//! Fills up to @p cap windows, sets @p count to the total.
int32_t
u_desktop_control_list_windows(struct u_desktop_control *c,
                               struct u_desktop_window *out,
                               uint32_t cap,
                               uint32_t *count);

int32_t
u_desktop_control_get_pointer(struct u_desktop_control *c, struct u_desktop_pick *out);

int32_t
u_desktop_control_pick(struct u_desktop_control *c, int32_t x, int32_t y, struct u_desktop_pick *out);

//! UTF-8 text into @p buf (always NUL-terminated, "" when none).
int32_t
u_desktop_control_text_at(struct u_desktop_control *c, int32_t x, int32_t y, char *buf, size_t cap);

// --- actions --------------------------------------------------------------

/*!
 * Validate and queue. On success @p out_id is set (never 0) and exactly one
 * action_completed callback follows (unless the object is destroyed first).
 * Errors: NOT_GRANTED, VALIDATION_FAILURE (no id), LIMIT_REACHED (queue full).
 */
int32_t
u_desktop_control_submit(struct u_desktop_control *c, const struct u_desktop_action *a, uint64_t *out_id);

//! Validation alone (spec §4), for tests and the state tracker. Fills @p why on failure.
bool
u_desktop_control_validate(const struct u_desktop_action *a, char *why, size_t why_cap);

/*!
 * Length of @p s[0..len) without a trailing, incomplete UTF-8 sequence (a code
 * point cut by a full buffer). Never splits a complete code point.
 */
size_t
u_desktop_utf8_trim_partial(const char *s, size_t len);

//! "UNSUPPORTED", "GRANTED", ...
const char *
u_desktop_control_state_str(enum u_desktop_control_state s);


// --- spec v2: scopes and screen capture (§10) -------------------------------

/*!
 * xrRequestDesktopControlDXR with an XrDesktopControlRequestScopeDXR: ask for
 * @p scopes (enum u_desktop_scope bits, non-zero) in ONE prompt. Per listed scope:
 * rows 1–5 resolve without prompting, a scope already GRANTED is not asked again,
 * a remembered store grant resolves without a prompt; each listed scope's outcome
 * is always announced on its own callback. A no-op while a listed scope is
 * PENDING. u_desktop_control_request() is this with CONTROL only. Returns SUCCESS,
 * or VALIDATION_FAILURE for 0 / unknown bits.
 */
int32_t
u_desktop_control_request_scopes(struct u_desktop_control *c, const char *reason, uint32_t scopes);

//! The CAPTURE scope's state (§3 re-evaluated now for that scope; a change fires
//! capture_state_changed). Never gated.
enum u_desktop_control_state
u_desktop_control_get_capture_state(struct u_desktop_control *c);

//! Capture validation alone (§10.3). Fills @p why on failure.
bool
u_desktop_control_capture_validate(const struct u_desktop_capture_info *info, char *why, size_t why_cap);

/*!
 * xrCaptureDesktopDXR: validate, gate (FEATURE_UNSUPPORTED without the capture
 * capability, NOT_GRANTED unless CAPTURE is GRANTED), one in flight
 * (LIMIT_REACHED), then hand to the session's capture worker. On SUCCESS
 * @p out_id is set (never 0) and exactly one capture_completed callback follows
 * (unless the object is destroyed first). Never prompts.
 */
int32_t
u_desktop_control_capture(struct u_desktop_control *c, const struct u_desktop_capture_info *info, uint64_t *out_id);

/*!
 * xrGetDesktopCaptureImageDXR: the retained image of capture @p id (two-call idiom,
 * BYTES). @p count is set to w * h * 4. A call that copies the image out ends its
 * retention; a size query or a too-small buffer (SIZE_INSUFFICIENT) does not. Any
 * other id, an expired or already-fetched one: VALIDATION_FAILURE.
 */
int32_t
u_desktop_control_capture_image(struct u_desktop_control *c, uint64_t id, uint32_t cap, uint32_t *count, uint8_t *rgba);


#ifdef __cplusplus
}
#endif
