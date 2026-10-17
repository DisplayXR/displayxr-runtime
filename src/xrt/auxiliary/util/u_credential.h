// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Read a named secret from the OS credential store (ADR-057 §2, §5).
 *
 * A remote conversion endpoint's bearer token, or a gated model repository's
 * token, is referenced BY NAME from settings and pack manifests; the secret
 * itself lives only in the OS credential store and is read here, at the moment
 * it is needed. Never from the registry, a settings file, a manifest or the
 * environment.
 *
 * | OS      | Store                                   | How a user adds one                                     |
 * |---------|-----------------------------------------|---------------------------------------------------------|
 * | Windows | Credential Manager, generic credential  | `cmdkey /generic:<name> /user:displayxr /pass:<token>`  |
 * |         | (CredReadW, target = name)              |                                                         |
 * | macOS   | login Keychain, generic password        | `security add-generic-password -s <name> -a displayxr   |
 * |         | (SecItemCopyMatching, service = name)   |  -w <token> -T <the binary that reads it>`              |
 * | Linux   | Secret Service through libsecret        | `secret-tool store --label='DisplayXR <name>'           |
 * |         | (loaded at run time; attribute service) |  service <name>`                                        |
 *
 * macOS never shows a prompt from here (UI is disallowed): an item whose
 * access list does not include the calling binary reads as DENIED, with a
 * message saying so. Linux without libsecret is UNSUPPORTED (a clear error,
 * not a silent fallback to anything else).
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Longest credential name accepted (UTF-8, NUL excluded).
#define U_CREDENTIAL_NAME_MAX 127
//! Largest secret returned (bytes, NUL excluded).
#define U_CREDENTIAL_SECRET_MAX 4095

enum u_credential_result
{
	U_CREDENTIAL_OK = 0,
	U_CREDENTIAL_NOT_FOUND,   //!< no credential of that name
	U_CREDENTIAL_DENIED,      //!< it exists but this process may not read it (macOS access list)
	U_CREDENTIAL_UNSUPPORTED, //!< no credential store on this platform (Linux without libsecret)
	U_CREDENTIAL_BAD_NAME,    //!< empty, too long, or a control character
	U_CREDENTIAL_TOO_LONG,    //!< the secret does not fit the caller's buffer
	U_CREDENTIAL_ERROR,       //!< the store failed; @p err says how
	U_CREDENTIAL_BAD_SECRET,  //!< the secret holds a control character (CR / LF / NUL ...): refused
};

/*! Are the @p n bytes of @p s a usable secret (non-empty, no control
 *  character, so it can never inject a header line)? */
bool
u_credential_secret_valid_n(const char *s, size_t n);

/*!
 * Read the secret stored under @p name into @p out (NUL-terminated, trailing
 * CR / LF trimmed). On anything but OK, @p out is zeroed and @p err (may be
 * NULL) holds a one-line, secret-free message for the user.
 */
enum u_credential_result
u_credential_read(const char *name, char *out, size_t out_size, char *err, size_t err_size);

/*!
 * Store @p secret under @p name in the OS credential store (replacing one of
 * the same name), as THIS program. Used by `displayxr-cli conv credential set`:
 * on macOS an item a DisplayXR binary created is readable by the DisplayXR
 * binaries of the same signer without a prompt, which an item made with the
 * `security` tool is not (its partition list names only Apple's tools).
 */
enum u_credential_result
u_credential_write(const char *name, const char *secret, char *err, size_t err_size);

//! Remove the credential @p name (absent = OK).
enum u_credential_result
u_credential_delete(const char *name, char *err, size_t err_size);

//! Short name of @p r ("ok", "not_found", ...).
const char *
u_credential_result_str(enum u_credential_result r);

//! Is @p name acceptable (1..U_CREDENTIAL_NAME_MAX printable bytes)?
bool
u_credential_name_valid(const char *name);

//! Zero @p buf in a way the compiler may not elide (a secret leaving scope).
void
u_credential_wipe(void *buf, size_t size);

/*!
 * TESTS ONLY: replace the OS store with @p fn (NULL restores it). Production
 * code never calls this; it exists so unit tests of a provider that needs a
 * token run without touching the developer's real keychain.
 */
typedef enum u_credential_result (*u_credential_reader_fn)(const char *name, char *out, size_t out_size);
void
u_credential_set_reader_for_tests(u_credential_reader_fn fn);

#ifdef __cplusplus
}
#endif
