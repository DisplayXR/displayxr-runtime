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
 *   DXR_DESKTOP_FAKE_STORE=1         behave like Windows (remembers_in_store)
 *   DXR_DESKTOP_FAKE_LOCKED=1        the screen is locked
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

#ifdef __cplusplus
}
#endif
