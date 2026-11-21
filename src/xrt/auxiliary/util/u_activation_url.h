// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Strict parser for the `displayxr://activate` deep link, and the
 *         builder of the purchase URL an activation starts with (ADR-064 §5,
 *         `docs/specs/runtime/activation-deep-link.md`).
 *
 * The link is reachable from any web page or document on the machine, so this
 * is an allow-list parser, not a general URL parser: one scheme, one action,
 * three known query keys, a closed character set, hard length caps. Anything
 * else is refused with a reason code and nothing is extracted from it. It is
 * pure (no I/O, no allocation, no logging) so it is table-tested.
 *
 * Grammar (ABNF, RFC 3986 character classes):
 *
 *     link       = scheme "://" action [ "/" ] "?" param *( "&" param )
 *     scheme     = "displayxr"                    ; ASCII case-insensitive
 *     action     = "activate"                     ; exact, the only action
 *     param      = "nonce=" nonce / "token=" token / "item=" item
 *     nonce      = 43( ALPHA / DIGIT / "-" / "_" )
 *     token      = 1*( unreserved / pct-encoded )
 *     item       = class ( "/" / "%2F" ) id
 *     class      = "dp" / "input" / "conv" / "tracking"
 *     id         = 1*63( ALPHA / DIGIT / "." / "_" / "-" )
 *     unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~"
 *
 * `nonce` and `token` are required, `item` is optional, each key appears at
 * most once, in any order, and no other key is accepted. The whole link is at
 * most @ref U_ACTIVATION_URL_MAX bytes. The decoded token is 1 ..
 * `XRT_COMPONENT_ACTIVATION_TOKEN_MAX - 1` bytes of well-formed UTF-8 with no
 * control character (U+0000-U+001F, U+007F-U+009F). No user-info, port,
 * fragment, `+`-as-space, quote, space or raw non-ASCII byte is accepted
 * anywhere.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Longest link accepted, in bytes (NUL excluded). A 2047-byte token, fully percent-encoded, is 6141.
#define U_ACTIVATION_URL_MAX 6400u
//! Characters of a nonce: 32 random bytes, base64url without padding.
#define U_ACTIVATION_NONCE_CHARS 43u
//! Largest decoded token, NUL included. Equals XRT_COMPONENT_ACTIVATION_TOKEN_MAX (asserted in the .c).
#define U_ACTIVATION_TOKEN_MAX 2048u
//! Bytes of a component id, NUL included (the registration id cap of every class).
#define U_ACTIVATION_ID_MAX 64u
//! Bytes of a class verb, NUL included.
#define U_ACTIVATION_CLASS_MAX 12u

//! Why a link was refused. Stable names: @ref u_activation_url_error_str.
enum u_activation_url_error
{
	U_ACTIVATION_URL_OK = 0,
	U_ACTIVATION_URL_EMPTY,         //!< NULL or ""
	U_ACTIVATION_URL_TOO_LONG,      //!< over @ref U_ACTIVATION_URL_MAX
	U_ACTIVATION_URL_BAD_CHARACTER, //!< a byte outside the link's closed character set
	U_ACTIVATION_URL_BAD_SCHEME,    //!< not `displayxr://`
	U_ACTIVATION_URL_BAD_ACTION,    //!< the action is not `activate`
	U_ACTIVATION_URL_BAD_QUERY,     //!< no query, an empty / unknown / repeated / malformed parameter
	U_ACTIVATION_URL_BAD_NONCE,     //!< missing, or not 43 base64url characters
	U_ACTIVATION_URL_BAD_TOKEN, //!< missing, empty, bad percent-encoding, too long, not UTF-8, control character
	U_ACTIVATION_URL_BAD_ITEM,  //!< `item` is present and not `<class>/<id>`
};

//! What a well-formed link carries. The token is a secret: wipe with @ref u_activation_url_wipe.
struct u_activation_url
{
	char nonce[U_ACTIVATION_NONCE_CHARS + 1];
	bool has_item;                           //!< the optional `item` echo was present
	char item_class[U_ACTIVATION_CLASS_MAX]; //!< "dp" / "input" / "conv" / "tracking" when @ref has_item
	char item_id[U_ACTIVATION_ID_MAX];       //!< the component id when @ref has_item
	uint32_t token_size;                     //!< bytes, NUL excluded
	char token[U_ACTIVATION_TOKEN_MAX];      //!< decoded, NUL-terminated
};

/*!
 * Parse @p url (NUL-terminated; attacker-controlled). On anything but
 * @ref U_ACTIVATION_URL_OK, @p out is zeroed: a refused link yields nothing.
 */
enum u_activation_url_error
u_activation_url_parse(const char *url, struct u_activation_url *out);

//! Zero @p u, the token with it, in a way the compiler does not drop.
void
u_activation_url_wipe(struct u_activation_url *u);

//! "ok" / "empty" / "too-long" / "bad-character" / "bad-scheme" / "bad-action" / "bad-query" / "bad-nonce" /
//! "bad-token" / "bad-item".
const char *
u_activation_url_error_str(enum u_activation_url_error e);

//! Is @p s one of the four class verbs ("dp", "input", "conv", "tracking")?
bool
u_activation_class_valid(const char *s);

//! Is @p s a component id this module accepts (1..63 of ALPHA / DIGIT / "." / "_" / "-")?
bool
u_activation_id_valid(const char *s);

//! Is @p s a nonce (exactly 43 base64url characters)?
bool
u_activation_nonce_valid(const char *s);

/*!
 * Is @p s well-formed UTF-8 of @p n bytes with no control character
 * (U+0000-U+001F, U+007F-U+009F), no surrogate, no overlong form and nothing
 * above U+10FFFF? The rule a token must pass before it reaches a component.
 */
bool
u_activation_text_valid(const char *s, size_t n);

//! Why a purchase URL could not be built.
enum u_activation_purchase_error
{
	U_ACTIVATION_PURCHASE_OK = 0,
	U_ACTIVATION_PURCHASE_NOT_HTTPS,     //!< empty, or not `https://<host>...`
	U_ACTIVATION_PURCHASE_BAD_CHARACTER, //!< a space, quote, control or non-ASCII byte in the vendor's URL
	U_ACTIVATION_PURCHASE_BAD_ARGUMENT,  //!< the class / id / nonce is not well-formed
	U_ACTIVATION_PURCHASE_TOO_LONG,      //!< does not fit @p cap
};

/*!
 * Build the URL the Dashboard opens to start an activation: the component's
 * own `purchase_url` with three parameters appended to its query (before any
 * fragment), each percent-encoded:
 *
 *     displayxr_nonce=<nonce>
 *     displayxr_item=<class>%2F<id>
 *     displayxr_return=displayxr%3A%2F%2Factivate
 *
 * @p purchase_url comes from a third-party binary, so it is checked, not
 * trusted: `https://` only, printable ASCII, no space, quote, backslash,
 * angle bracket or backtick. Pure.
 */
enum u_activation_purchase_error
u_activation_purchase_url_build(
    const char *purchase_url, const char *cls, const char *id, const char *nonce, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
