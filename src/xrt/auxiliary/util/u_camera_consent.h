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
 * Decision order at every start / calibration read (first hit wins):
 *   1. sharing off (DXR_STEREO_CAMERA=0 kill switch, or the user's "Share the
 *      3D camera with apps" toggle)                            -> DISABLED
 *   2. DXR_STEREO_CAMERA_DEV_ALLOW=1 (dev override, logged once) -> ALLOWED
 *   3. no verifiable peer executable                           -> REFUSED
 *   4. registered consent-delegating client (the browser)       -> ALLOWED
 *      (its own per-origin prompt + indicator is the consent)
 *   5. OS camera privacy denies this executable                -> OS_DENIED
 *   6. stored per-app decision (Allow / Deny)                  -> as stored
 *   7. "Allow once" granted earlier to this same process       -> ALLOWED
 *   8. tray prompt ("<app> wants to use the 3D camera —
 *      Allow / Allow once / Deny"): Allow + Deny are written to the store,
 *      Allow once is remembered for the process; no prompt available
 *      (headless, DXR_STEREO_CAMERA_PROMPT=0) or unanswered in time -> REFUSED
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
	//! given RAW frames (it exposes cameras to third-party content).
	bool delegating;
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
	bool (*is_delegating)(void *ctx, const char *exe);
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
 */
void
u_camera_consent_evaluate(struct u_camera_consent *c,
                          const char *exe,
                          const char *app_name,
                          long pid,
                          struct u_camera_consent_decision *out);

//! Human-readable rule name for @ref u_camera_consent_why.
const char *
u_camera_consent_why_str(enum u_camera_consent_why why);

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
 * The real store (u_camera_consent_store.c).
 *
 */

//! The platform store: Windows registry / POSIX JSON. @p ctx is unused (NULL).
const struct u_camera_consent_store_ops *
u_camera_consent_store_default(void);

/*!
 * Add / remove an executable from the USER-level delegating list (the
 * machine-level list is the installer's). `displayxr-cli camera trust`.
 */
bool
u_camera_consent_store_set_delegating(const char *exe, bool delegating);

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

#ifdef __cplusplus
}
#endif
