// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stereo camera consent on Android (ADR-043 R3 / spec §7, Android rows):
 *         the consent SUBJECT, the built-in trusted list, and the store view
 *         the policy runs over there.
 *
 * Android has no per-app executable: every app is the zygote's app_process,
 * and another uid's /proc entry is hidden. So the u_camera_consent policy is
 * given, in place of an executable path, a SUBJECT that names the peer's
 * package and the SHA-256 of its APK signing certificate:
 *
 *     android:<package>@<64 lowercase hex>
 *
 * The platform (the runtime service's Java side, from the SO_PEERCRED uid of
 * the connection) supplies the package and digest; @ref
 * u_camera_consent_android_identity turns them into a subject or refuses.
 * Everything else — stored Allow / Deny, Allow once, the prompt, the keyed
 * persistentId — is the platform-neutral policy, unchanged, keyed on the
 * subject. A reinstall signed by someone else is therefore a different
 * subject: it inherits no stored decision.
 *
 * The delegating list on Android is BUILT INTO THE RUNTIME (an APK cannot be
 * edited by another installer, and there is no `camera trust`): an entry is a
 * package AND a certificate digest. A package that is on the list but signed
 * by anyone else is not "another app" — it is refused outright (no prompt), so
 * a rebuild under the same package name can never pass for the listed client.
 * Matching entries delegate at SYSTEM scope (no first-use notice). Nothing
 * here names a display vendor: the list names consumer applications.
 *
 * Pure (no OS calls): unit-tested on every OS.
 *
 * @ingroup aux_util
 */

#pragma once

#include "util/u_camera_consent.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_CAMERA_CONSENT_ANDROID_PREFIX "android:"
//! Longest package name accepted (Android caps it well below this).
#define U_CAMERA_CONSENT_ANDROID_PACKAGE_MAX 256
//! SHA-256 as lowercase hex, NUL included.
#define U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE 65

//! One built-in trusted (consent-delegating) client.
struct u_camera_consent_android_trusted
{
	const char *package;     //!< e.g. "org.chromium.chrome"
	const char *cert_sha256; //!< 64 hex chars (either case)
	const char *what;        //!< for the log line, e.g. "DisplayXR Browser (release)"
};

//! What @ref u_camera_consent_android_identity decided.
enum u_camera_consent_android_identity_result
{
	U_CAMERA_CONSENT_ANDROID_ID_OK = 0,        //!< a subject was written
	U_CAMERA_CONSENT_ANDROID_ID_NO_PACKAGE,    //!< no (single, valid) package for the uid
	U_CAMERA_CONSENT_ANDROID_ID_NO_CERT,       //!< no usable signing-certificate digest
	U_CAMERA_CONSENT_ANDROID_ID_CERT_MISMATCH, //!< a trusted package name, signed by someone else
};

/*!
 * Is @p pkg a syntactically valid Android package name for a subject
 * ([A-Za-z0-9_.] plus ':' for a shared-uid name, non-empty, bounded)? Rejects
 * '@' and anything a JSON key or a log line could be confused by.
 */
bool
u_camera_consent_android_package_valid(const char *pkg);

/*!
 * Normalise a SHA-256 digest given as 64 hex chars (optionally with ':'
 * separators, as `apksigner` / `keytool` print it) into 64 lowercase hex
 * chars in @p out. false if it is not exactly 32 bytes of hex.
 */
bool
u_camera_consent_android_digest_normalize(const char *in, char out[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE]);

//! "android:<pkg>@<digest>" into @p out. false on an invalid package / digest or no room.
bool
u_camera_consent_android_subject_format(const char *pkg, const char *digest, char *out, size_t cap);

/*!
 * Split a subject back into package and digest. false if @p subject is not
 * an Android subject. Either output may be NULL.
 */
bool
u_camera_consent_android_subject_parse(const char *subject,
                                       char *pkg,
                                       size_t pkg_cap,
                                       char digest[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE]);

//! The trusted list built into this runtime (never NULL; @p out_count may be 0).
const struct u_camera_consent_android_trusted *
u_camera_consent_android_builtin_trusted(uint32_t *out_count);

//! Lookup result in a trusted list.
enum u_camera_consent_android_trust
{
	U_CAMERA_CONSENT_ANDROID_NOT_LISTED = 0, //!< package not on the list
	U_CAMERA_CONSENT_ANDROID_TRUSTED,        //!< package AND certificate match an entry
	U_CAMERA_CONSENT_ANDROID_PACKAGE_ONLY,   //!< listed package, other certificate
};

//! Look (@p pkg, @p digest) up in @p list (@p n entries). @p hit (may be NULL) gets the matching entry.
enum u_camera_consent_android_trust
u_camera_consent_android_trust_lookup(const struct u_camera_consent_android_trusted *list,
                                      uint32_t n,
                                      const char *pkg,
                                      const char *digest,
                                      const struct u_camera_consent_android_trusted **hit);

/*!
 * Turn what the platform resolved for the peer uid into the consent subject.
 * @p pkg / @p digest may be NULL or "" (unknown). A listed package with a
 * different certificate is CERT_MISMATCH: the caller refuses it as an
 * unverifiable peer (spec §7.1 step 3), never prompts for it.
 * @p subject is "" unless OK.
 */
enum u_camera_consent_android_identity_result
u_camera_consent_android_identity(const struct u_camera_consent_android_trusted *list,
                                  uint32_t n,
                                  const char *pkg,
                                  const char *digest,
                                  char *subject,
                                  size_t cap);

//! Human-readable @ref u_camera_consent_android_identity_result.
const char *
u_camera_consent_android_identity_str(enum u_camera_consent_android_identity_result r);

/*!
 * The store the policy runs over on Android: decisions, sharing toggle and
 * secret come from @p base (the runtime's private-storage JSON store); the
 * delegating list is @p list ONLY (the built-in one), at SYSTEM scope. A
 * delegating list in @p base is ignored on Android.
 */
struct u_camera_consent_store_android
{
	struct u_camera_consent_store_ops ops; //!< pass &this->ops with ctx = this
	const struct u_camera_consent_store_ops *base;
	void *base_ctx;
	const struct u_camera_consent_android_trusted *list;
	uint32_t list_count;
};

void
u_camera_consent_store_android_init(struct u_camera_consent_store_android *s,
                                    const struct u_camera_consent_store_ops *base,
                                    void *base_ctx,
                                    const struct u_camera_consent_android_trusted *list,
                                    uint32_t list_count);

/*!
 * Spec §7.2a (Android, option R): may a USER_FACING camera start for a peer?
 * Only while that peer's package presents woven 3D in this service process
 * (a live weave or a visible session), so an app in the background can never
 * take the camera the display's head tracking depends on. Any other camera:
 * always. Pure — the caller supplies @p presenting.
 */
static inline bool
u_camera_consent_user_facing_gate(bool gate_enabled, bool camera_user_facing, bool presenting)
{
	return !gate_enabled || !camera_user_facing || presenting;
}

/*!
 * A short display label for a consent subject or executable path: the
 * package for an Android subject, the base name for a path ("" -> "").
 */
void
u_camera_consent_subject_label(const char *subject, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
