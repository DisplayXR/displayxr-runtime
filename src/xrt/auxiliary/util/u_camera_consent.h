// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stereo camera consent policy + store (ADR-043 R3, spec §7).
 *
 * The POLICY is platform-neutral and takes every fact it needs through two
 * injected vtables, so the decision tree can be unit-tested with fakes:
 *  - @ref u_camera_consent_store_ops — the persistent per-user store (Windows:
 *    HKCU\Software\DisplayXR\CameraConsent; POSIX: camera_consent.json in the
 *    user config dir) plus the registered consent-DELEGATING client list
 *    (installer-written under HKLM / a system JSON, or the user list written by
 *    `displayxr-cli camera trust`). u_camera_consent_store_default() is the
 *    real one; the CLI uses it directly.
 *  - @ref u_camera_consent_env_ops — what only the service process knows: the
 *    OS camera privacy switch for a given executable and the tray prompt.
 *
 * Decision order at every start / calibration read (first hit wins). This is
 * the one statement of it in code; spec §7.1 is the normative one:
 *   1. sharing off (DXR_STEREO_CAMERA=0 kill switch, or the user's "Share the
 *      3D camera with apps" toggle)                            -> DISABLED
 *   2. DXR_STEREO_CAMERA_DEV_ALLOW=1 (dev override, logged once) -> ALLOWED
 *   3. no verifiable peer executable                           -> REFUSED
 *   4. OS camera privacy denies this executable                -> OS_DENIED
 *   5. stored per-app Deny                                     -> REFUSED
 *   6. registered consent-delegating client (a browser)         -> ALLOWED
 *      (its own per-origin prompt + indicator is the consent) — unless the
 *      client DECLINED delegation for this instance
 *      (U_CAMERA_CONSENT_FLAG_DECLINE_DELEGATION), or the entry's executable
 *      lives in a USER-WRITABLE location and is not Authenticode-signed by
 *      the signer the entry names (see "Trusting a delegating entry" below)
 *   7. stored per-app Allow                                    -> ALLOWED
 *   8. "Allow once" granted earlier to this same process       -> ALLOWED
 *   9. tray prompt ("<app> wants to use the 3D camera —
 *      Allow / Allow once / Deny"): Allow + Deny are written to the store,
 *      Allow once is remembered for the process; no prompt available
 *      (headless, DXR_STEREO_CAMERA_PROMPT=0) or unanswered in time -> REFUSED
 *
 * Delegation (step 6) means exactly "no runtime prompt and no stored per-app
 * decision needed". It never outranks a refusal: the kill switches, the OS
 * switch and a stored Deny apply to a delegating client like to any other.
 * The OS switch matters most for it: the service, not the client, opens the
 * camera, so the OS never sees the consumer and nothing else enforces it.
 *
 * Trusting a delegating entry (spec §7.1.1). An entry names an executable by
 * full path. Where that path is admin-protected (Windows: under
 * %ProgramFiles%, %ProgramFiles(x86)% or %SystemRoot%) the path alone is
 * trusted: whoever can replace the file could also have written the entry.
 * Anywhere else (a per-user install in %LOCALAPPDATA%, a dev tree, Downloads)
 * any process of the user could drop a different binary at that path, so the
 * entry must also record the required signer (its CN) and the executable must
 * carry a valid Authenticode signature by exactly that signer. An entry that
 * fails this is treated as if it did not exist for step 6 (stored decision /
 * prompt instead), with one WARN per executable per service run naming why.
 * POSIX: delegation is path-only for now (path_user_writable is wired to
 * "never"; there is no platform code-signature check yet).
 *
 * A USER-level entry (HKCU / the user JSON — `displayxr-cli camera trust`, or
 * anything else running as the user) is honoured, but the first time it
 * actually allows a given executable in a service run the policy logs a WARN
 * and raises env->user_delegation_notice (the tray balloon), so a silent
 * self-registration cannot go unnoticed. A SYSTEM-level (installer) entry is
 * silent.
 *
 * The keyed persistentId (§7.5) also lives here: HMAC-SHA-256 under a 32-byte
 * secret the store generates once per user, so a page cannot derive the
 * device from the id and two installs never share one.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_CAMERA_CONSENT_EXE_MAX 512
#define U_CAMERA_CONSENT_SECRET_SIZE 32
//! "Allow once" grants remembered per (executable, pid) for the service's lifetime.
#define U_CAMERA_CONSENT_ONCE_MAX 16
//! Executables remembered for the once-per-run WARN / notice (oldest overwritten).
#define U_CAMERA_CONSENT_SEEN_MAX 16
//! A signer common name (Authenticode subject CN), UTF-8, NUL included.
#define U_CAMERA_CONSENT_SIGNER_MAX 256

/*!
 * @ref u_camera_consent_evaluate flags (the service maps them from the
 * client's XrStereoCameraClientInfoDXR::flags). Unknown bits are ignored.
 */
//! The client declined delegation for this instance (XR_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION_BIT_DXR).
#define U_CAMERA_CONSENT_FLAG_DECLINE_DELEGATION (1u << 0)

//! Which list a delegating entry came from.
enum u_camera_consent_delegation_scope
{
	U_CAMERA_CONSENT_DELEGATION_NONE = 0,   //!< not registered
	U_CAMERA_CONSENT_DELEGATION_SYSTEM = 1, //!< machine-level (installer): HKLM / system JSON
	U_CAMERA_CONSENT_DELEGATION_USER = 2,   //!< user-level: HKCU / the user JSON
};

//! A registered delegating entry for one executable.
struct u_camera_consent_delegation
{
	enum u_camera_consent_delegation_scope scope;
	//! The signer the entry requires (Windows: the `<valueName>.signer` REG_SZ
	//! next to the entry). "" = none recorded. Only consulted when the
	//! executable's path is user-writable.
	char signer[U_CAMERA_CONSENT_SIGNER_MAX];
};

//! Why a registered entry did NOT delegate (for the log line and the tests).
enum u_camera_consent_delegation_skip
{
	U_CAMERA_CONSENT_DELEGATION_APPLIES = 0,     //!< not skipped (or not registered at all)
	U_CAMERA_CONSENT_DELEGATION_DECLINED,        //!< the client set DECLINE_DELEGATION
	U_CAMERA_CONSENT_DELEGATION_NO_SIGNER,       //!< user-writable path, the entry records no signer
	U_CAMERA_CONSENT_DELEGATION_NOT_SIGNED,      //!< user-writable path, no valid signature
	U_CAMERA_CONSENT_DELEGATION_SIGNER_MISMATCH, //!< validly signed, by someone else
};

//! A stored per-executable decision.
enum u_camera_consent_stored
{
	U_CAMERA_CONSENT_STORED_NONE = 0,
	U_CAMERA_CONSENT_STORED_ALLOW = 1,
	U_CAMERA_CONSENT_STORED_DENY = 2,
};

//! What the prompt came back with.
enum u_camera_consent_prompt_answer
{
	U_CAMERA_CONSENT_PROMPT_UNAVAILABLE = 0, //!< no UI (headless service, prompt disabled)
	U_CAMERA_CONSENT_PROMPT_ALLOW = 1,
	U_CAMERA_CONSENT_PROMPT_ALLOW_ONCE = 2,
	U_CAMERA_CONSENT_PROMPT_DENY = 3,
	U_CAMERA_CONSENT_PROMPT_TIMEOUT = 4, //!< shown, not answered in time
};

//! The outcome a caller acts on (maps 1:1 to the XR results, spec §8).
enum u_camera_consent_verdict
{
	U_CAMERA_CONSENT_ALLOWED = 0,
	U_CAMERA_CONSENT_REFUSED = 1,   //!< XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR
	U_CAMERA_CONSENT_DISABLED = 2,  //!< XR_ERROR_STEREO_CAMERA_DISABLED_DXR
	U_CAMERA_CONSENT_OS_DENIED = 3, //!< XR_ERROR_PERMISSION_INSUFFICIENT
};

//! Which rule decided (for the log line and the tests).
enum u_camera_consent_why
{
	U_CAMERA_CONSENT_WHY_KILL_SWITCH = 0,
	U_CAMERA_CONSENT_WHY_SHARING_OFF,
	U_CAMERA_CONSENT_WHY_DEV_OVERRIDE,
	U_CAMERA_CONSENT_WHY_NO_IDENTITY,
	U_CAMERA_CONSENT_WHY_DELEGATING,
	U_CAMERA_CONSENT_WHY_OS_DENIED,
	U_CAMERA_CONSENT_WHY_STORED_ALLOW,
	U_CAMERA_CONSENT_WHY_STORED_DENY,
	U_CAMERA_CONSENT_WHY_ALLOW_ONCE,
	U_CAMERA_CONSENT_WHY_PROMPT_ALLOW,
	U_CAMERA_CONSENT_WHY_PROMPT_ALLOW_ONCE,
	U_CAMERA_CONSENT_WHY_PROMPT_DENY,
	U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE,
	U_CAMERA_CONSENT_WHY_PROMPT_TIMEOUT,
};

struct u_camera_consent_decision
{
	enum u_camera_consent_verdict verdict;
	enum u_camera_consent_why why;
	//! The executable is a registered consent-delegating client: it shows its
	//! own prompt + indicator, follows its own visibility rule, and is never
	//! given RAW frames (it exposes cameras to third-party content). Set from
	//! step 3 on whatever the verdict (it is a fact about the executable);
	//! the dev override (step 2) and a missing identity leave it false. It
	//! means REGISTERED: the RAW refusal keys on it even when the entry was
	//! not trusted or the client declined delegation (fails closed).
	bool delegating;
	//! Step 6 decided: the delegation was trusted and applied (not declined,
	//! signer check passed). The service exempts only such a stream from the
	//! foreground rule; a declined / untrusted one is an ordinary app.
	bool delegated;
	//! @ref delegating but step 6 did not apply it: why (APPLIES otherwise,
	//! and whenever an earlier step decided).
	enum u_camera_consent_delegation_skip delegation_skip;
	//! The list the matching entry came from (NONE when not registered).
	enum u_camera_consent_delegation_scope delegation_scope;
};

/*!
 * The persistent store. Every call takes the full, OS-derived executable path
 * of the peer. All functions may be NULL (treated as: no entry / not
 * delegating / sharing on / no secret).
 */
struct u_camera_consent_store_ops
{
	bool (*get)(void *ctx, const char *exe, enum u_camera_consent_stored *out);
	//! NONE removes the entry. Returns false if the store could not be written.
	bool (*set)(void *ctx, const char *exe, enum u_camera_consent_stored value);
	//! The delegating entry for @p exe: false (or scope NONE) = not registered.
	//! A machine-level entry wins over a user-level one for the same path.
	bool (*get_delegation)(void *ctx, const char *exe, struct u_camera_consent_delegation *out);
	//! The user's "Share the 3D camera with apps" toggle (default on).
	bool (*sharing_enabled)(void *ctx);
	bool (*set_sharing_enabled)(void *ctx, bool enabled);
	//! The per-user secret, generated on first use. false = no store (the id
	//! is then keyed with a per-process random key: stable for the service's
	//! lifetime, never across restarts, which is the safe failure).
	bool (*get_secret)(void *ctx, uint8_t out[U_CAMERA_CONSENT_SECRET_SIZE]);
};

/*!
 * What the hosting process supplies. NULL = OS allows / no prompt available.
 */
struct u_camera_consent_env_ops
{
	//! The OS camera privacy switch for @p exe (Windows: CapabilityAccessManager
	//! ConsentStore\webcam, global + desktop-app + per-app). true elsewhere.
	bool (*os_camera_allowed)(void *ctx, const char *exe);
	//! Show the consent prompt and BLOCK until answered or @p timeout_ms. The
	//! caller holds no locks of the policy while this runs.
	enum u_camera_consent_prompt_answer (*prompt)(
	    void *ctx, const char *exe, const char *app_name, long pid, uint32_t timeout_ms);
	//! Could a non-admin process replace @p exe (Windows: not under
	//! %ProgramFiles%, %ProgramFiles(x86)%, %SystemRoot%)? NULL = assume YES
	//! (fails closed: a host that wires nothing gets signer-checked
	//! delegation, which without @ref exe_signer means no delegation).
	bool (*path_user_writable)(void *ctx, const char *exe);
	//! Does @p exe carry a VALID code signature? If so, its signer CN into
	//! @p out. NULL = nothing can be verified.
	bool (*exe_signer)(void *ctx, const char *exe, char *out, size_t cap);
	//! A USER-level delegating entry just allowed @p exe for the first time in
	//! this service run (the tray balloon). Never called for SYSTEM entries.
	//! Called with the caller's consent serialisation held: must not block.
	void (*user_delegation_notice)(void *ctx, const char *exe, const char *app_name);
};

struct u_camera_consent
{
	const struct u_camera_consent_store_ops *store;
	void *store_ctx;
	const struct u_camera_consent_env_ops *env;
	void *env_ctx;

	bool kill_switch;    //!< DXR_STEREO_CAMERA=0
	bool dev_override;   //!< DXR_STEREO_CAMERA_DEV_ALLOW=1
	bool prompt_enabled; //!< DXR_STEREO_CAMERA_PROMPT (default on)
	uint32_t prompt_timeout_ms;
	bool dev_override_logged;

	struct
	{
		char exe[U_CAMERA_CONSENT_EXE_MAX];
		long pid;
	} once[U_CAMERA_CONSENT_ONCE_MAX];
	uint32_t once_count;

	//! Executables already WARNed about an untrusted delegating entry.
	char untrusted_warned[U_CAMERA_CONSENT_SEEN_MAX][U_CAMERA_CONSENT_EXE_MAX];
	uint32_t untrusted_warned_count;
	//! Executables already noticed for a user-level delegation.
	char user_noticed[U_CAMERA_CONSENT_SEEN_MAX][U_CAMERA_CONSENT_EXE_MAX];
	uint32_t user_noticed_count;

	//! Secret for persistentId: the store's, else a per-process random one.
	uint8_t secret[U_CAMERA_CONSENT_SECRET_SIZE];
	bool secret_ready;
};

void
u_camera_consent_init(struct u_camera_consent *c,
                      const struct u_camera_consent_store_ops *store,
                      void *store_ctx,
                      const struct u_camera_consent_env_ops *env,
                      void *env_ctx);

/*!
 * Run the decision tree for one peer. Not thread-safe by itself: the caller
 * serialises evaluations (the service does so per stream start) and holds no
 * lock the prompt might need. @p exe may be "" (no verifiable identity).
 * @p flags: U_CAMERA_CONSENT_FLAG_*; unknown bits are ignored.
 */
void
u_camera_consent_evaluate(struct u_camera_consent *c,
                          const char *exe,
                          const char *app_name,
                          long pid,
                          uint32_t flags,
                          struct u_camera_consent_decision *out);

//! Human-readable rule name for @ref u_camera_consent_why.
const char *
u_camera_consent_why_str(enum u_camera_consent_why why);

//! Human-readable reason for @ref u_camera_consent_delegation_skip ("" for APPLIES).
const char *
u_camera_consent_delegation_skip_str(enum u_camera_consent_delegation_skip skip);

//! Is @p exe registered in either delegating list (whether or not it would be trusted)?
bool
u_camera_consent_is_registered_delegating(const struct u_camera_consent_store_ops *store,
                                          void *store_ctx,
                                          const char *exe);

//! Forget every "Allow once" grant (e.g. when the user stops sharing).
void
u_camera_consent_forget_once(struct u_camera_consent *c);

//! Is sharing on right now (kill switch + user toggle)?
bool
u_camera_consent_sharing_enabled(struct u_camera_consent *c);

/*!
 * The keyed persistentId (spec §7.5): HMAC-SHA-256(secret, device | consumer),
 * rendered as "dxrcam-" + 32 hex chars into @p out[64]. The same (secret,
 * device, consumer) always gives the same id; any other secret gives an
 * unrelated one; the id reveals nothing about the device.
 */
void
u_camera_consent_persistent_id(struct u_camera_consent *c,
                               const char *device_identity,
                               const char *consumer_exe,
                               char out[64]);

//! Same, with an explicit key (tests, and the pure form of the above).
void
u_camera_consent_persistent_id_keyed(
    const uint8_t *key, size_t key_len, const char *device_identity, const char *consumer_exe, char out[64]);


/*
 *
 * Pure helpers the platform store is built on (unit-tested on every OS).
 *
 */

/*!
 * Do two executable paths name the same file the way the store compares them?
 * @p windows_rules: ASCII case-insensitive, either slash a separator (Windows); else
 * byte-exact (POSIX). Non-ASCII bytes always compare exactly — a UTF-8 path
 * that differs only in non-ASCII letter case does NOT match (fails closed for
 * a delegating entry; the Windows Apps lookup uses the registry's own
 * case-insensitive value-name match instead). Empty or NULL never matches.
 */
bool
u_camera_consent_path_equal(const char *a, const char *b, bool windows_rules);

/*!
 * Is @p path inside directory @p dir (at a separator boundary)? Same
 * comparison rules as @ref u_camera_consent_path_equal; a trailing separator
 * on @p dir is optional. A path with a ".." component never is (it could
 * climb back out); empty / NULL never is.
 */
bool
u_camera_consent_path_is_under(const char *path, const char *dir, bool windows_rules);

/*!
 * Do two signer names match? ASCII case-insensitive after trimming leading /
 * trailing spaces; non-ASCII bytes compare exactly. Empty / NULL never match.
 */
bool
u_camera_consent_signer_equal(const char *a, const char *b);

//! One step of a list enumerator (see @ref u_camera_consent_list_find).
enum u_camera_consent_enum_result
{
	U_CAMERA_CONSENT_ENUM_ENTRY = 0, //!< name + path were written (NUL-terminated UTF-8)
	U_CAMERA_CONSENT_ENUM_SKIP = 1,  //!< this index is unusable (oversized, wrong type, not
	                                 //!< convertible): ignore it and go on with the next
	U_CAMERA_CONSENT_ENUM_END = 2,   //!< no entry at this index or after it (or the list
	                                 //!< cannot be read any further)
};

/*!
 * Read entry @p index of a delegating list: @p name is the entry's id (the
 * registry value name; "" where the list has none), @p path its executable.
 */
typedef enum u_camera_consent_enum_result (*u_camera_consent_enum_fn)(
    void *ctx, uint32_t index, char *name, size_t name_cap, char *path, size_t path_cap);

//! Upper bound on the indices @ref u_camera_consent_list_find visits.
#define U_CAMERA_CONSENT_LIST_MAX 4096

/*!
 * Walk a delegating list for @p exe. An unusable entry (SKIP) is passed over,
 * never ends the scan — one oversized value an installer wrote must not hide
 * every entry after it. On a match, the entry's name is copied to
 * @p match_name (if non-NULL). Stops at END or after U_CAMERA_CONSENT_LIST_MAX
 * indices.
 */
bool
u_camera_consent_list_find(
    u_camera_consent_enum_fn fn, void *ctx, const char *exe, bool windows_rules, char *match_name, size_t match_cap);


/*
 *
 * The real store (u_camera_consent_store.c).
 *
 */

//! The platform store: Windows registry / POSIX JSON. @p ctx is unused (NULL).
const struct u_camera_consent_store_ops *
u_camera_consent_store_default(void);

/*!
 * Add / remove an executable from the USER-level delegating list (the
 * machine-level list is the installer's). `displayxr-cli camera trust`.
 * @p signer (NULL / "" = none) is the required signer CN recorded with the
 * entry (Windows: the `<valueName>.signer` REG_SZ next to it); trusting an
 * existing entry replaces its signer. Removing an entry removes its signer.
 * POSIX ignores @p signer (delegation is path-only there).
 */
bool
u_camera_consent_store_set_delegating(const char *exe, bool delegating, const char *signer);

/*!
 * List the store for diagnostics: calls @p cb for every stored app decision
 * (kind 'a' with "allow"/"deny"), every delegating entry (kind 'd', value
 * "user"/"system") and the sharing toggle (kind 's', "on"/"off").
 */
void
u_camera_consent_store_list(void (*cb)(void *ctx, char kind, const char *exe, const char *value), void *ctx);

//! Where the user store lives (for messages); false if unknown.
bool
u_camera_consent_store_path(char *out, size_t cap);

//! The OS camera privacy switch for @p exe (Windows ConsentStore; true elsewhere).
bool
u_camera_consent_os_camera_allowed(const char *exe);

/*!
 * Could a non-admin process replace @p exe? Windows: true unless the full path
 * is under FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86 or FOLDERID_Windows
 * (SHGetKnownFolderPath; ASCII case-insensitive; empty / unresolvable = true).
 * POSIX: false for now — delegation is path-only there (no signature check).
 */
bool
u_camera_consent_path_user_writable(const char *exe);

/*!
 * Windows: does @p exe carry a VALID Authenticode signature (WinVerifyTrust,
 * WINTRUST_ACTION_GENERIC_VERIFY_V2, no UI, no revocation check, cache-only
 * URL retrieval — it never blocks on the network)? If so, the leaf signer's
 * subject CN into @p out. POSIX: always false.
 */
bool
u_camera_consent_exe_signer(const char *exe, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
