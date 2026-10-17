// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The desktop-control user store and audit log (XR_DXR_desktop_control
 *         spec §6, ADR-056 D3.6).
 *
 * Store — one JSON file per user, `desktop_control.json`:
 *
 *     { "version": 1, "suspended": false, "idle_ms": 600,
 *       "apps": { "<executable full path>": "allow" | "deny" } }
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
 * `DXR_DESKTOP_CONTROL_DIR`, when set and non-empty, overrides BOTH
 * directories (tests, CI, a portable install).
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

enum u_desktop_app_decision
u_desktop_policy_app(const struct u_desktop_policy *p, const char *exe);

typedef void (*u_desktop_policy_app_fn)(void *ctx, const char *exe, enum u_desktop_app_decision d);

void
u_desktop_policy_for_each_app(const struct u_desktop_policy *p, u_desktop_policy_app_fn fn, void *ctx);


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

//! Flip the kill switch. Audited as "suspended" / "resumed".
bool
u_desktop_policy_set_suspended(bool suspended, const char *by);


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
};

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
