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
	//! with lost_before_key: the user's copy landed between reading the previous
	//! clipboard and our SetSelection, which replaced it (taking the clipboard)
	bool replaced;
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
 * | lost             | writes | field    | verdict                          |
 * |------------------|--------|----------|----------------------------------|
 * | before, replaced | any    | any      | CHANGED_HANDS_REPLACED (refused) |
 * | before           | any    | any      | CHANGED_HANDS (refused)          |
 * | after            | 0      | any      | LOST_NOT_SERVED (failed)         |
 * | after            | > 0    | changed  | DELIVERED                        |
 * | after            | > 0    | else     | LOST_UNCERTAIN (failed)          |
 * | no               | any    | changed  | DELIVERED                        |
 * | no               | 0      | else     | NOT_ASKED (failed)               |
 * | no               | > 0    | same     | FIELD_UNCHANGED (failed)         |
 * | no               | > 0    | no field | UNVERIFIED (ok, unverified)      |
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
//! message starts with U_DESKTOP_MSG_UNVERIFIED. The backend's own: the app gets
//! the fixed message of u_desktop_paste_input_verdict(v) instead (same words).
const char *
u_desktop_paste_message(enum u_desktop_paste_verdict v);

//! The keyboard-action verdict the app is told (u_desktop.h): one per paste
//! verdict, NONE for a plain delivery.
enum u_desktop_input_verdict
u_desktop_paste_input_verdict(enum u_desktop_paste_verdict v);

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
 * Chord keys (libei key codes).
 */

/*!
 * The evdev key code (linux/input-event-codes.h, what libei sends) of a named,
 * layout-independent key ("enter", "f5", "numpadadd", "mediaplaypause",
 * "rightctrl", ...), case-insensitively; 0 when @p name is not one (a letter,
 * digit or punctuation name is a CHARACTER: u_desktop_gnome_key_char). Every name
 * u_desktop_chord_named_key lists resolves here or there.
 */
uint32_t
u_desktop_gnome_key_code(const char *name);

/*!
 * The character a character key name stands for ("a".."z", "0".."9" and the
 * punctuation names: "minus" → '-'), resolved on the active layout's keymap by the
 * backend; 0 when @p name is not one. "A".."Z" give the lower-case letter (the key;
 * Shift is always explicit).
 */
uint32_t
u_desktop_gnome_key_char(const char *name);

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

/*
 * Portal probes at login: retried until they answer, a failure never cached.
 */

//! How often an unanswered portal probe is retried (the ScreenCast probe and the
//! RemoteDesktop version read alike).
#define U_DESKTOP_PORTAL_RETRY_MS 10000

/*!
 * A D-Bus error from reading a portal property: true when it means the portal
 * has not answered (yet) — a timeout / no reply, the bus name not owned or not
 * started, a dropped connection, an empty name — so the read must be retried;
 * false when it IS the portal's answer (no such interface / property / object:
 * this portal does not have it), which may be cached.
 */
bool
u_desktop_dbus_error_is_no_answer(const char *error_name);

//! A probe retried until it answers (never caching "no answer").
struct u_desktop_retry
{
	bool known;       //!< it answered: never probed again
	uint64_t next_ms; //!< the earliest next attempt (0 = now)
};

//! True when a probe should run now: not answered yet and the retry is due.
bool
u_desktop_retry_due(const struct u_desktop_retry *r, uint64_t now_ms);

//! Record an attempt: @p answered → known for good; else the next try in @p period_ms.
void
u_desktop_retry_record(struct u_desktop_retry *r, bool answered, uint64_t now_ms, uint32_t period_ms);

/*
 * Restoring a screen grant: wait for a stable monitor layout.
 */

/*!
 * The monitor layout as seen through repeated observations, for "restore only
 * after the layout has been stable for settle_ms" (spec §10.5, the DS1's HDMI
 * blink). Stability is measured from the LATER of: the last observed layout
 * change (any change of @p fingerprint — serial, connectors, rects — not just
 * mutter's serial), and the moment every monitor the grant covers was seen
 * present again after one was seen missing.
 */
struct u_desktop_layout_settle
{
	uint64_t fingerprint; //!< the last layout seen (0 = none yet)
	uint64_t changed_ms;  //!< when it last changed (0 = not seen changing)
	uint64_t present_ms;  //!< when the covered monitors came back (0 = never seen missing)
	bool was_missing;     //!< the last observation had a covered monitor missing
};

//! The layout the grant was made on: the baseline (nothing pending).
void
u_desktop_layout_settle_reset(struct u_desktop_layout_settle *s, uint64_t fingerprint);

#define U_DESKTOP_LAYOUT_MISSING UINT32_MAX

/*!
 * Feed one observation: @p missing covered monitors absent. Returns the ms left
 * until the layout counts as settled (0 = restore now), or
 * U_DESKTOP_LAYOUT_MISSING while a covered monitor is absent.
 */
uint32_t
u_desktop_layout_settle_step(
    struct u_desktop_layout_settle *s, uint64_t now_ms, uint64_t fingerprint, uint32_t missing, uint32_t settle_ms);

/*
 * Restoring a screen grant: the monitors it covers, and when one is gone for good.
 */

#define U_DESKTOP_GNOME_COVERED_MAX 8
#define U_DESKTOP_GNOME_MON_NAME 32

//! A logical monitor in mutter's DisplayConfig (GetCurrentState): its connector,
//! where it is (stage px), and what its (connector, vendor, product, serial) spec
//! says about it.
struct u_desktop_gnome_monitor
{
	char conn[U_DESKTOP_GNOME_MON_NAME];
	struct u_desktop_rect rect;
	bool cast_virtual;                     //!< u_desktop_gnome_is_cast_virtual
	char serial[U_DESKTOP_GNOME_MON_NAME]; //!< the spec's serial
};

/*!
 * True for the virtual monitor mutter creates for a ScreenCast `RecordVirtual`
 * stream: a screen-casting or remote-desktop tool's "extended display". Its spec
 * is vendor "MetaVendor", product "Virtual remote monitor", serial "0x%.6x" from a
 * counter (mutter 42.9, 46.2 and 50.1 alike). Its connector is "Meta-<n>", a name
 * the persistent `--virtual-monitor` monitors of a headless session share
 * (product "MetaVirtualMonitor"), so the name alone is not the marker.
 *
 * It exists only while that stream runs and never comes back as the same monitor:
 * the next one gets a new serial, and xdg-desktop-portal-gnome restores a monitor
 * only by vendor:product:serial. So a grant that covered one cannot restore its
 * screens once it is gone. Waiting for it is pointless.
 */
bool
u_desktop_gnome_is_cast_virtual(const char *vendor, const char *product);

//! The monitors a screen grant covers (the monitor-blink guard, spec §10.5).
struct u_desktop_gnome_covered
{
	//! Real monitors, by connector: one that is missing at a restore is waited for
	//! (an HDMI blink).
	char conn[U_DESKTOP_GNOME_COVERED_MAX][U_DESKTOP_GNOME_MON_NAME];
	uint32_t n;
	//! Screen-cast virtual monitors, by connector AND serial: gone for good once
	//! that monitor is not there (never waited for).
	char vconn[U_DESKTOP_GNOME_COVERED_MAX][U_DESKTOP_GNOME_MON_NAME];
	char vserial[U_DESKTOP_GNOME_COVERED_MAX][U_DESKTOP_GNOME_MON_NAME];
	uint32_t nv;
};

/*!
 * The monitors a session's screen streams show: for each stream rect (stage px),
 * the monitor in @p mons whose rect matches it within 2 px, as a real or a
 * cast-virtual entry of @p out. A stream that matches no monitor adds nothing.
 */
void
u_desktop_gnome_covered_from_streams(const struct u_desktop_rect *streams,
                                     uint32_t nstreams,
                                     const struct u_desktop_gnome_monitor *mons,
                                     uint32_t nmons,
                                     struct u_desktop_gnome_covered *out);

/*!
 * Which covered monitors @p mons lacks. @p missing: real ones whose connector is
 * absent. @p gone: cast-virtual ones not present as that same monitor (the
 * connector with the recorded serial, still cast-virtual); a recreated virtual
 * monitor that took the old connector is a different monitor. @p names (may be
 * NULL) gets the connectors, comma-separated, for the log.
 */
void
u_desktop_gnome_covered_missing(const struct u_desktop_gnome_covered *cov,
                                const struct u_desktop_gnome_monitor *mons,
                                uint32_t nmons,
                                uint32_t *missing,
                                uint32_t *gone,
                                char *names,
                                size_t names_cap);

enum u_desktop_restore_verdict
{
	//! The layout is settled and every covered monitor is there: restore.
	U_DESKTOP_RESTORE_NOW = 0,
	//! Keep waiting: a covered monitor is missing (a blink?), or the layout changed
	//! less than settle_ms ago.
	U_DESKTOP_RESTORE_WAIT,
	//! A covered monitor will not come back: a cast-virtual one is gone (at once),
	//! or a real one is still missing after wait_max_ms. The portal restores the
	//! screens all or nothing, so it cannot restore them now.
	U_DESKTOP_RESTORE_GONE,
	//! Every covered monitor is there, but the layout kept changing for the whole wait.
	U_DESKTOP_RESTORE_UNSETTLED,
};

/*!
 * One observation of the restore guard (spec §10.5): u_desktop_layout_settle_step
 * plus when to stop waiting. @p start_ms: when this restore began waiting;
 * @p missing / @p gone: from u_desktop_gnome_covered_missing. A real monitor back
 * within @p wait_max_ms is restored on settle_ms after it returned, exactly as
 * before (the DS1's HDMI blink).
 */
enum u_desktop_restore_verdict
u_desktop_restore_guard_step(struct u_desktop_layout_settle *s,
                             uint64_t start_ms,
                             uint64_t now_ms,
                             uint64_t fingerprint,
                             uint32_t missing,
                             uint32_t gone,
                             uint32_t settle_ms,
                             uint32_t wait_max_ms);

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
