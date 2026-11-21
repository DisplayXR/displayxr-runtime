// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Strict `displayxr://activate` parser + purchase-URL builder.
 * @ingroup aux_util
 */

#include "u_activation_url.h"

#include "xrt/xrt_component_license.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static_assert(U_ACTIVATION_TOKEN_MAX == XRT_COMPONENT_ACTIVATION_TOKEN_MAX,
              "the link's token cap is the activation slot's");

static bool
is_alpha(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool
is_digit(unsigned char c)
{
	return c >= '0' && c <= '9';
}

static bool
is_unreserved(unsigned char c)
{
	return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

static int
hex_val(unsigned char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static unsigned char
ascii_lower(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

void
u_activation_url_wipe(struct u_activation_url *u)
{
	if (u == NULL) {
		return;
	}
	volatile unsigned char *p = (volatile unsigned char *)u;
	for (size_t i = 0; i < sizeof(*u); i++) {
		p[i] = 0;
	}
}

const char *
u_activation_url_error_str(enum u_activation_url_error e)
{
	switch (e) {
	case U_ACTIVATION_URL_OK: return "ok";
	case U_ACTIVATION_URL_EMPTY: return "empty";
	case U_ACTIVATION_URL_TOO_LONG: return "too-long";
	case U_ACTIVATION_URL_BAD_CHARACTER: return "bad-character";
	case U_ACTIVATION_URL_BAD_SCHEME: return "bad-scheme";
	case U_ACTIVATION_URL_BAD_ACTION: return "bad-action";
	case U_ACTIVATION_URL_BAD_QUERY: return "bad-query";
	case U_ACTIVATION_URL_BAD_NONCE: return "bad-nonce";
	case U_ACTIVATION_URL_BAD_TOKEN: return "bad-token";
	case U_ACTIVATION_URL_BAD_ITEM: return "bad-item";
	default: return "bad-query";
	}
}

bool
u_activation_class_valid(const char *s)
{
	return s != NULL && (strcmp(s, "dp") == 0 || strcmp(s, "input") == 0 || strcmp(s, "conv") == 0 ||
	                     strcmp(s, "tracking") == 0);
}

static bool
id_valid_n(const char *s, size_t n)
{
	if (n == 0 || n >= U_ACTIVATION_ID_MAX) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		const unsigned char c = (unsigned char)s[i];
		if (!(is_alpha(c) || is_digit(c) || c == '.' || c == '_' || c == '-')) {
			return false;
		}
	}
	// "." and ".." are never ids (the id is also a file / registry key name).
	return !(s[0] == '.' && (n == 1 || (n == 2 && s[1] == '.')));
}

bool
u_activation_id_valid(const char *s)
{
	return s != NULL && id_valid_n(s, strlen(s));
}

static bool
nonce_valid_n(const char *s, size_t n)
{
	if (n != U_ACTIVATION_NONCE_CHARS) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		const unsigned char c = (unsigned char)s[i];
		if (!(is_alpha(c) || is_digit(c) || c == '-' || c == '_')) {
			return false;
		}
	}
	return true;
}

bool
u_activation_nonce_valid(const char *s)
{
	return s != NULL && nonce_valid_n(s, strlen(s));
}

bool
u_activation_text_valid(const char *s, size_t n)
{
	if (s == NULL) {
		return false;
	}
	size_t i = 0;
	while (i < n) {
		const unsigned char c = (unsigned char)s[i];
		uint32_t cp = 0;
		size_t len = 0;
		if (c < 0x80) {
			cp = c;
			len = 1;
		} else if (c >= 0xC2 && c <= 0xDF) {
			cp = c & 0x1Fu;
			len = 2;
		} else if (c >= 0xE0 && c <= 0xEF) {
			cp = c & 0x0Fu;
			len = 3;
		} else if (c >= 0xF0 && c <= 0xF4) {
			cp = c & 0x07u;
			len = 4;
		} else {
			return false; // a continuation byte first, 0xC0 / 0xC1 (overlong), or 0xF5..0xFF
		}
		if (i + len > n) {
			return false; // truncated sequence
		}
		for (size_t k = 1; k < len; k++) {
			const unsigned char cc = (unsigned char)s[i + k];
			if ((cc & 0xC0u) != 0x80u) {
				return false;
			}
			cp = (cp << 6) | (cc & 0x3Fu);
		}
		if ((len == 3 && cp < 0x800u) || (len == 4 && cp < 0x10000u)) {
			return false; // overlong
		}
		if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
			return false; // out of range / surrogate
		}
		if (cp < 0x20u || (cp >= 0x7Fu && cp <= 0x9Fu)) {
			return false; // C0, DEL, C1
		}
		i += len;
	}
	return true;
}

//! Percent-decode @p n bytes of `unreserved / pct-encoded` into @p out (cap bytes, NUL included). -1 = refused.
static int
decode_token(const char *s, size_t n, char *out, size_t cap)
{
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		const unsigned char c = (unsigned char)s[i];
		unsigned char b;
		if (c == '%') {
			if (i + 2 >= n) {
				return -1; // a "%" without two hex digits after it
			}
			const int hi = hex_val((unsigned char)s[i + 1]);
			const int lo = hex_val((unsigned char)s[i + 2]);
			if (hi < 0 || lo < 0) {
				return -1;
			}
			b = (unsigned char)((hi << 4) | lo);
			i += 2;
		} else if (is_unreserved(c)) {
			b = c;
		} else {
			return -1;
		}
		if (o + 1 >= cap) {
			return -1; // too long
		}
		out[o++] = (char)b;
	}
	out[o] = '\0';
	return (int)o;
}

//! `<class>/<id>` or `<class>%2F<id>` in @p n bytes.
static bool
parse_item(const char *s, size_t n, struct u_activation_url *out)
{
	size_t sep = 0;
	size_t sep_len = 0;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '/') {
			sep = i;
			sep_len = 1;
			break;
		}
		if (s[i] == '%') {
			if (i + 2 < n && s[i + 1] == '2' && ascii_lower((unsigned char)s[i + 2]) == 'f') {
				sep = i;
				sep_len = 3;
				break;
			}
			return false; // no other escape belongs in an item
		}
	}
	if (sep_len == 0 || sep == 0 || sep >= U_ACTIVATION_CLASS_MAX) {
		return false;
	}
	char cls[U_ACTIVATION_CLASS_MAX];
	memcpy(cls, s, sep);
	cls[sep] = '\0';
	const char *id = s + sep + sep_len;
	const size_t id_n = n - sep - sep_len;
	if (!u_activation_class_valid(cls) || !id_valid_n(id, id_n)) {
		return false;
	}
	memcpy(out->item_class, cls, sep + 1);
	memcpy(out->item_id, id, id_n);
	out->item_id[id_n] = '\0';
	out->has_item = true;
	return true;
}

static enum u_activation_url_error
parse(const char *url, struct u_activation_url *out)
{
	if (url == NULL || url[0] == '\0') {
		return U_ACTIVATION_URL_EMPTY;
	}
	// Bounded length scan: never walk an unterminated or enormous argument.
	size_t n = 0;
	while (n <= U_ACTIVATION_URL_MAX && url[n] != '\0') {
		n++;
	}
	if (n > U_ACTIVATION_URL_MAX) {
		return U_ACTIVATION_URL_TOO_LONG;
	}
	// The closed character set of the whole link: RFC 3986 unreserved, "%",
	// and the five delimiters the grammar uses. Everything else - space,
	// quotes, backslash, "#", "+", "@", ";", control bytes, non-ASCII - ends it.
	for (size_t i = 0; i < n; i++) {
		const unsigned char c = (unsigned char)url[i];
		if (!(is_unreserved(c) || c == '%' || c == ':' || c == '/' || c == '?' || c == '&' || c == '=')) {
			return U_ACTIVATION_URL_BAD_CHARACTER;
		}
	}

	static const char scheme[] = "displayxr://";
	const size_t scheme_n = sizeof(scheme) - 1;
	if (n < scheme_n) {
		return U_ACTIVATION_URL_BAD_SCHEME;
	}
	for (size_t i = 0; i < scheme_n; i++) {
		if (ascii_lower((unsigned char)url[i]) != (unsigned char)scheme[i]) {
			return U_ACTIVATION_URL_BAD_SCHEME;
		}
	}
	const char *p = url + scheme_n;
	const char *end = url + n;

	static const char action[] = "activate";
	const size_t action_n = sizeof(action) - 1;
	if ((size_t)(end - p) < action_n || memcmp(p, action, action_n) != 0) {
		return U_ACTIVATION_URL_BAD_ACTION;
	}
	p += action_n;
	if (p < end && *p == '/') {
		p++; // one optional slash: some launchers normalise "host?query" to "host/?query"
	}
	if (p == end) {
		return U_ACTIVATION_URL_BAD_QUERY; // the action alone
	}
	if (*p != '?') {
		return U_ACTIVATION_URL_BAD_ACTION; // "activateX", "activate/path", "activate:80"
	}
	p++;

	bool have_nonce = false;
	bool have_token = false;
	bool have_item = false;
	while (p <= end) {
		const char *amp = memchr(p, '&', (size_t)(end - p));
		const char *pe = amp != NULL ? amp : end;
		const char *eq = memchr(p, '=', (size_t)(pe - p));
		if (eq == NULL || eq == p) {
			return U_ACTIVATION_URL_BAD_QUERY; // "", "key", "=value"
		}
		const size_t kn = (size_t)(eq - p);
		const char *v = eq + 1;
		const size_t vn = (size_t)(pe - v);
		if (kn == 5 && memcmp(p, "nonce", 5) == 0) {
			if (have_nonce) {
				return U_ACTIVATION_URL_BAD_QUERY;
			}
			if (!nonce_valid_n(v, vn)) {
				return U_ACTIVATION_URL_BAD_NONCE;
			}
			memcpy(out->nonce, v, vn);
			out->nonce[vn] = '\0';
			have_nonce = true;
		} else if (kn == 5 && memcmp(p, "token", 5) == 0) {
			if (have_token) {
				return U_ACTIVATION_URL_BAD_QUERY;
			}
			const int tn = decode_token(v, vn, out->token, sizeof(out->token));
			if (tn <= 0 || !u_activation_text_valid(out->token, (size_t)tn)) {
				return U_ACTIVATION_URL_BAD_TOKEN;
			}
			out->token_size = (uint32_t)tn;
			have_token = true;
		} else if (kn == 4 && memcmp(p, "item", 4) == 0) {
			if (have_item) {
				return U_ACTIVATION_URL_BAD_QUERY;
			}
			if (!parse_item(v, vn, out)) {
				return U_ACTIVATION_URL_BAD_ITEM;
			}
			have_item = true;
		} else {
			return U_ACTIVATION_URL_BAD_QUERY; // an unknown key is refused, not ignored
		}
		if (amp == NULL) {
			break;
		}
		p = amp + 1;
	}
	if (!have_nonce) {
		return U_ACTIVATION_URL_BAD_NONCE;
	}
	if (!have_token) {
		return U_ACTIVATION_URL_BAD_TOKEN;
	}
	return U_ACTIVATION_URL_OK;
}

enum u_activation_url_error
u_activation_url_parse(const char *url, struct u_activation_url *out)
{
	if (out == NULL) {
		return U_ACTIVATION_URL_EMPTY;
	}
	u_activation_url_wipe(out);
	const enum u_activation_url_error e = parse(url, out);
	if (e != U_ACTIVATION_URL_OK) {
		u_activation_url_wipe(out);
	}
	return e;
}

enum u_activation_purchase_error
u_activation_purchase_url_build(
    const char *purchase_url, const char *cls, const char *id, const char *nonce, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return U_ACTIVATION_PURCHASE_TOO_LONG;
	}
	out[0] = '\0';
	if (!u_activation_class_valid(cls) || !u_activation_id_valid(id) || !u_activation_nonce_valid(nonce)) {
		return U_ACTIVATION_PURCHASE_BAD_ARGUMENT;
	}
	static const char https[] = "https://";
	if (purchase_url == NULL || strncmp(purchase_url, https, sizeof(https) - 1) != 0 ||
	    purchase_url[sizeof(https) - 1] == '\0' || purchase_url[sizeof(https) - 1] == '/' ||
	    purchase_url[sizeof(https) - 1] == '?' || purchase_url[sizeof(https) - 1] == '#') {
		return U_ACTIVATION_PURCHASE_NOT_HTTPS;
	}
	const size_t n = strlen(purchase_url);
	const char *frag = NULL;
	bool has_query = false;
	for (size_t i = 0; i < n; i++) {
		const unsigned char c = (unsigned char)purchase_url[i];
		if (c <= 0x20 || c >= 0x7F || c == '"' || c == '\'' || c == '\\' || c == '<' || c == '>' || c == '`' ||
		    c == '^' || c == '{' || c == '}' || c == '|') {
			return U_ACTIVATION_PURCHASE_BAD_CHARACTER;
		}
		if (c == '#' && frag == NULL) {
			frag = purchase_url + i;
		}
		if (c == '?' && frag == NULL) {
			has_query = true;
		}
	}
	const size_t head = frag != NULL ? (size_t)(frag - purchase_url) : n;
	// A URL that already ends in "?" or "&" takes the parameters with no separator.
	const char last = purchase_url[head - 1];
	const char *sep = (last == '?' || last == '&') ? "" : (has_query ? "&" : "?");
	const int w = snprintf(
	    out, cap,
	    "%.*s%sdisplayxr_nonce=%s&displayxr_item=%s%%2F%s&displayxr_return=displayxr%%3A%%2F%%2Factivate%s",
	    (int)head, purchase_url, sep, nonce, cls, id, frag != NULL ? frag : "");
	if (w < 0 || (size_t)w >= cap) {
		out[0] = '\0';
		return U_ACTIVATION_PURCHASE_TOO_LONG;
	}
	return U_ACTIVATION_PURCHASE_OK;
}
