// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The desktop-control user store and audit log (XR_DXR_desktop_control
 *         spec §6, ADR-056 D3.6).
 *
 * Store — one JSON file per user, `desktop_control.json`:
 *
 *     { "version": 2, "suspended": false, "idle_ms": 600,
 *       "apps": { "<executable full path>": "allow" | "deny"
 *                 | { "control": "allow" | "deny", "capture": "allow" | "deny" } } }
 *
 * Spec v2 (§10.6): an app's entry is a string (v1: the CONTROL answer) or an
 * object with one answer per scope. v1 files are read as is; every write
 * rewrites the file as version 2, and an entry with no CAPTURE answer is kept as
 * a plain string (so an older runtime reading the file still sees its CONTROL
 * deny). The kill switch covers both scopes. `kill_switch_count` (incremented by
 * every kill switch "on") lets a running session see that the kill switch happened
 * even when it is off again (spec §6.4: the kill switch ends running grants).
 *
 *   Windows  %LOCALAPPDATA%\DisplayXR\
 *   macOS    ~/Library/Application Support/DisplayXR/
 *   Linux    $XDG_CONFIG_HOME/displayxr/ (default ~/.config/displayxr/)
 *
 * Audit — JSON lines, `desktop_control_audit.jsonl`, next to the store
 * (Linux: $XDG_STATE_HOME/displayxr/, default ~/.local/state/displayxr/),
 * rotated to `.1` at 1 MiB. The record type has NO text field: typed text can
 * never reach the log, only its length (`chars`).
 *
 * Saved OS grants — one private file per app under `desktop-control/` next
 * to the audit log (Linux: $XDG_STATE_HOME/displayxr/desktop-control/, default
 * ~/.local/state/displayxr/desktop-control/). Only the GNOME backend saves one
 * (its portal restore token, u_desktop_gnome_grant.h); the kill switch removes
 * them all. See "Saved OS grants" below.
 *
 * `DXR_DESKTOP_CONTROL_DIR`, when set and non-empty, overrides ALL THREE
 * locations (tests, CI, a portable install): the store and the audit log go in
 * it, the saved grants in its `desktop-control/` subdirectory.
 *
 * Reads are cached: u_desktop_policy_refresh() re-reads the file only when its
 * stamp (mtime + size + file identity) changed. Writes are read-modify-write
 * and atomic (temp file + rename; Windows MoveFileExW REPLACE_EXISTING). A
 * MISSING store means defaults: nothing suspended, nobody allowed or denied,
 * idle_ms 600. A store that EXISTS but cannot be parsed never means defaults:
 * the last good policy read in this process is kept, and with none the
 * policy reads as suspended (u_desktop_policy_is_unreadable()). Writers take a
 * cross-process lock on `<store>.lock` (flock / LockFileEx) and re-read the
 * store under it; they refuse to change an unparseable store, except the kill
 * switch, which moves it aside to `<store>.corrupt` and writes a suspended one.
 *
 * Not thread-safe per object; the owner (u_desktop_control) serialises.
 * Never links a backend: the CLI uses this file alone.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_DESKTOP_POLICY_DEFAULT_IDLE_MS 600u
//! idle_ms values above this are clamped (the idle wait itself is capped at 10 s).
#define U_DESKTOP_POLICY_MAX_IDLE_MS 10000u
#define U_DESKTOP_AUDIT_ROTATE_BYTES (1024u * 1024u)
#define U_DESKTOP_POLICY_DIR_ENV "DXR_DESKTOP_CONTROL_DIR"

enum u_desktop_app_decision
{
	U_DESKTOP_APP_NONE = 0, //!< no entry for this executable
	U_DESKTOP_APP_ALLOW,
	U_DESKTOP_APP_DENY,
};

//! Full path of the store file (directories are not created).
bool
u_desktop_policy_store_path(char *out, size_t cap);

//! Full path of the audit log (directories are not created).
bool
u_desktop_policy_audit_path(char *out, size_t cap);

//! True when @p a and @p b name the same executable (Windows: ASCII
//! case-insensitive, either separator; POSIX: byte-exact).
bool
u_desktop_policy_exe_equal(const char *a, const char *b);


/*
 *
 * Cached reader.
 *
 */

struct u_desktop_policy;

struct u_desktop_policy *
u_desktop_policy_create(void);

void
u_desktop_policy_destroy(struct u_desktop_policy **p_ptr);

//! Re-read the store if it changed on disk. Returns true when it reloaded.
bool
u_desktop_policy_refresh(struct u_desktop_policy *p);

//! The kill switch (`displayxr-cli desktop stop`) — also true while the store is
//! unreadable with no good read to fall back on (fail closed, spec §6.1).
bool
u_desktop_policy_is_suspended(const struct u_desktop_policy *p);

/*!
 * The store exists but cannot be parsed and this process has no good read of it
 * to keep: everything is suspended until the file is fixed or removed. (With a
 * good read in hand, the last good policy is kept and this stays false.)
 */
bool
u_desktop_policy_is_unreadable(const struct u_desktop_policy *p);

uint32_t
u_desktop_policy_idle_ms(const struct u_desktop_policy *p);

/*!
 * How many times the kill switch was turned on (`kill_switch_count` in the store,
 * 0 when absent). It only ever changes when it is turned on, so a session that sees
 * it change knows the kill switch happened since — even if it was turned off again
 * before the session looked (spec §6.4: the kill switch ends running grants).
 */
uint64_t
u_desktop_policy_kill_switch_count(const struct u_desktop_policy *p);

//! The CONTROL answer for @p exe (spec v1 meaning).
enum u_desktop_app_decision
u_desktop_policy_app(const struct u_desktop_policy *p, const char *exe);

//! The answer for @p exe in ONE @p scope (enum u_desktop_scope). A deny under any
//! spelling of the executable wins.
enum u_desktop_app_decision
u_desktop_policy_app_scope(const struct u_desktop_policy *p, const char *exe, uint32_t scope);

//! One call per entry with its CONTROL answer (v1 callers).
typedef void (*u_desktop_policy_app_fn)(void *ctx, const char *exe, enum u_desktop_app_decision d);

void
u_desktop_policy_for_each_app(const struct u_desktop_policy *p, u_desktop_policy_app_fn fn, void *ctx);

//! One call per entry with both answers (NONE where the entry has none).
typedef void (*u_desktop_policy_app_scopes_fn)(void *ctx,
                                               const char *exe,
                                               enum u_desktop_app_decision control,
                                               enum u_desktop_app_decision capture);

void
u_desktop_policy_for_each_app_scopes(const struct u_desktop_policy *p, u_desktop_policy_app_scopes_fn fn, void *ctx);

//! "control", "capture", "all" (both) — for messages and the audit log.
const char *
u_desktop_scope_str(uint32_t scopes);


/*
 *
 * Writers — read-modify-write, atomic, and each one audited.
 *
 */

/*!
 * Set (ALLOW / DENY) or remove (NONE, "forget") the entry for @p exe. @p by
 * names the writer in the audit record ("cli", "prompt", ...).
 */
bool
u_desktop_policy_set_app(const char *exe, enum u_desktop_app_decision d, const char *by);

/*!
 * As u_desktop_policy_set_app, for the scopes in @p scopes (enum u_desktop_scope
 * bits; the others keep their answer). NONE removes those scopes' answers; the
 * entry goes away when no answer is left. The audit record names the scope.
 */
bool
u_desktop_policy_set_app_scope(const char *exe, uint32_t scopes, enum u_desktop_app_decision d, const char *by);

/*!
 * Flip the kill switch. Audited as "suspended" / "resumed". Turning it ON also
 * removes every saved OS grant (u_desktop_grant_remove_all), whether or not the
 * store could be written: after a kill switch the next request of every app
 * shows the OS consent dialog again (spec §6.4).
 */
bool
u_desktop_policy_set_suspended(bool suspended, const char *by);


/*
 *
 * Saved OS grants (spec §6.4, ADR-056 Amendment 2).
 *
 * One small file per key (an app id, or "exe-…" for an app without one) that a backend writes so that the OS consent
 * survives a restart of the app. Only the GNOME backend writes one today: the
 * portal's restore token and what it covers (u_desktop_gnome_grant.h). This layer
 * only stores bytes, privately:
 *  - the directory `desktop-control/` (u_desktop_grant_dir) is created 0700 and
 *    tightened to 0700 when it already exists; a directory that is a symlink, not
 *    this user's or writable by others is never read or written;
 *  - a file is written 0600, atomically (an O_EXCL temp file, fsync, rename);
 *  - a file is read only when it is a regular file (not a symlink) of this user
 *    with no group / other permission bits, at most U_DESKTOP_GRANT_MAX_BYTES.
 *
 * POSIX only. On Windows every call fails or finds nothing (no Windows backend
 * saves a grant: the DisplayXR store is its consent).
 *
 */

#define U_DESKTOP_GRANT_DIR_NAME "desktop-control"
//! Longest key (an app id), terminator excluded.
#define U_DESKTOP_GRANT_KEY_MAX 200
//! Largest saved-grant file read back.
#define U_DESKTOP_GRANT_MAX_BYTES (64u * 1024u)

//! The saved-grant directory (not created).
bool
u_desktop_grant_dir(char *out, size_t cap);

/*!
 * True when @p key can name a saved grant: 1..U_DESKTOP_GRANT_KEY_MAX bytes of
 * [A-Za-z0-9._-], not starting with '.' (an app id passes; a path never does).
 */
bool
u_desktop_grant_key_valid(const char *key);

//! `<grant dir>/<key>.json`. False for an invalid key.
bool
u_desktop_grant_path(const char *key, char *out, size_t cap);

enum u_desktop_grant_read_result
{
	U_DESKTOP_GRANT_READ_NONE = 0, //!< no file (or no directory, or an invalid key)
	U_DESKTOP_GRANT_READ_OK,       //!< *out_text is the content (malloc'd, NUL-terminated)
	//! Not a private regular file of this user (a symlink, another owner, group /
	//! other bits, too large) or its directory is not private: ignored, left alone.
	U_DESKTOP_GRANT_READ_UNSAFE,
};

enum u_desktop_grant_read_result
u_desktop_grant_read(const char *key, char **out_text);

//! Replace the file for @p key with @p text (see the rules above).
bool
u_desktop_grant_write(const char *key, const char *text);

//! Remove the file for @p key. True if a file was removed.
bool
u_desktop_grant_remove(const char *key);

//! True when a file for @p key exists (whatever its content or permissions).
bool
u_desktop_grant_exists(const char *key);

//! Remove every saved grant (the kill switch). Returns how many were removed.
uint32_t
u_desktop_grant_remove_all(void);

//! How many saved grants there are.
uint32_t
u_desktop_grant_count(void);

typedef void (*u_desktop_grant_key_fn)(void *ctx, const char *key);

//! Call @p fn with the key of every saved grant (sorted by name). Returns how many.
uint32_t
u_desktop_grant_for_each_key(u_desktop_grant_key_fn fn, void *ctx);


/*
 *
 * Audit log.
 *
 */

/*!
 * One audit record. Every pointer may be NULL (field omitted); numeric fields
 * have an explicit presence flag or sentinel. There is deliberately no text
 * field: typed text is represented by @ref chars only.
 */
struct u_desktop_audit_record
{
	const char *exe;    //!< the acting app's executable (NULL: this process's)
	uint32_t pid;       //!< 0: this process
	const char *action; //!< "type_text", "click", ..., "request", "granted", "denied",
	                    //!< "suspended", "resumed", "allow", "deny", "forget"
	bool has_target;
	uint64_t target_id;
	const char *target_app;
	const char *target_title;
	bool has_point;
	int32_t x, y;
	int64_t chars;       //!< TYPE_TEXT: code points typed; < 0 = absent
	const char *keys;    //!< KEY_CHORD, canonical form (shortcuts only, see u_desktop_control.c)
	const char *result;  //!< "ok", "refused", "not_granted", "window_lost", ...
	const char *message; //!< human detail on failure
	const char *reason;  //!< app-supplied consent reason ("request")
	const char *by;      //!< writer of a store change ("cli", "prompt")
	const char *subject; //!< the executable a store change is about (allow/deny/forget)
	//! Spec v2: "control" / "capture" / "all" — consent records and store changes.
	const char *scope;

	// ---- spec v2: one record per capture BURST (spec §10.4). Never a pixel,
	// ---- a thumbnail or a hash: only what was captured, how big, how often.
	const char *target_kind; //!< "desktop" / "region" / "window" (target object)
	bool has_rect;
	int32_t rx, ry, rw, rh; //!< the source rect, desktop coordinates ("rect")
	bool has_size;
	uint32_t w, h;       //!< output image size
	uint32_t count;      //!< captures in the burst (0: field absent)
	const char *first_t; //!< UTC time of the burst's first / last capture
	const char *last_t;
};

//! Now, as the audit log writes it ("2026-10-16T18:02:11Z").
void
u_desktop_audit_utc_now(char *out, size_t cap);

//! Append one line (rotating at 1 MiB). Creates the directory. Best effort.
bool
u_desktop_audit_append(const struct u_desktop_audit_record *r);

typedef void (*u_desktop_audit_line_fn)(void *ctx, const char *line);

/*!
 * Call @p fn for the last @p n lines (oldest first), reading the rotated
 * `.1` file too when the current one has fewer. Returns false if no log exists.
 */
bool
u_desktop_audit_tail(uint32_t n, u_desktop_audit_line_fn fn, void *ctx);


/*
 *
 * Small portable helpers shared by the desktop-control code.
 *
 */

//! This process's id.
uint32_t
u_desktop_self_pid(void);

/*!
 * This process's executable (full path, UTF-8) and a display name for a
 * prompt: the bundle name on macOS when inside an `.app`, else the exe base
 * name without extension.
 */
bool
u_desktop_self_identity(char *exe, size_t exe_cap, char *name, size_t name_cap);

//! Derive the display name from an executable path (see above).
void
u_desktop_display_name_from_exe(const char *exe, char *name, size_t name_cap);

#ifdef __cplusplus
}
#endif
