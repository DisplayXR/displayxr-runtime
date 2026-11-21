// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The per-user store of PENDING ACTIVATIONS behind the
 *         `displayxr://activate` deep link (ADR-064 §5,
 *         `docs/specs/runtime/activation-deep-link.md`).
 *
 * An activation is started on purpose (`displayxr-cli <class> activate-begin
 * <id>`, the Dashboard's Buy / Activate action): that creates a one-time
 * NONCE bound to one component and one user, valid for a few minutes. The
 * deep link a vendor's checkout returns carries the nonce; the handler
 * CONSUMES it here before anything reaches a plug-in. A link whose nonce is
 * not pending does nothing, which is what keeps an arbitrary web page from
 * pushing data at a plug-in.
 *
 * The store is a directory of small files, one per pending nonce, in the
 * user's own profile:
 *
 * | Platform | Directory |
 * |---|---|
 * | Windows | `%LOCALAPPDATA%\DisplayXR\Activation` (the profile's ACL: the user, SYSTEM, Administrators) |
 * | macOS | `~/Library/Application Support/DisplayXR/activation` (0700) |
 * | Linux | `$XDG_STATE_HOME/displayxr/activation`, else `~/.local/state/displayxr/activation` (0700) |
 *
 * What is on disk: the SHA-256 of the nonce as the file name (the nonce
 * itself is never written), and inside it the component class and id, the
 * creating user, the creation and expiry times. No token, ever: the store is
 * touched before the token is looked at and never sees it.
 *
 * Rules, all enforced here:
 *  - single use: @ref u_activation_store_consume claims the record with an
 *    atomic rename and deletes it WHATEVER the outcome, so a nonce is spent by
 *    its first presentation, valid or not;
 *  - short-lived: a record past its expiry (or dated in the future) is
 *    refused, and pruned by the next begin;
 *  - one user: the record names the user that created it and is refused for
 *    any other; on POSIX the directory and the file must also be owned by the
 *    caller and closed to group and others;
 *  - bounded: at most @ref U_ACTIVATION_MAX_PENDING records per user;
 *  - failures are counted (@ref u_activation_store_note_failure) so the
 *    handler can stop answering a flood (@ref u_activation_store_rate_limited).
 *
 * Every function takes the directory, so tests run against a scratch one;
 * the handler passes @ref u_activation_store_default_dir, which is derived
 * from the profile variables alone (`LOCALAPPDATA`, `HOME`, `XDG_STATE_HOME`).
 * There is no DisplayXR-specific override to point the handler elsewhere.
 *
 * @ingroup aux_util
 */

#pragma once

#include "util/u_activation_url.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_ACTIVATION_TTL_S 900         //!< a pending activation lives 15 minutes
#define U_ACTIVATION_MAX_PENDING 16    //!< pending activations per user
#define U_ACTIVATION_FAIL_WINDOW_S 600 //!< failures are counted over 10 minutes
#define U_ACTIVATION_FAIL_LIMIT 5      //!< this many failed links in the window stop the handler answering
#define U_ACTIVATION_NONCE_ID_CHARS 8  //!< the loggable id of a nonce (hex prefix of its hash)

//! Outcome of a store call. Stable names: @ref u_activation_store_result_str.
enum u_activation_store_result
{
	U_ACTIVATION_STORE_OK = 0,
	U_ACTIVATION_STORE_NOT_FOUND,    //!< no pending record for that nonce (never begun, already used, pruned)
	U_ACTIVATION_STORE_EXPIRED,      //!< found (and now deleted), but past its expiry
	U_ACTIVATION_STORE_WRONG_USER,   //!< found (and now deleted), but begun by another user
	U_ACTIVATION_STORE_CORRUPT,      //!< found (and now deleted), but not a record this module wrote
	U_ACTIVATION_STORE_TOO_MANY,     //!< begin: @ref U_ACTIVATION_MAX_PENDING live records already
	U_ACTIVATION_STORE_BAD_ARGUMENT, //!< a class / id / nonce / directory that is not well-formed
	U_ACTIVATION_STORE_IO_ERROR,     //!< the directory is unusable (missing profile, wrong owner, no entropy)
};

//! One pending activation, as begun.
struct u_activation_record
{
	char cls[U_ACTIVATION_CLASS_MAX];               //!< "dp" / "input" / "conv" / "tracking"
	char id[U_ACTIVATION_ID_MAX];                   //!< the component's registration id
	char nonce_id[U_ACTIVATION_NONCE_ID_CHARS + 1]; //!< loggable; says nothing about the nonce
	int64_t created_unix_s;
	int64_t expires_unix_s;
};

//! "ok" / "not-found" / "expired" / "wrong-user" / "corrupt" / "too-many" / "bad-argument" / "io-error".
const char *
u_activation_store_result_str(enum u_activation_store_result r);

//! The per-user directory of the table above into @p out. False when the profile cannot be located.
bool
u_activation_store_default_dir(char *out, size_t cap);

//! The loggable id of @p nonce (8 hex characters of its SHA-256) into @p out; "" for a malformed nonce.
void
u_activation_nonce_id(const char *nonce, char out[U_ACTIVATION_NONCE_ID_CHARS + 1]);

/*!
 * Begin an activation of component @p id of class @p cls: prune what
 * expired, refuse when the user already has @ref U_ACTIVATION_MAX_PENDING
 * pending, then create a record valid for @p ttl_s seconds from @p now_unix_s
 * and return its nonce (43 characters + NUL) in @p out_nonce and the record
 * in @p out (may be NULL). Creates @p dir (owner-only) when missing.
 */
enum u_activation_store_result
u_activation_store_begin(const char *dir,
                         const char *cls,
                         const char *id,
                         int64_t now_unix_s,
                         int64_t ttl_s,
                         char out_nonce[U_ACTIVATION_NONCE_CHARS + 1],
                         struct u_activation_record *out);

/*!
 * Consume @p nonce: claim its record, delete it, and only then judge it. On
 * @ref U_ACTIVATION_STORE_OK @p out holds what the activation was begun for.
 * On every other result the record (if there was one) is gone all the same.
 */
enum u_activation_store_result
u_activation_store_consume(const char *dir, const char *nonce, int64_t now_unix_s, struct u_activation_record *out);

//! Delete expired records and stale claims; returns how many live records remain, or -1 when @p dir is unusable.
int
u_activation_store_prune(const char *dir, int64_t now_unix_s);

//! Count one failed link at @p now_unix_s. Best effort; never fails the caller.
void
u_activation_store_note_failure(const char *dir, int64_t now_unix_s);

//! Have @ref U_ACTIVATION_FAIL_LIMIT links failed within @ref U_ACTIVATION_FAIL_WINDOW_S of @p now_unix_s?
bool
u_activation_store_rate_limited(const char *dir, int64_t now_unix_s);

/*!
 * Append one line to the store's `activation.log` (truncated past 64 KB):
 * time, nonce id, class, id, outcome. The caller passes NO token and no part
 * of a link; every field is filtered to a safe character set here.
 */
void
u_activation_store_log(
    const char *dir, int64_t now_unix_s, const char *nonce_id, const char *cls, const char *id, const char *outcome);

//! The current wall-clock time, seconds since the Unix epoch.
int64_t
u_activation_now_unix_s(void);

#ifdef __cplusplus
}
#endif
