// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  An in-memory desktop for XR_DXR_desktop_control tests (ADR-056).
 *
 * Built on every OS. Selected at runtime by `DXR_DESKTOP_BACKEND=fake`
 * (u_desktop_backend_create()), so the whole pipeline — OpenXR entry points,
 * the common safety layer, the store and the audit log — can be exercised end
 * to end without touching the real desktop, in CI, on any OS.
 *
 * The default desktop is a 1920x1080 screen with a 32 px shell panel on top and
 * three windows, top-most first:
 *
 *   101  gedit           "notes.txt - Text Editor"  (100,100 800x600)  focused
 *   102  gnome-terminal  "Terminal"                 (1000,200 800x600)
 *   103  nautilus        "Home"                     (50,720 600x300)
 *
 * Behaviour: activate raises + focuses (+ un-minimizes); a button press focuses
 * and raises the window under the pointer; typed text is appended to the
 * focused window's content, which text_at returns (else the window title);
 * close removes the window.
 *
 * Contract v2 (u_desktop.h "Delivery guarantee"), honoured exactly: text is
 * injected one code point per batch, a chord one key-down per batch, and before
 * every batch, every button press, every scroll and every window op the fake
 * calls guard->still_allowed and re-checks focus (text, chords) or the pick at
 * the VERIFIED point (press, scroll); a failed check stops with
 * U_DESKTOP_ERR_REFUSED, keeps what was delivered and releases what it pressed. Every injected operation is appended to
 * a log (test-only, so it DOES contain typed text — the audit log never does).
 *
 * Environment knobs when selected through `DXR_DESKTOP_BACKEND=fake`:
 *   DXR_DESKTOP_FAKE_ANSWER=deny     the consent request is refused
 *   DXR_DESKTOP_FAKE_CAPTURE_ANSWER=deny  the CAPTURE scope is refused (spec v2)
 *   DXR_DESKTOP_FAKE_STORE=1         behave like Windows (remembers_in_store)
 *   DXR_DESKTOP_FAKE_LOCKED=1        the screen is locked
 *
 * The batch hook (below) also fires "capture" right before a capture takes its
 * picture and "captured" right after, so a test can flip the kill switch or the
 * lock between the capture and the hand-over.
 */

#pragma once

#include "u_desktop.h"

#ifdef __cplusplus
extern "C" {
#endif

struct u_desktop_fake;

//! A fresh fake with the default desktop. The returned backend is owned by the
//! caller (destroy through backend->destroy).
struct u_desktop_backend *
u_desktop_fake_create(void);

//! The fake behind a backend created by u_desktop_fake_create(), else NULL.
struct u_desktop_fake *
u_desktop_fake_from_backend(struct u_desktop_backend *b);

// --- desktop -----------------------------------------------------------------

//! Insert @p w as the top-most window (z is recomputed).
void
u_desktop_fake_add_window(struct u_desktop_fake *f, const struct u_desktop_window *w);

void
u_desktop_fake_remove_window(struct u_desktop_fake *f, uint64_t id);

//! Set keyboard focus (0 = none) WITHOUT raising.
void
u_desktop_fake_set_focus(struct u_desktop_fake *f, uint64_t id);

//! Make pick() report something else at every point (e.g. a window that is
//! not in the list). Pass NULL to restore the real hit test.
void
u_desktop_fake_set_pick_override(struct u_desktop_fake *f, const struct u_desktop_pick *pick);

//! Content typed into window @p id so far ("" if none). Returns false if no such window.
bool
u_desktop_fake_window_content(struct u_desktop_fake *f, uint64_t id, char *out, size_t cap);

// --- consent / session -------------------------------------------------------

//! What access_status() reports right now.
void
u_desktop_fake_set_access(struct u_desktop_fake *f, enum u_desktop_access a);

/*!
 * How request_access answers. @p answer GRANTED / DENIED; when @p deferred the
 * callback is held until u_desktop_fake_complete_request(), else it is called
 * synchronously from inside request_access. A GRANTED answer also makes
 * access_status() report GRANTED (DENIED -> DENIED), unless remembers_in_store.
 */
void
u_desktop_fake_set_request_answer(struct u_desktop_fake *f, enum u_desktop_access answer, bool deferred);

//! Deliver a deferred answer. Returns false if no request is pending.
bool
u_desktop_fake_complete_request(struct u_desktop_fake *f);

//! How many times request_access was called.
uint32_t
u_desktop_fake_request_count(struct u_desktop_fake *f);

void
u_desktop_fake_set_remembers_in_store(struct u_desktop_fake *f, bool remembers);

void
u_desktop_fake_set_caps(struct u_desktop_fake *f, uint32_t caps);

void
u_desktop_fake_set_locked(struct u_desktop_fake *f, bool locked);

//! is_locked sleeps @p ms before answering (GNOME: a D-Bus call of up to 250 ms).
void
u_desktop_fake_set_lock_query_delay_ms(struct u_desktop_fake *f, uint32_t ms);

// --- idle --------------------------------------------------------------------

//! While true, the user is "typing": user_idle_ms reports 0 on every poll.
void
u_desktop_fake_set_user_active(struct u_desktop_fake *f, bool active);

//! Report the idle time as unknown (user_idle_ms returns false).
void
u_desktop_fake_set_idle_unknown(struct u_desktop_fake *f, bool unknown);

// --- actions -----------------------------------------------------------------

//! Every action call sleeps @p ms first (outside the fake's lock).
void
u_desktop_fake_set_action_delay_ms(struct u_desktop_fake *f, uint32_t ms);

//! Make the next action calls of kind @p op ("window_op", "type_text", "key_chord",
//! "pointer_move", "pointer_button", "scroll"; NULL = all) fail with @p r.
void
u_desktop_fake_set_action_result(struct u_desktop_fake *f, const char *op, enum u_desktop_result r);

/*!
 * Make the next type_text and key_chord calls end with @p r and report verdict
 * @p v through the guard (u_desktop_guard_set_verdict). @p r != OK: nothing is
 * typed or pressed. @p r == OK: the input is delivered as usual, then the verdict
 * is set. Either way the fake's own message is careless on purpose — it names the
 * text or the key (and on success starts "unverified: "): the common layer must
 * never pass it on. Off in a fresh fake; once set it stays set.
 */
void
u_desktop_fake_set_keyboard_verdict(struct u_desktop_fake *f, enum u_desktop_result r, enum u_desktop_input_verdict v);

//! pointer_move lands @p dx/dy away from where it was asked (0,0 = exact).
void
u_desktop_fake_set_pointer_skew(struct u_desktop_fake *f, int32_t dx, int32_t dy);

//! Raise window @p id to the top when the pointer next moves (another app
//! popping up between the move and the press).
void
u_desktop_fake_raise_on_next_move(struct u_desktop_fake *f, uint64_t id);

/*!
 * The operation log, one line per injected operation / window op, e.g.
 * "type 101 hello", "key ctrl+s", "move 400 300", "button left down",
 * "button left up 400 300" (a release names its point), "scroll 0 3", "op activate 102". Copies at most @p cap - 1 bytes.
 */
size_t
u_desktop_fake_log(struct u_desktop_fake *f, char *out, size_t cap);

//! Number of injected/window operations so far.
uint32_t
u_desktop_fake_op_count(struct u_desktop_fake *f);

void
u_desktop_fake_clear_log(struct u_desktop_fake *f);

// --- contract v2 knobs ---------------------------------------------------------

/*!
 * Called before every injection batch (outside the fake's lock — it may change
 * the desktop or the store): @p op is "prepare", "type", "key", "press",
 * "scroll" or "window_op"; @p index counts batches within the call. One more op,
 * "pressed", fires AFTER a button press was delivered (the button is held).
 */
typedef void (*u_desktop_fake_batch_fn)(void *ctx, const char *op, uint32_t index);

void
u_desktop_fake_set_batch_hook(struct u_desktop_fake *f, u_desktop_fake_batch_fn fn, void *ctx);

//! false: our injections do not reset the idle counter (macOS HID idle). Default true.
void
u_desktop_fake_set_injection_is_input(struct u_desktop_fake *f, bool counts);

//! Bit per enum u_desktop_button currently held down.
uint32_t
u_desktop_fake_pressed_buttons(struct u_desktop_fake *f);

//! How often prepare_input was called.
uint32_t
u_desktop_fake_prepare_count(struct u_desktop_fake *f);

//! Every action-path call (prepare, window op, text, chord, move, button, scroll).
uint32_t
u_desktop_fake_action_calls(struct u_desktop_fake *f);

struct u_desktop_fake_orphan
{
	u_desktop_access_done_fn done;
	void *ctx;
};

/*!
 * Test-only contract BREAK: on destroy, a consent answer still owed is NOT
 * delivered but stored in @p orphan, so a test can deliver it after the control
 * object is gone (the common layer must survive that).
 */
void
u_desktop_fake_set_orphan_on_destroy(struct u_desktop_fake *f, struct u_desktop_fake_orphan *orphan);

// --- spec v2: screen capture ---------------------------------------------------
//
// The fake's capture draws a deterministic picture of its desktop, monitor by
// monitor at each monitor's scale (device px = desktop px x scale), through the
// same u_desktop_image_draw_frame a real backend uses, then withholds every
// capture-excluded window. The picture of desktop point (x, y) (whole desktop px):
//   inside window W (top-most, not minimized):  R 0x10, G = W.id & 0xff, B 0xF0
//   elsewhere (the background):                 R = x & 0xff, G = y & 0xff, B 0x80
// all with alpha 255. An unshared monitor and the space between monitors stay
// (0,0,0,0). It never touches a real screen.

#define U_DESKTOP_FAKE_MAX_MONITORS 4

struct u_desktop_fake_monitor
{
	struct u_desktop_rect rect; //!< desktop coordinates
	uint32_t scale;             //!< device px per desktop px (1, 2, ...)
	bool shared;                //!< false: the user did not share it (GNOME) — withheld
};

//! Replace the monitors (default: one 1920x1080 at scale 1, shared).
void
u_desktop_fake_set_monitors(struct u_desktop_fake *f, const struct u_desktop_fake_monitor *m, uint32_t n);

//! Mark window @p id capture-excluded (withheld from every capture) or not.
void
u_desktop_fake_set_capture_excluded(struct u_desktop_fake *f, uint64_t id, bool excluded);

//! true: the fake cannot tell which windows are excluded — it refuses every capture.
void
u_desktop_fake_set_exclusion_unknown(struct u_desktop_fake *f, bool unknown);

//! What capture_access_status() reports right now (default UNKNOWN).
void
u_desktop_fake_set_capture_access(struct u_desktop_fake *f, enum u_desktop_access a);

/*!
 * How request_scopes answers the CAPTURE scope (default GRANTED; the CONTROL scope
 * follows u_desktop_fake_set_request_answer, deferral included). With
 * @p report_all, an answer reports BOTH scopes even when only one was asked (the
 * GNOME combined dialog).
 */
void
u_desktop_fake_set_capture_request_answer(struct u_desktop_fake *f, enum u_desktop_access answer, bool report_all);

//! The scopes of the last request_scopes call, and how many there were.
uint32_t
u_desktop_fake_last_request_scopes(struct u_desktop_fake *f, uint32_t *count);

/*!
 * The parent_window the last request_access / request_scopes call carried, copied
 * into @p out ("" when none). Returns false when the app info's field was NULL (the
 * common layer always passes a string, "" for none).
 */
bool
u_desktop_fake_last_parent_window(struct u_desktop_fake *f, char *out, size_t cap);

//! The next captures fail with @p r (OK = normal).
void
u_desktop_fake_set_capture_result(struct u_desktop_fake *f, enum u_desktop_result r);

//! Every capture sleeps @p ms first.
void
u_desktop_fake_set_capture_delay_ms(struct u_desktop_fake *f, uint32_t ms);

//! Captures the backend actually took.
uint32_t
u_desktop_fake_capture_count(struct u_desktop_fake *f);

//! The fake's picture of desktop point (@p x, @p y), RGBA — what a test expects.
void
u_desktop_fake_expected_pixel(struct u_desktop_fake *f, int32_t x, int32_t y, uint8_t out[4]);

#ifdef __cplusplus
}
#endif
