// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: input through the portal session — libei
 *         pointer, buttons, scroll, layout-aware chords, and IME-safe text via the
 *         portal clipboard (ADR-056). Runs on the portal thread
 *         (u_desktop_gnome_portal.c); see that file's header for the design.
 */

#include "u_desktop_gnome_portal_priv.h"

#include "util/u_logging.h"

#include <linux/input-event-codes.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CLIP_MAX_BYTES (1u << 20)
//! Gap between consecutive key / button events, so every client sees each one.
#define KEY_GAP_MS 8

static bool
is_text_mime(const char *mime)
{
	return mime != NULL &&
	       (strcmp(mime, "text/plain;charset=utf-8") == 0 || strcmp(mime, "text/plain") == 0 ||
	        strcmp(mime, "UTF8_STRING") == 0 || strcmp(mime, "STRING") == 0 || strcmp(mime, "TEXT") == 0);
}

//! Write all of @p data to @p fd within @p timeout_ms.
static bool
write_all(int fd, const char *data, size_t len, uint32_t timeout_ms)
{
	uint64_t deadline = gdbus_now_ms() + timeout_ms;
	size_t off = 0;
	int fl = fcntl(fd, F_GETFL);
	if (fl >= 0) {
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	}
	while (off < len) {
		ssize_t w = write(fd, data + off, len - off);
		if (w > 0) {
			off += (size_t)w;
			continue;
		}
		if (w < 0 && errno != EAGAIN && errno != EINTR) {
			return false;
		}
		uint64_t now = gdbus_now_ms();
		if (now >= deadline) {
			return false;
		}
		struct pollfd pfd = {fd, POLLOUT, 0};
		poll(&pfd, 1, (int)(deadline - now));
	}
	return true;
}

void
gp_serve_transfer(struct gnome_portal *p, const char *mime, uint32_t serial)
{
	bool ok = false;
	if (p->clip_text != NULL && is_text_mime(mime)) {
		// SelectionWrite(o session, u serial) -> (h fd)
		DBusMessage *m = dbus_message_new_method_call(PORTAL_DEST, PORTAL_PATH, CLIP_IFACE, "SelectionWrite");
		const char *s = p->session;
		dbus_uint32_t ser = serial;
		dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &s, DBUS_TYPE_UINT32, &ser, DBUS_TYPE_INVALID);
		DBusMessage *reply = dbus_connection_send_with_reply_and_block(p->c, m, CALL_TIMEOUT_MS, NULL);
		dbus_message_unref(m);
		int fd = -1;
		if (reply != NULL && dbus_message_get_args(reply, NULL, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID) &&
		    fd >= 0) {
			ok = write_all(fd, p->clip_text, strlen(p->clip_text), 1000);
			close(fd);
		}
		if (reply != NULL) {
			dbus_message_unref(reply);
		}
	}
	DBusMessage *m = dbus_message_new_method_call(PORTAL_DEST, PORTAL_PATH, CLIP_IFACE, "SelectionWriteDone");
	const char *s = p->session;
	dbus_uint32_t ser = serial;
	dbus_bool_t success = ok;
	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &s, DBUS_TYPE_UINT32, &ser, DBUS_TYPE_BOOLEAN, &success,
	                         DBUS_TYPE_INVALID);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(p->c, m, CALL_TIMEOUT_MS, NULL);
	dbus_message_unref(m);
	if (reply != NULL) {
		dbus_message_unref(reply);
	}
	if (ok) {
		p->transfers++;
	}
}

void
gp_xkb_forget(struct gnome_portal *p)
{
	if (p->xkb_km != NULL) {
		g_xkb.keymap_unref(p->xkb_km);
	}
	if (p->xkb_ctx != NULL) {
		g_xkb.context_unref(p->xkb_ctx);
	}
	p->xkb_km = NULL;
	p->xkb_ctx = NULL;
	p->xkb_dev = NULL;
	p->xkb_tried = false;
}

static enum u_desktop_result
cmd_motion(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	struct ei_device *d = gp_find_dev(p, EIX_CAP_POINTER_ABSOLUTE, false);
	if (d == NULL) {
		gp_set_msg(c->msg, "the remote-desktop session has no absolute pointer");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	bool inside = false;
	struct ei_region *reg;
	for (size_t i = 0; !inside && (reg = g_ei.device_get_region(d, i)) != NULL; i++) {
		int64_t rx = g_ei.region_get_x(reg), ry = g_ei.region_get_y(reg);
		int64_t rw = g_ei.region_get_width(reg), rh = g_ei.region_get_height(reg);
		inside = c->x >= rx && c->y >= ry && c->x < rx + rw && c->y < ry + rh;
	}
	if (!inside) {
		gp_set_msg(c->msg, "(%d,%d) is on no monitor", c->x, c->y);
		return U_DESKTOP_ERR_REFUSED;
	}
	g_ei.motion_absolute(d, (double)c->x, (double)c->y);
	g_ei.device_frame(d, g_ei.now(p->ei));
	p->last_use_ms = gdbus_now_ms();
	return U_DESKTOP_OK;
}

static enum u_desktop_result
cmd_button(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	struct ei_device *d = gp_find_dev(p, EIX_CAP_BUTTON, true);
	if (d == NULL) {
		gp_set_msg(c->msg, "the remote-desktop session has no pointer buttons");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	g_ei.button_button(d, c->code, c->down);
	g_ei.device_frame(d, g_ei.now(p->ei));
	gdbus_sleep_ms(KEY_GAP_MS);
	p->last_use_ms = gdbus_now_ms();
	return U_DESKTOP_OK;
}

static enum u_desktop_result
cmd_scroll(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	struct ei_device *d = gp_find_dev(p, EIX_CAP_SCROLL, true);
	if (d == NULL) {
		gp_set_msg(c->msg, "the remote-desktop session has no scroll device");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	g_ei.scroll_discrete(d, c->x * 120, c->y * 120);
	g_ei.device_frame(d, g_ei.now(p->ei));
	p->last_use_ms = gdbus_now_ms();
	return U_DESKTOP_OK;
}

static enum u_desktop_result
send_keys(struct gnome_portal *p, const uint32_t *codes, const bool *press, uint32_t n, char *msg)
{
	struct ei_device *d = gp_find_dev(p, EIX_CAP_KEYBOARD, false);
	if (d == NULL) {
		gp_set_msg(msg, "the remote-desktop session has no keyboard");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	for (uint32_t i = 0; i < n; i++) {
		g_ei.keyboard_key(d, codes[i], press[i]);
		g_ei.device_frame(d, g_ei.now(p->ei));
		gdbus_sleep_ms(KEY_GAP_MS);
	}
	p->last_use_ms = gdbus_now_ms();
	return U_DESKTOP_OK;
}

//! Compile the keyboard device's keymap once per device. False: none available.
static bool
xkb_ensure(struct gnome_portal *p, struct ei_device *kbd)
{
	if (p->xkb_dev != kbd) {
		gp_xkb_forget(p);
		p->xkb_dev = kbd;
	}
	if (p->xkb_km != NULL || p->xkb_tried) {
		return p->xkb_km != NULL;
	}
	p->xkb_tried = true;
	if (!gnome_libs_xkb()) {
		return false;
	}
	struct ei_keymap *k = g_ei.device_keyboard_get_keymap(kbd);
	if (k == NULL || g_ei.keymap_get_type(k) != EIX_KEYMAP_TYPE_XKB) {
		U_LOG_W("desktop/gnome: the libei keyboard has no XKB keymap — chords use US key positions");
		return false;
	}
	int fd = g_ei.keymap_get_fd(k); // owned by libei: mapped, never closed here
	size_t size = g_ei.keymap_get_size(k);
	void *map = size > 0 ? mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	if (map == MAP_FAILED) {
		U_LOG_W("desktop/gnome: could not map the libei keymap (%s)", strerror(errno));
		return false;
	}
	p->xkb_ctx = g_xkb.context_new(0);
	if (p->xkb_ctx != NULL) {
		p->xkb_km =
		    g_xkb.keymap_new_from_buffer(p->xkb_ctx, map, strnlen(map, size), XKBX_KEYMAP_FORMAT_TEXT_V1, 0);
	}
	munmap(map, size);
	if (p->xkb_km == NULL) {
		U_LOG_W("desktop/gnome: xkbcommon could not compile the session keymap — chords use US positions");
		return false;
	}
	return true;
}

/*!
 * The evdev code of the key producing @p ch on the active layout, at level 1
 * (*shift = false) or the Shift level (*shift = true). False when no key does
 * without more modifiers (AltGr…).
 */
static bool
xkb_find_char(struct gnome_portal *p, uint32_t ch, uint32_t *code, bool *shift)
{
	uint32_t lo = g_xkb.keymap_min_keycode(p->xkb_km), hi = g_xkb.keymap_max_keycode(p->xkb_km);
	for (uint32_t level = 0; level < 2; level++) {
		for (uint32_t key = lo; key <= hi; key++) {
			uint32_t layouts = g_xkb.keymap_num_layouts_for_key(p->xkb_km, key);
			if (layouts == 0 || key < 8) {
				continue;
			}
			// A key with fewer groups wraps (XKB's default group wrap).
			uint32_t group = p->xkb_group % layouts;
			const uint32_t *syms = NULL;
			int n = g_xkb.keymap_key_get_syms_by_level(p->xkb_km, key, group, level, &syms);
			for (int i = 0; i < n; i++) {
				uint32_t u = g_xkb.keysym_to_utf32(syms[i]);
				// A letter key's level 1 is lower case; accept its upper case too.
				if (u == ch || (ch >= 'a' && ch <= 'z' && u == ch - 32 && level == 0)) {
					*code = key - 8;
					*shift = level == 1;
					return true;
				}
			}
		}
	}
	return false;
}

//! US-position fallback for a character when no keymap is available.
static uint32_t
us_code_for_char(uint32_t ch)
{
	static const uint32_t letters[26] = {
	    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
	    KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	};
	static const uint32_t digits[10] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
	if (ch >= 'a' && ch <= 'z') {
		return letters[ch - 'a'];
	}
	if (ch >= '0' && ch <= '9') {
		return digits[ch - '0'];
	}
	switch (ch) {
	case '-': return KEY_MINUS;
	case '=': return KEY_EQUAL;
	case ',': return KEY_COMMA;
	case '.': return KEY_DOT;
	case '/': return KEY_SLASH;
	case '\\': return KEY_BACKSLASH;
	case ';': return KEY_SEMICOLON;
	case '\'': return KEY_APOSTROPHE;
	case '[': return KEY_LEFTBRACE;
	case ']': return KEY_RIGHTBRACE;
	case '`': return KEY_GRAVE;
	default: return 0;
	}
}

/*!
 * Press @p mods (u_desktop_mod bits) + one key, release in reverse. The key is
 * @p ch (a character, looked up on the session keymap's active layout) or, when
 * @p ch is 0, the fixed evdev @p code (named keys; 0 = modifiers only).
 */
static enum u_desktop_result
send_chord(struct gnome_portal *p, uint32_t mods, uint32_t code, uint32_t ch, char *msg)
{
	struct ei_device *kbd = gp_find_dev(p, EIX_CAP_KEYBOARD, false);
	if (kbd == NULL) {
		gp_set_msg(msg, "the remote-desktop session has no keyboard");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	if (ch != 0) {
		bool shift = false;
		if (xkb_ensure(p, kbd)) {
			if (!xkb_find_char(p, ch, &code, &shift)) {
				if (ch < 0x80) {
					gp_set_msg(msg, "no key produces '%c' on the active keyboard layout", (char)ch);
				} else {
					gp_set_msg(msg, "no key produces U+%04X on the active keyboard layout", ch);
				}
				return U_DESKTOP_ERR_UNSUPPORTED;
			}
		} else {
			code = us_code_for_char(ch);
			if (code == 0) {
				gp_set_msg(msg, "no key for character U+%04X", ch);
				return U_DESKTOP_ERR_UNSUPPORTED;
			}
		}
		if (shift) {
			mods |= U_DESKTOP_MOD_SHIFT;
		}
	}
	static const struct
	{
		uint32_t bit, code;
	} modkeys[] = {
	    {U_DESKTOP_MOD_CTRL, KEY_LEFTCTRL},
	    {U_DESKTOP_MOD_SHIFT, KEY_LEFTSHIFT},
	    {U_DESKTOP_MOD_ALT, KEY_LEFTALT},
	    {U_DESKTOP_MOD_SUPER, KEY_LEFTMETA},
	};
	uint32_t codes[GNOME_PORTAL_MAX_KEYS];
	bool press[GNOME_PORTAL_MAX_KEYS];
	uint32_t n = 0;
	for (size_t i = 0; i < 4; i++) {
		if (mods & modkeys[i].bit) {
			codes[n] = modkeys[i].code, press[n++] = true;
		}
	}
	if (code != 0) {
		codes[n] = code, press[n++] = true;
		codes[n] = code, press[n++] = false;
	}
	for (size_t i = 4; i-- > 0;) {
		if (mods & modkeys[i].bit) {
			codes[n] = modkeys[i].code, press[n++] = false;
		}
	}
	return send_keys(p, codes, press, n, msg);
}

static enum u_desktop_result
cmd_chord(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	return send_chord(p, c->mods, c->code, c->ch, c->msg);
}

static enum u_desktop_result
cmd_keys(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	return send_keys(p, c->keys, c->press, c->nkeys, c->msg);
}

//! SelectionRead of the current clipboard text, best effort, ≤ 300 ms. NULL if none.
static char *
clipboard_read(struct gnome_portal *p)
{
	DBusMessage *m = dbus_message_new_method_call(PORTAL_DEST, PORTAL_PATH, CLIP_IFACE, "SelectionRead");
	const char *s = p->session;
	const char *mime = "text/plain;charset=utf-8";
	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &s, DBUS_TYPE_STRING, &mime, DBUS_TYPE_INVALID);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(p->c, m, 300, NULL);
	dbus_message_unref(m);
	int fd = -1;
	if (reply == NULL || !dbus_message_get_args(reply, NULL, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID) || fd < 0) {
		if (reply != NULL) {
			dbus_message_unref(reply);
		}
		return NULL;
	}
	dbus_message_unref(reply);
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	uint64_t deadline = gdbus_now_ms() + 300;
	bool eof = false;
	while (buf != NULL && !eof) {
		uint64_t now = gdbus_now_ms();
		if (now >= deadline) {
			break;
		}
		struct pollfd pfd = {fd, POLLIN, 0};
		if (poll(&pfd, 1, (int)(deadline - now)) <= 0) {
			break;
		}
		if (len + 1 >= cap) {
			if (cap >= CLIP_MAX_BYTES) {
				break;
			}
			char *nb = realloc(buf, cap * 2);
			if (nb == NULL) {
				break;
			}
			buf = nb;
			cap *= 2;
		}
		ssize_t r = read(fd, buf + len, cap - len - 1);
		if (r > 0) {
			len += (size_t)r;
		} else if (r == 0) {
			eof = true;
		} else if (errno != EINTR && errno != EAGAIN) {
			break;
		}
	}
	close(fd);
	if (buf == NULL || !eof) {
		free(buf); // partial reads are not restored
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

static bool
clipboard_offer(struct gnome_portal *p, const char *text)
{
	free(p->clip_text);
	p->clip_text = strdup(text);
	p->clip_owner = false;
	static const char *const mimes[] = {"text/plain;charset=utf-8", "text/plain"};
	struct pmsg pm;
	gp_pmsg_begin(&pm, CLIP_IFACE, "SetSelection", p->session, NULL);
	gp_dict_add_strv(&pm.dict, "mime_types", mimes, 2);
	struct gdbus_err err;
	DBusMessage *reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply == NULL) {
		U_LOG_W("desktop/gnome: Clipboard.SetSelection failed: %s %s", err.name, err.message);
		return false;
	}
	dbus_message_unref(reply);
	// Wait (briefly) until mutter made us the owner, so the paste asks us.
	uint64_t deadline = gdbus_now_ms() + 500;
	while (!p->clip_owner && gdbus_now_ms() < deadline && p->session_open) {
		gp_pump(p, 10);
	}
	return p->session_open;
}

static enum u_desktop_result
cmd_paste(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	if (!p->have_clipboard_portal || !p->clipboard_enabled) {
		gp_set_msg(c->msg,
		           "no portal clipboard in this session (xdg-desktop-portal without Clipboard) — text "
		           "is never typed as key codes");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	// The previous text: our own if we still own the selection (reading from
	// ourselves would deadlock this thread), else whatever the owner gives.
	char *prev = NULL;
	if (p->clip_owner && p->clip_text != NULL) {
		prev = strdup(p->clip_text);
	} else {
		prev = clipboard_read(p);
	}

	if (!clipboard_offer(p, c->text)) {
		free(prev);
		gp_set_msg(c->msg, "could not put the text on the clipboard");
		return U_DESKTOP_ERR_FAILED;
	}

	// Ctrl+V matched by keysym: the key producing 'v' on the active layout.
	uint32_t before = p->transfers;
	r = send_chord(p, U_DESKTOP_MOD_CTRL | (c->terminal ? U_DESKTOP_MOD_SHIFT : 0), 0, 'v', c->msg);
	if (r != U_DESKTOP_OK) {
		free(prev);
		return r;
	}

	// Serve the paste: at least one transfer, then a short quiet period (a
	// client may ask for several mime types).
	uint64_t deadline = gdbus_now_ms() + 2000;
	uint64_t quiet_until = 0;
	uint32_t seen = before;
	while (gdbus_now_ms() < deadline && p->session_open) {
		gp_pump(p, 10);
		if (p->transfers != seen) {
			seen = p->transfers;
			quiet_until = gdbus_now_ms() + 150;
		}
		if (quiet_until != 0 && gdbus_now_ms() >= quiet_until) {
			break;
		}
	}
	p->last_use_ms = gdbus_now_ms();
	bool served = p->transfers != before;

	if (prev != NULL && p->session_open) {
		clipboard_offer(p, prev);
	}
	free(prev);
	if (!served) {
		gp_set_msg(c->msg, "the focused window did not ask for the pasted text (does it accept text?)");
		return U_DESKTOP_ERR_FAILED;
	}
	return U_DESKTOP_OK;
}

enum u_desktop_result
gnome_portal_motion(struct gnome_portal *p, int32_t x, int32_t y, char *msg)
{
	struct gp_cmd *c = gp_cmd_new(cmd_motion);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	c->x = x;
	c->y = y;
	return gp_run_sync(p, c, ACTION_TIMEOUT_MS, msg);
}

enum u_desktop_result
gnome_portal_button(struct gnome_portal *p, uint32_t evdev_button, bool down, char *msg)
{
	struct gp_cmd *c = gp_cmd_new(cmd_button);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	c->code = evdev_button;
	c->down = down;
	return gp_run_sync(p, c, ACTION_TIMEOUT_MS, msg);
}

enum u_desktop_result
gnome_portal_scroll(struct gnome_portal *p, int32_t dx, int32_t dy, char *msg)
{
	struct gp_cmd *c = gp_cmd_new(cmd_scroll);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	c->x = dx;
	c->y = dy;
	return gp_run_sync(p, c, ACTION_TIMEOUT_MS, msg);
}

enum u_desktop_result
gnome_portal_keys(struct gnome_portal *p, const uint32_t *codes, const bool *press, uint32_t n, char *msg)
{
	if (n > GNOME_PORTAL_MAX_KEYS) {
		gp_set_msg(msg, "too many keys");
		return U_DESKTOP_ERR_FAILED;
	}
	struct gp_cmd *c = gp_cmd_new(cmd_keys);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	memcpy(c->keys, codes, n * sizeof(codes[0]));
	memcpy(c->press, press, n * sizeof(press[0]));
	c->nkeys = n;
	return gp_run_sync(p, c, ACTION_TIMEOUT_MS, msg);
}

enum u_desktop_result
gnome_portal_chord(struct gnome_portal *p, uint32_t mods, uint32_t evdev_code, uint32_t ch, char *msg)
{
	struct gp_cmd *c = gp_cmd_new(cmd_chord);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	c->mods = mods;
	c->code = evdev_code;
	c->ch = ch;
	return gp_run_sync(p, c, ACTION_TIMEOUT_MS, msg);
}

enum u_desktop_result
gnome_portal_paste_text(struct gnome_portal *p, const char *utf8, bool terminal, char *msg)
{
	struct gp_cmd *c = gp_cmd_new(cmd_paste);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	c->text = strdup(utf8);
	c->terminal = terminal;
	if (c->text == NULL) {
		gp_cmd_free(c);
		return U_DESKTOP_ERR_FAILED;
	}
	return gp_run_sync(p, c, ACTION_TIMEOUT_MS + 3000, msg);
}
