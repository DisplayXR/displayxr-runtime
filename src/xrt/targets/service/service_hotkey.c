// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Workspace-controller launch hotkey parser / formatter.
 * @ingroup ipc
 */

#include "service_hotkey.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>


/*
 *
 * Tables
 *
 */

struct named_code
{
	const char *name;
	uint32_t code;
};

static const struct named_code s_mods[] = {
    {"Ctrl", SERVICE_HOTKEY_MOD_CTRL},
    {"Shift", SERVICE_HOTKEY_MOD_SHIFT},
    {"Alt", SERVICE_HOTKEY_MOD_ALT},
    {"Win", SERVICE_HOTKEY_MOD_WIN},
};

// Named keys, canonical spelling first for each code (the formatter takes the
// first match). Codes are Windows virtual-key numbers.
static const struct named_code s_keys[] = {
    {"Space", 0x20},
    {"Tab", 0x09},
    {"Enter", 0x0D},
    {"Backquote", 0xC0},
    {"Minus", 0xBD},
    {"Equals", 0xBB},
    {"BracketLeft", 0xDB},
    {"BracketRight", 0xDD},
    {"Semicolon", 0xBA},
    {"Quote", 0xDE},
    {"Comma", 0xBC},
    {"Period", 0xBE},
    {"Slash", 0xBF},
    {"Backslash", 0xDC},
    {"Insert", 0x2D},
    {"Delete", 0x2E},
    {"Home", 0x24},
    {"End", 0x23},
    {"PageUp", 0x21},
    {"PageDown", 0x22},
    {"Left", 0x25},
    {"Up", 0x26},
    {"Right", 0x27},
    {"Down", 0x28},
    // Parse-only aliases (never written).
    {"ArrowLeft", 0x25},
    {"ArrowUp", 0x26},
    {"ArrowRight", 0x27},
    {"ArrowDown", 0x28},
};

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))


/*
 *
 * Helpers
 *
 */

static bool
token_ieq(const char *tok, size_t len, const char *name)
{
	size_t n = strlen(name);
	if (n != len) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (tolower((unsigned char)tok[i]) != tolower((unsigned char)name[i])) {
			return false;
		}
	}
	return true;
}

//! Key token → VK, or 0 when not in the grammar.
static uint32_t
key_from_token(const char *tok, size_t len)
{
	if (len == 1) {
		char c = (char)toupper((unsigned char)tok[0]);
		if (c >= 'A' && c <= 'Z') {
			return (uint32_t)c; // VK_A..VK_Z == 'A'..'Z'
		}
		if (c >= '0' && c <= '9') {
			return (uint32_t)c; // VK_0..VK_9 == '0'..'9'
		}
		return 0;
	}
	if ((tok[0] == 'F' || tok[0] == 'f') && (len == 2 || len == 3)) {
		int n = 0;
		for (size_t i = 1; i < len; i++) {
			if (!isdigit((unsigned char)tok[i])) {
				return 0;
			}
			n = n * 10 + (tok[i] - '0');
		}
		if (tok[1] == '0' || n < 1 || n > 24) {
			return 0; // no "F0", "F01", "F25"
		}
		return 0x70u + (uint32_t)(n - 1); // VK_F1..VK_F24
	}
	for (size_t i = 0; i < ARRAY_COUNT(s_keys); i++) {
		if (token_ieq(tok, len, s_keys[i].name)) {
			return s_keys[i].code;
		}
	}
	return 0;
}

static uint32_t
mod_from_token(const char *tok, size_t len)
{
	for (size_t i = 0; i < ARRAY_COUNT(s_mods); i++) {
		if (token_ieq(tok, len, s_mods[i].name)) {
			return s_mods[i].code;
		}
	}
	return 0;
}


/*
 *
 * Public API
 *
 */

bool
service_hotkey_parse(const char *text, struct service_hotkey *out)
{
	struct service_hotkey hk = {0, 0};
	if (out != NULL) {
		*out = hk;
	}
	if (text == NULL || out == NULL || text[0] == '\0') {
		return false;
	}

	const char *p = text;
	for (;;) {
		const char *plus = strchr(p, '+');
		size_t len = plus ? (size_t)(plus - p) : strlen(p);
		if (len == 0) {
			return false; // "", "Ctrl++A", trailing '+'
		}
		for (size_t i = 0; i < len; i++) {
			if (isspace((unsigned char)p[i])) {
				return false;
			}
		}

		if (plus != NULL) {
			// Not the last token: must be a modifier, at most once.
			uint32_t m = mod_from_token(p, len);
			if (m == 0 || (hk.mods & m) != 0) {
				return false;
			}
			hk.mods |= m;
			p = plus + 1;
			continue;
		}

		// Last token: the key.
		uint32_t vk = key_from_token(p, len);
		if (vk == 0) {
			return false; // unknown key, or a bare modifier as the key
		}
		hk.vk = vk;
		break;
	}

	if (hk.mods == 0) {
		return false; // at least one modifier
	}
	*out = hk;
	return true;
}

bool
service_hotkey_format(const struct service_hotkey *hk, char *buf, size_t buf_size)
{
	if (hk == NULL || buf == NULL || buf_size == 0) {
		return false;
	}
	buf[0] = '\0';
	if (hk->mods == 0 || (hk->mods & ~0xFu) != 0) {
		return false;
	}

	char key[16] = "";
	if ((hk->vk >= 'A' && hk->vk <= 'Z') || (hk->vk >= '0' && hk->vk <= '9')) {
		key[0] = (char)hk->vk;
		key[1] = '\0';
	} else if (hk->vk >= 0x70 && hk->vk <= 0x87) {
		snprintf(key, sizeof(key), "F%u", (unsigned)(hk->vk - 0x70 + 1));
	} else {
		for (size_t i = 0; i < ARRAY_COUNT(s_keys); i++) {
			if (s_keys[i].code == hk->vk) {
				snprintf(key, sizeof(key), "%s", s_keys[i].name);
				break;
			}
		}
	}
	if (key[0] == '\0') {
		return false;
	}

	int n = snprintf(buf, buf_size, "%s%s%s%s%s", (hk->mods & SERVICE_HOTKEY_MOD_CTRL) ? "Ctrl+" : "",
	                 (hk->mods & SERVICE_HOTKEY_MOD_SHIFT) ? "Shift+" : "",
	                 (hk->mods & SERVICE_HOTKEY_MOD_ALT) ? "Alt+" : "",
	                 (hk->mods & SERVICE_HOTKEY_MOD_WIN) ? "Win+" : "", key);
	if (n < 0 || (size_t)n >= buf_size) {
		buf[0] = '\0';
		return false;
	}
	return true;
}

bool
service_hotkey_canonicalize(const char *text, char *out, size_t out_size)
{
	struct service_hotkey hk;
	return service_hotkey_parse(text, &hk) && service_hotkey_format(&hk, out, out_size);
}


/*
 *
 * Linux mappings: X11 keysyms and GTK accelerator names.
 *
 */

struct key_names
{
	uint32_t vk;
	uint32_t keysym;
	const char *gtk;
};

// Every non-alphanumeric, non-F key of the grammar. keysymdef.h values.
static const struct key_names s_linux_keys[] = {
    {0x20, 0x0020, "space"},        {0x09, 0xff09, "Tab"},          {0x0D, 0xff0d, "Return"},
    {0xC0, 0x0060, "grave"},        {0xBD, 0x002d, "minus"},        {0xBB, 0x003d, "equal"},
    {0xDB, 0x005b, "bracketleft"},  {0xDD, 0x005d, "bracketright"}, {0xBA, 0x003b, "semicolon"},
    {0xDE, 0x0027, "apostrophe"},   {0xBC, 0x002c, "comma"},        {0xBE, 0x002e, "period"},
    {0xBF, 0x002f, "slash"},        {0xDC, 0x005c, "backslash"},    {0x2D, 0xff63, "Insert"},
    {0x2E, 0xffff, "Delete"},       {0x24, 0xff50, "Home"},         {0x23, 0xff57, "End"},
    {0x21, 0xff55, "Page_Up"},      {0x22, 0xff56, "Page_Down"},    {0x25, 0xff51, "Left"},
    {0x26, 0xff52, "Up"},           {0x27, 0xff53, "Right"},        {0x28, 0xff54, "Down"},
};

//! VK → keysym + GTK name. @p gtk_buf receives the name (>= 16 bytes).
static bool
linux_key(uint32_t vk, uint32_t *keysym, char *gtk_buf, size_t gtk_size)
{
	if (vk >= 'A' && vk <= 'Z') {
		*keysym = vk - 'A' + 0x61; // XK_a..XK_z
		snprintf(gtk_buf, gtk_size, "%c", (char)(vk - 'A' + 'a'));
		return true;
	}
	if (vk >= '0' && vk <= '9') {
		*keysym = vk; // XK_0..XK_9 == '0'..'9'
		snprintf(gtk_buf, gtk_size, "%c", (char)vk);
		return true;
	}
	if (vk >= 0x70 && vk <= 0x87) {
		*keysym = 0xffbe + (vk - 0x70); // XK_F1..XK_F24 are contiguous
		snprintf(gtk_buf, gtk_size, "F%u", (unsigned)(vk - 0x70 + 1));
		return true;
	}
	for (size_t i = 0; i < ARRAY_COUNT(s_linux_keys); i++) {
		if (s_linux_keys[i].vk == vk) {
			*keysym = s_linux_keys[i].keysym;
			snprintf(gtk_buf, gtk_size, "%s", s_linux_keys[i].gtk);
			return true;
		}
	}
	return false;
}

static bool
hotkey_valid(const struct service_hotkey *hk)
{
	char tmp[SERVICE_HOTKEY_MAX];
	return hk != NULL && service_hotkey_format(hk, tmp, sizeof(tmp));
}

bool
service_hotkey_to_x11(const struct service_hotkey *hk, uint32_t *out_keysym, uint32_t *out_mods)
{
	if (out_keysym == NULL || out_mods == NULL) {
		return false;
	}
	*out_keysym = 0;
	*out_mods = 0;
	char name[16];
	uint32_t keysym = 0;
	if (!hotkey_valid(hk) || !linux_key(hk->vk, &keysym, name, sizeof(name))) {
		return false;
	}
	uint32_t mods = 0;
	if (hk->mods & SERVICE_HOTKEY_MOD_CTRL) {
		mods |= SERVICE_HOTKEY_X11_CONTROL;
	}
	if (hk->mods & SERVICE_HOTKEY_MOD_SHIFT) {
		mods |= SERVICE_HOTKEY_X11_SHIFT;
	}
	if (hk->mods & SERVICE_HOTKEY_MOD_ALT) {
		mods |= SERVICE_HOTKEY_X11_MOD1;
	}
	if (hk->mods & SERVICE_HOTKEY_MOD_WIN) {
		mods |= SERVICE_HOTKEY_X11_MOD4;
	}
	*out_keysym = keysym;
	*out_mods = mods;
	return true;
}

bool
service_hotkey_to_accelerator(const struct service_hotkey *hk, char *buf, size_t buf_size)
{
	if (buf == NULL || buf_size == 0) {
		return false;
	}
	buf[0] = '\0';
	char name[16];
	uint32_t keysym = 0;
	if (!hotkey_valid(hk) || !linux_key(hk->vk, &keysym, name, sizeof(name))) {
		return false;
	}
	int n = snprintf(buf, buf_size, "%s%s%s%s%s", (hk->mods & SERVICE_HOTKEY_MOD_CTRL) ? "<Control>" : "",
	                 (hk->mods & SERVICE_HOTKEY_MOD_SHIFT) ? "<Shift>" : "",
	                 (hk->mods & SERVICE_HOTKEY_MOD_ALT) ? "<Alt>" : "",
	                 (hk->mods & SERVICE_HOTKEY_MOD_WIN) ? "<Super>" : "", name);
	if (n < 0 || (size_t)n >= buf_size) {
		buf[0] = '\0';
		return false;
	}
	return true;
}
