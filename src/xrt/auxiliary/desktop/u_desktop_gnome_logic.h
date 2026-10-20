// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: the backend's decisions as pure functions
 *         (ADR-056). Compiled on every OS so tests_desktop_control pins them;
 *         the GNOME backend (u_desktop_gnome*.c) is their only caller.
 */

#pragma once

#include "u_desktop.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The paste (type_text): what was observed, and what it means.
 */

//! What the paste saw. text_check: whether the focused text field (found through
//! AT-SPI before the paste) changed: 1 changed, 0 unchanged, -1 no accessible
//! text field (or no answer) — then nothing confirms or denies the insert.
struct u_desktop_paste_obs
{
	bool lost_before_key;      //!< the selection changed hands before the paste keystroke
	bool lost_after_key;       //!< ... after it, while the paste was being served
	uint32_t writes_after_key; //!< reads of the agent's text after the keystroke (any reader)
	int text_check;
};

enum u_desktop_paste_verdict
{
	U_DESKTOP_PASTE_DELIVERED = 0,   //!< the focused text field changed
	U_DESKTOP_PASTE_UNVERIFIED,      //!< read after the keystroke; nothing confirms the insert
	U_DESKTOP_PASTE_CHANGED_HANDS,   //!< the user copied before the keystroke: nothing pasted
	U_DESKTOP_PASTE_LOST_UNCERTAIN,  //!< the user copied after the keystroke: outcome unknown
	U_DESKTOP_PASTE_NOT_ASKED,       //!< nobody read the agent's text
	U_DESKTOP_PASTE_FIELD_UNCHANGED, //!< read, but the focused text field did not change
	//! The user copied after the keystroke and the agent's text was never read:
	//! the target can only have been handed the user's clipboard.
	U_DESKTOP_PASTE_LOST_NOT_SERVED,
	//! The user copied in the ~1.5 ms between reading the previous clipboard and
	//! our SetSelection: refused, but our SetSelection had already replaced the
	//! user's copy (gone); the selection is released, the clipboard left empty.
	U_DESKTOP_PASTE_CHANGED_HANDS_REPLACED,
};

//! The focused text field seen through AT-SPI: its length, its caret and a hash
//! of the text around the caret (the text itself is never kept or logged).
struct u_desktop_text_state
{
	bool found; //!< a focused accessible with a Text interface was found
	int32_t count;
	int32_t caret;
	uint64_t hash;
};

//! obs.text_check from the state before the paste and after: -1 when either
//! has no focused text field (nothing to compare), else 1 changed / 0 not.
int
u_desktop_text_check(const struct u_desktop_text_state *before, const struct u_desktop_text_state *after);

//! FNV-1a over @p len bytes (for u_desktop_text_state::hash).
uint64_t
u_desktop_fnv1a(const char *s, size_t len);

/*!
 * The verdict, and (@p restore) whether the previous clipboard may be offered
 * again: never once the selection changed hands — that would overwrite what the
 * user just copied.
 *
 * | lost      | writes | field    | verdict                         |
 * |-----------|--------|----------|---------------------------------|
 * | before    | any    | any      | CHANGED_HANDS (refused)         |
 * | after     | 0      | any      | LOST_NOT_SERVED (failed)        |
 * | after     | > 0    | changed  | DELIVERED                       |
 * | after     | > 0    | else     | LOST_UNCERTAIN (failed)         |
 * | no        | any    | changed  | DELIVERED                       |
 * | no        | 0      | else     | NOT_ASKED (failed)              |
 * | no        | > 0    | same     | FIELD_UNCHANGED (failed)        |
 * | no        | > 0    | no field | UNVERIFIED (ok, unverified)     |
 */
enum u_desktop_paste_verdict
u_desktop_paste_decide(const struct u_desktop_paste_obs *o, bool *restore);

/*!
 * The end of a type, for the clipboard: offer the previous text again
 * (RESTORE), RELEASE the selection we still hold (SetSelection with no
 * mime_types — never sit on it serving nothing: the clipboard would be owned and
 * empty), or LEAVE it (another client owns it: it is the user's).
 * @p changed_hands: the selection changed hands at any time since the type began
 * (clip_lost included); @p owner: this session owns the selection now;
 * @p have_prev: a previous text was read.
 */
enum u_desktop_clip_end
{
	U_DESKTOP_CLIP_END_RESTORE = 0,
	U_DESKTOP_CLIP_END_RELEASE,
	U_DESKTOP_CLIP_END_LEAVE,
};

enum u_desktop_clip_end
u_desktop_clip_end_decide(bool changed_hands, bool owner, bool have_prev);

//! The result code of a verdict (existing codes only).
enum u_desktop_result
u_desktop_paste_result(enum u_desktop_paste_verdict v);

//! The message of a verdict (never names any text). An UNVERIFIED delivery's
//! message starts with U_DESKTOP_MSG_UNVERIFIED.
const char *
u_desktop_paste_message(enum u_desktop_paste_verdict v);

/*!
 * A backend that succeeded without being able to confirm the effect fills its
 * message with this prefix; the common layer then completes the action
 * "ok (unverified: …)" instead of plain "ok" (u_desktop_success_message).
 */
#define U_DESKTOP_MSG_UNVERIFIED "unverified: "

//! The completion message of a SUCCESSFUL action from the backend's message.
void
u_desktop_success_message(const char *backend_msg, char *out, size_t cap);

/*
 * Window moves across monitors.
 */

/*!
 * Ubuntu's Tiling Assistant judges a drag against the PREVIOUS monitor for
 * 150 ms after the pointer changes monitors (its monitor-switch-grace-period, on
 * by default): a release inside that window tiles the window into the SOURCE
 * monitor. The drop waits this long on the target monitor after the last
 * crossing (250 ms verified on GNOME 50.1; margin added).
 */
#define U_DESKTOP_CROSS_DWELL_MS 300

/*!
 * A Super+click right after the pointer changed monitors can be applied with a
 * STALE pointer position: in Ubuntu's session (Tiling Assistant + dock) mutter's
 * grab relocated the window ~4–5 ms after the press, to where the first waypoint
 * from the other monitor would put it (GNOME 50.1: 4/65 activates, 4/18 with the
 * avatar window). The press waits this long on the target monitor after the last
 * crossing, at the press point, before Super goes down.
 */
#define U_DESKTOP_PRESS_SETTLE_MS U_DESKTOP_CROSS_DWELL_MS

/*!
 * The frame moved (or changed size) by more than @p tol px on any edge. An
 * ACTIVATE must never return a plain success with the window moved: it is put
 * back (a verified move to @p before) or the activate fails saying so.
 */
bool
u_desktop_frame_moved(const struct u_desktop_rect *before, const struct u_desktop_rect *after, int32_t tol);

//! How much longer to wait before releasing: 0 when @p dwell_ms have passed
//! since @p crossed_at_ms (0 = no crossing).
uint32_t
u_desktop_cross_dwell_left_ms(uint64_t crossed_at_ms, uint64_t now_ms, uint32_t dwell_ms);

/*!
 * Shell UI covering the screen that was not there before a gesture (Ubuntu
 * Tiling Assistant's popup after a tile, an overview…): @p before / @p after are
 * the pick kinds (enum u_desktop_pick_kind) at the same @p n sample points.
 * True when a point that was not shell before is shell now. A point that was
 * already shell (the user's own dialog, a panel) never counts: only what our
 * gesture opened is ours to dismiss; nor does a point whose kind before is
 * unknown (U_DESKTOP_COVER_UNKNOWN, a failed pick).
 */
#define U_DESKTOP_COVER_UNKNOWN 0xFFu
bool
u_desktop_shell_cover_appeared(const uint8_t *before, const uint8_t *after, uint32_t n);

#ifndef _WIN32
/*!
 * write(2) that cannot raise SIGPIPE: the clipboard reader may go away mid-write
 * (the selection changed hands), and this runs in the APP's process — so the
 * process-wide disposition is never touched. SIGPIPE is blocked for the calling
 * thread around the write, a SIGPIPE the write raised is consumed before
 * unblocking (one already pending before is left alone), and the result is
 * write's (-1 / EPIPE when the reader is gone).
 */
ptrdiff_t
u_desktop_write_nosigpipe(int fd, const void *buf, size_t len);
#endif

#ifdef __cplusplus
}
#endif
