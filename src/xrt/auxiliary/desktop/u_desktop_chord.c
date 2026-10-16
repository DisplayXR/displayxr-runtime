// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Key-chord parser for XR_DXR_desktop_control (spec §5, ADR-056).
 */

#include "u_desktop_chord.h"

#include <stdio.h>
#include <string.h>

static const char *const k_named_keys[] = {
    "enter",     "tab",       "escape",       "backspace",    "delete", "insert",   "home",
    "end",       "pageup",    "pagedown",     "left",         "right",  "up",       "down",
    "space",     "minus",     "equal",        "comma",        "period", "slash",    "backslash",
    "semicolon", "quote",     "bracketleft",  "bracketright", "grave",  "capslock", "printscreen",
    "menu",
};

static bool
is_canonical_key(const char *k)
{
	size_t n = strlen(k);
	if (n == 1 && ((k[0] >= 'a' && k[0] <= 'z') || (k[0] >= '0' && k[0] <= '9'))) {
		return true;
	}
	// f1..f24
	if (k[0] == 'f' && (n == 2 || n == 3)) {
		int v = 0;
		for (size_t i = 1; i < n; i++) {
			if (k[i] < '0' || k[i] > '9') {
				return false;
			}
			v = v * 10 + (k[i] - '0');
		}
		if (n == 3 && k[1] == '0') {
			return false; // "f01"
		}
		return v >= 1 && v <= 24;
	}
	for (size_t i = 0; i < sizeof(k_named_keys) / sizeof(k_named_keys[0]); i++) {
		if (strcmp(k, k_named_keys[i]) == 0) {
			return true;
		}
	}
	return false;
}

static uint32_t
modifier_bit(const char *t)
{
	if (strcmp(t, "ctrl") == 0 || strcmp(t, "control") == 0) {
		return U_DESKTOP_MOD_CTRL;
	}
	if (strcmp(t, "shift") == 0) {
		return U_DESKTOP_MOD_SHIFT;
	}
	if (strcmp(t, "alt") == 0 || strcmp(t, "option") == 0) {
		return U_DESKTOP_MOD_ALT;
	}
	if (strcmp(t, "super") == 0 || strcmp(t, "win") == 0 || strcmp(t, "cmd") == 0 ||
	    strcmp(t, "command") == 0 || strcmp(t, "meta") == 0) {
		return U_DESKTOP_MOD_SUPER;
	}
	return 0;
}

bool
u_desktop_chord_parse(const char *s, struct u_desktop_chord *out)
{
	struct u_desktop_chord c;
	memset(&c, 0, sizeof(c));
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	if (s == NULL || out == NULL) {
		return false;
	}

	bool any_token = false;
	const char *p = s;
	for (;;) {
		// One token: [p, end).
		const char *end = strchr(p, '+');
		size_t len = end != NULL ? (size_t)(end - p) : strlen(p);

		// Trim spaces.
		while (len > 0 && (*p == ' ' || *p == '\t')) {
			p++;
			len--;
		}
		while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
			len--;
		}
		char tok[32];
		if (len == 0 || len >= sizeof(tok)) {
			return false; // empty token ("ctrl++a", trailing '+') or absurdly long
		}
		for (size_t i = 0; i < len; i++) {
			char ch = p[i];
			if (ch >= 'A' && ch <= 'Z') {
				ch = (char)(ch - 'A' + 'a');
			}
			tok[i] = ch;
		}
		tok[len] = '\0';

		uint32_t bit = modifier_bit(tok);
		if (bit != 0) {
			c.mods |= bit;
		} else {
			if (c.key[0] != '\0' || !is_canonical_key(tok)) {
				return false; // a second key, or not a known key
			}
			snprintf(c.key, sizeof(c.key), "%s", tok);
		}
		any_token = true;

		if (end == NULL) {
			break;
		}
		p = end + 1;
	}

	if (!any_token) {
		return false;
	}
	*out = c;
	return true;
}

bool
u_desktop_chord_format(const struct u_desktop_chord *chord, char *out, size_t cap)
{
	if (chord == NULL || out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	size_t n = 0;
	static const struct
	{
		uint32_t bit;
		const char *name;
	} mods[] = {
	    {U_DESKTOP_MOD_CTRL, "ctrl"},
	    {U_DESKTOP_MOD_SHIFT, "shift"},
	    {U_DESKTOP_MOD_ALT, "alt"},
	    {U_DESKTOP_MOD_SUPER, "super"},
	};
	for (size_t i = 0; i < 4; i++) {
		if ((chord->mods & mods[i].bit) == 0) {
			continue;
		}
		int w = snprintf(out + n, cap - n, "%s%s", n > 0 ? "+" : "", mods[i].name);
		if (w < 0 || (size_t)w >= cap - n) {
			return false;
		}
		n += (size_t)w;
	}
	if (chord->key[0] != '\0') {
		int w = snprintf(out + n, cap - n, "%s%s", n > 0 ? "+" : "", chord->key);
		if (w < 0 || (size_t)w >= cap - n) {
			return false;
		}
	}
	return true;
}
