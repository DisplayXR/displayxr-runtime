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
#include "u_desktop_gnome_logic.h"

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
/*!
 * A new session's virtual keyboard: no key until it has been resumed this long.
 * Every RemoteDesktop session (also the silent restore after
 * SESSION_IDLE_CLOSE_MS without input) brings a new libei keyboard. While mutter
 * switches to it, the focused client gets wl_keyboard.leave, and enter again
 * 1-9 ms after the device is resumed (GNOME 50, nested shell, 18 of 18). A key
 * sent right after the resume raced that enter: 4 of 21 first keys after a
 * restore were lost, and a GtkEntry selects all on the enter, so a key that did
 * arrive replaced the field. Only a session's first keys wait; a burst does not.
 */
#define GNOME_KEYBOARD_SETTLE_MS 300
//! The agent's text stays the clipboard at least this long after the paste keystroke.
#define PASTE_SERVE_MS 1200
//! A write arriving late keeps serving this much longer (more mime types may follow).
#define PASTE_QUIET_MS 150
//! The whole paste, at most; no write of the agent's text by then = not delivered.
#define PASTE_MAX_MS 2500

/*!
 * Input was injected: the idle close counts from now, and prepare_input's hold
 * (kept for the verification + idle wait BEFORE the injection) is over — so
 * GNOME's remote-access indicator goes away SESSION_IDLE_CLOSE_MS after the
 * last action, not GNOME_PREPARE_HOLD_MS after the last prepare.
 */
static void
injected(struct gnome_portal *p)
{
	p->last_use_ms = gdbus_now_ms();
	p->hold_until_ms = 0;
}

static bool
is_text_mime(const char *mime)
{
	return mime != NULL &&
	       (strcmp(mime, "text/plain;charset=utf-8") == 0 || strcmp(mime, "text/plain") == 0 ||
	        strcmp(mime, "UTF8_STRING") == 0 || strcmp(mime, "STRING") == 0 || strcmp(mime, "TEXT") == 0);
}

/*!
 * Write all of @p data to @p fd within @p timeout_ms. The reader can go away
 * mid-write (the selection changed hands ~10 ms into a type): EPIPE is "the
 * transfer was cancelled", never a SIGPIPE — this runs in the APP's process, so
 * the write blocks SIGPIPE for this thread only (u_desktop_write_nosigpipe).
 */
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
		ptrdiff_t w = u_desktop_write_nosigpipe(fd, data + off, len - off);
		if (w > 0) {
			off += (size_t)w;
			continue;
		}
		if (w < 0 && errno == EPIPE) {
			U_LOG_I("desktop/gnome: a clipboard reader went away mid-transfer (cancelled)");
			return false;
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
	// Never hand out the agent's text once the selection changed hands during
	// this type: a read now is of a selection we set over the user's copy
	// (mutter's clipboard manager keeping a copy of it, typically) — the agent's
	// text must not become what the clipboard holds after we release it.
	bool withheld = p->clip_is_paste && p->clip_foreign_gen != p->clip_take_gen;
	if (withheld) {
		U_LOG_I("desktop/gnome: not serving the agent's text — the clipboard changed hands");
	}
	if (!withheld && p->clip_text != NULL && is_text_mime(mime)) {
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
	// Count only a write of the AGENT's text, after the paste keystroke, once per
	// serial: a clipboard manager reading the new selection must not make an
	// unanswered paste look delivered.
	if (ok && p->paste_armed && p->clip_is_paste && (!p->paste_have_serial || serial != p->paste_last_serial)) {
		p->paste_writes++;
		p->paste_last_serial = serial;
		p->paste_have_serial = true;
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
	enum u_desktop_result r = gp_ensure_session(p, c->restore, c->msg);
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
	injected(p);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
cmd_button(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, c->restore, c->msg);
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
	injected(p);
	return U_DESKTOP_OK;
}

/*!
 * A button press AT (x, y): the absolute motion and the press in ONE libei frame
 * on a device that has both, so pointer motion from the user (a wiggling mouse)
 * cannot land between them and move the press off the verified point. A
 * session with no such device sends the motion frame, then the press frame.
 */
static enum u_desktop_result
cmd_press_at(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, false, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	struct ei_device *d = gp_find_dev(p, EIX_CAP_POINTER_ABSOLUTE, false);
	struct ei_device *bd = gp_find_dev(p, EIX_CAP_BUTTON, true);
	if (d == NULL || bd == NULL) {
		gp_set_msg(c->msg, "the remote-desktop session has no absolute pointer with buttons");
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
	if (bd != d) {
		g_ei.device_frame(d, g_ei.now(p->ei));
	}
	g_ei.button_button(bd, c->code, true);
	g_ei.device_frame(bd, g_ei.now(p->ei));
	gdbus_sleep_ms(KEY_GAP_MS);
	injected(p);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
cmd_scroll(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, c->restore, c->msg);
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
	injected(p);
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
	uint64_t now = gdbus_now_ms();
	if (p->kbd_resumed_ms != 0 && now < p->kbd_resumed_ms + GNOME_KEYBOARD_SETTLE_MS) {
		gdbus_sleep_ms((uint32_t)(p->kbd_resumed_ms + GNOME_KEYBOARD_SETTLE_MS - now));
	}
	for (uint32_t i = 0; i < n; i++) {
		g_ei.keyboard_key(d, codes[i], press[i]);
		g_ei.device_frame(d, g_ei.now(p->ei));
		gdbus_sleep_ms(KEY_GAP_MS);
	}
	injected(p);
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



static enum u_desktop_result
cmd_keys(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, c->restore, c->msg);
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



/*!
 * Map a chord's key to an evdev code: @p ch (a character, looked up on the
 * session keymap's active layout; Shift added when it sits on the Shift level)
 * or, when @p ch is 0, the fixed evdev @p code. Messages never name the
 * character (privacy: a chord can be part of what the agent types).
 */
static enum u_desktop_result
resolve_chord(struct gnome_portal *p,
              uint32_t mods,
              uint32_t code,
              uint32_t ch,
              uint32_t *out_mods,
              uint32_t *out_code,
              char *msg)
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
				gp_set_msg(msg,
				           "the active keyboard layout has no key for that character (without AltGr)");
				return U_DESKTOP_ERR_UNSUPPORTED;
			}
		} else {
			code = us_code_for_char(ch);
			if (code == 0) {
				gp_set_msg(msg, "no key for that character");
				return U_DESKTOP_ERR_UNSUPPORTED;
			}
		}
		if (shift) {
			mods |= U_DESKTOP_MOD_SHIFT;
		}
	}
	*out_mods = mods;
	*out_code = code;
	return U_DESKTOP_OK;
}

static const struct
{
	uint32_t bit, code;
} k_modkeys[] = {
    {U_DESKTOP_MOD_CTRL, KEY_LEFTCTRL},
    {U_DESKTOP_MOD_SHIFT, KEY_LEFTSHIFT},
    {U_DESKTOP_MOD_ALT, KEY_LEFTALT},
    {U_DESKTOP_MOD_SUPER, KEY_LEFTMETA},
};

uint32_t
gnome_chord_mod_codes(uint32_t mods, uint32_t *codes)
{
	uint32_t n = 0;
	for (size_t i = 0; i < 4; i++) {
		if (mods & k_modkeys[i].bit) {
			codes[n++] = k_modkeys[i].code;
		}
	}
	return n;
}

static enum u_desktop_result
cmd_prepare(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, true, c->msg);
	if (r == U_DESKTOP_OK) {
		p->hold_until_ms = gdbus_now_ms() + GNOME_PREPARE_HOLD_MS;
	}
	return r;
}

static enum u_desktop_result
cmd_resolve(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, false, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	return resolve_chord(p, c->mods, c->code, c->ch, &c->out_mods, &c->out_code, c->msg);
}

/*!
 * Offer @p text as the clipboard. True only once the PORTAL said this session
 * owns the selection: otherwise a paste would insert whatever the previous owner
 * holds (possibly a copied password) into the target.
 */
static bool
clipboard_offer(struct gnome_portal *p, const char *text, bool is_paste)
{
	free(p->clip_text);
	p->clip_text = strdup(text);
	p->clip_is_paste = is_paste && p->clip_text != NULL;
	p->clip_owner = false;
	p->clip_lost = false;
	if (p->clip_text == NULL) {
		return false;
	}
	static const char *const mimes[] = {"text/plain;charset=utf-8", "text/plain"};
	struct pmsg pm;
	gp_pmsg_begin(&pm, CLIP_IFACE, "SetSelection", p->session, NULL);
	gp_dict_add_strv(&pm.dict, "mime_types", mimes, 2);
	struct gdbus_err err;
	DBusMessage *reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply == NULL) {
		U_LOG_W("desktop/gnome: Clipboard.SetSelection failed: %s", err.name);
		return false;
	}
	dbus_message_unref(reply);
	// Wait (briefly) until mutter made us the owner, so the paste asks us.
	uint64_t deadline = gdbus_now_ms() + 500;
	while (!p->clip_owner && gdbus_now_ms() < deadline && p->session_open && !gp_quitting(p)) {
		gp_pump(p, 10);
	}
	return p->session_open && p->clip_owner;
}

/*!
 * The user copied since this type started — from before the previous text was
 * read, through our SetSelection, the keystroke and the serve: any of it means
 * the selection is theirs now, and must neither be pasted nor overwritten.
 */
static bool
clip_changed_hands(const struct gnome_portal *p)
{
	return p->clip_foreign_gen != p->clip_take_gen;
}

/*!
 * Give up the selection: SetSelection with no mime_types (the portal passes
 * the empty options on; mutter then unsets this session's source — a no-op if
 * another client already owns the selection). Waits briefly for the portal's
 * SelectionOwnerChanged, so it is not taken for a user copy by the next type.
 */
static void
clipboard_release(struct gnome_portal *p)
{
	p->clip_is_paste = false; // our own release is not a loss of the agent's text
	free(p->clip_text);
	p->clip_text = NULL;
	struct pmsg pm;
	gp_pmsg_begin(&pm, CLIP_IFACE, "SetSelection", p->session, NULL);
	struct gdbus_err err;
	DBusMessage *reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply == NULL) {
		U_LOG_W("desktop/gnome: releasing the clipboard (SetSelection, no mime types) failed: %s", err.name);
		return;
	}
	dbus_message_unref(reply);
	uint64_t deadline = gdbus_now_ms() + 100;
	while (p->clip_owner && gdbus_now_ms() < deadline && p->session_open && !gp_quitting(p)) {
		gp_pump(p, 10);
	}
	gp_pump(p, 0);
}

/*!
 * The end of a type for the clipboard (u_desktop_clip_end_decide): offer the
 * text from before the paste again — unless the selection changed hands at any
 * time since the type began (the user copied something newer, and the old text
 * would overwrite it). A selection we still hold but won't restore is RELEASED,
 * never kept serving nothing (owned and empty). Either way the agent's text is
 * no longer served.
 */
static void
clipboard_restore(struct gnome_portal *p)
{
	gp_pump(p, 0); // a SelectionOwnerChanged already queued counts
	bool changed = p->clip_lost || clip_changed_hands(p);
	enum u_desktop_clip_end end = p->session_open
	                                  ? u_desktop_clip_end_decide(changed, p->clip_owner, p->clip_prev != NULL)
	                                  : U_DESKTOP_CLIP_END_LEAVE;
	char *prev = p->clip_prev;
	p->clip_prev = NULL;
	p->clip_lost = false;
	switch (end) {
	case U_DESKTOP_CLIP_END_RESTORE: clipboard_offer(p, prev, false); break;
	case U_DESKTOP_CLIP_END_RELEASE: clipboard_release(p); break;
	case U_DESKTOP_CLIP_END_LEAVE:
		free(p->clip_text);
		p->clip_text = NULL;
		p->clip_is_paste = false;
		break;
	}
	free(prev);
}

static enum u_desktop_result
cmd_clip_take(struct gnome_portal *p, struct gp_cmd *c)
{
	bool was_open = p->session_open;
	enum u_desktop_result r = gp_ensure_session(p, false, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	if (!p->have_clipboard_portal || !p->clipboard_enabled) {
		gp_set_msg(c->msg,
		           "no portal clipboard in this session (xdg-desktop-portal without Clipboard) — text "
		           "is never typed as key codes");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	// Ownership changes count from HERE, before the previous text is read: a copy
	// between SelectionRead and our SetSelection would otherwise be overwritten
	// by the agent's text (and its signal, arriving while we were not the owner
	// yet, would not set clip_lost). A session opened just now first gets a
	// moment for the portal's initial owner state to arrive, so it is not taken
	// for a copy.
	if (!was_open) {
		uint64_t settle = gdbus_now_ms() + 50;
		while (gdbus_now_ms() < settle && p->session_open) {
			gp_pump(p, 10);
		}
	}
	gp_pump(p, 0);
	p->clip_take_gen = p->clip_foreign_gen;
	// The previous text: if we still own the selection, what we were serving
	// (reading from ourselves would deadlock this thread), else the owner's.
	free(p->clip_prev);
	p->clip_prev = NULL;
	if (p->clip_owner && p->clip_text != NULL && !p->clip_is_paste) {
		p->clip_prev = strdup(p->clip_text);
	} else if (!p->clip_owner) {
		p->clip_prev = clipboard_read(p);
	}
	// Signals queued while SelectionRead blocked: a copy now is the user's.
	gp_pump(p, 0);
	if (clip_changed_hands(p)) {
		clipboard_restore(p); // offers nothing: the selection is the user's
		c->obs.lost_before_key = true;
		gp_set_msg(c->msg, "%s", u_desktop_paste_message(U_DESKTOP_PASTE_CHANGED_HANDS));
		return U_DESKTOP_ERR_REFUSED;
	}
	bool owned = clipboard_offer(p, c->text, true);
	gp_pump(p, 0);
	if (clip_changed_hands(p)) {
		// The user copied between the read and our SetSelection (or right after
		// it): too late not to have offered, but never paste and never restore —
		// the old text would overwrite theirs again.
		// Still the owner: the user's copy came BEFORE our SetSelection, which
		// replaced it — that data is gone (mutter dropped its copy of it for
		// ours, and we never serve ours now). Not the owner: the user copied
		// after it and their copy stands.
		bool replaced = p->clip_owner;
		U_LOG_W("desktop/gnome: the clipboard changed hands while taking it — not pasting%s",
		        replaced ? " (our offer replaced the user's copy; releasing)" : "");
		clipboard_restore(p); // releases a selection we still hold
		c->obs.lost_before_key = true;
		c->obs.replaced = replaced;
		gp_set_msg(c->msg, "%s",
		           u_desktop_paste_message(replaced ? U_DESKTOP_PASTE_CHANGED_HANDS_REPLACED
		                                            : U_DESKTOP_PASTE_CHANGED_HANDS));
		return U_DESKTOP_ERR_REFUSED;
	}
	if (!owned) {
		clipboard_restore(p);
		gp_set_msg(c->msg, "the portal did not make this session the clipboard owner — not pasting");
		return U_DESKTOP_ERR_FAILED;
	}
	return U_DESKTOP_OK;
}

/*!
 * The paste keystroke, then serving the paste. Fills c->obs; the caller decides
 * the outcome (u_desktop_paste_decide, with its own AT-SPI check) and then
 * restores the previous clipboard (cmd_clip_restore) — the agent's text is
 * served until then.
 */
static enum u_desktop_result
cmd_clip_paste(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, false, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	// The last look before the keystroke: a SelectionOwnerChanged already queued
	// (the user copied after we took the selection) must be seen NOW — Ctrl+V
	// would paste the user's new clipboard into the target.
	gp_pump(p, 0);
	if (p->clip_lost || clip_changed_hands(p) || !p->clip_owner || !p->clip_is_paste) {
		c->obs.lost_before_key = true;
		clipboard_restore(p); // never offers the old text over the user's new one
		gp_set_msg(c->msg, "%s", u_desktop_paste_message(U_DESKTOP_PASTE_CHANGED_HANDS));
		return U_DESKTOP_ERR_REFUSED;
	}
	uint32_t mods = 0, code = 0;
	r = resolve_chord(p, U_DESKTOP_MOD_CTRL | (c->terminal ? U_DESKTOP_MOD_SHIFT : 0), 0, 'v', &mods, &code,
	                  c->msg);
	if (r != U_DESKTOP_OK) {
		clipboard_restore(p);
		return r;
	}
	uint32_t mc[4], mup[4];
	bool down[4] = {true, true, true, true}, up[4] = {false, false, false, false};
	uint32_t nm = gnome_chord_mod_codes(mods, mc);
	for (uint32_t i = 0; i < nm; i++) {
		mup[i] = mc[nm - 1 - i];
	}
	r = send_keys(p, mc, down, nm, c->msg);
	if (r != U_DESKTOP_OK) {
		clipboard_restore(p);
		return r;
	}
	// The modifiers are down; V is next. Look once more, as late as possible: a
	// SelectionOwnerChanged queued during the modifier gaps (GNOME 50.1: it
	// arrived ~9 ms before V reached the target) means V would paste the user's
	// new clipboard. Then release the modifiers without V: nothing pasted. From
	// this check to V there is only the libei frame — no sleep, no D-Bus call.
	gp_pump(p, 0);
	if (p->clip_lost || clip_changed_hands(p) || !p->clip_owner) {
		send_keys(p, mup, up, nm, c->msg);
		c->obs.lost_before_key = true;
		clipboard_restore(p);
		gp_set_msg(c->msg, "%s", u_desktop_paste_message(U_DESKTOP_PASTE_CHANGED_HANDS));
		return U_DESKTOP_ERR_REFUSED;
	}
	p->paste_armed = true;
	p->paste_writes = 0;
	p->paste_have_serial = false;
	uint32_t vk[2] = {code, code};
	bool vp[2] = {true, false};
	r = send_keys(p, vk, vp, 2, c->msg);
	if (r == U_DESKTOP_OK) {
		r = send_keys(p, mup, up, nm, c->msg);
	}
	if (r != U_DESKTOP_OK) {
		p->paste_armed = false;
		clipboard_restore(p);
		return r;
	}

	// Serve the paste. The agent's text stays the clipboard for PASTE_SERVE_MS
	// after the keystroke whoever reads it first: a clipboard manager often reads
	// the new selection before the target does, and restoring the previous text
	// after THAT read would let a slow target paste the user's previous clipboard
	// (and a trailing "\n" would then submit it). Every SelectionTransfer in the
	// window gets the agent's text; a write still arriving late extends it by
	// PASTE_QUIET_MS (a client may ask for several mime types), up to
	// PASTE_MAX_MS. No write at all by PASTE_MAX_MS: nobody asked. Losing the
	// selection (the user copied) ends it: nothing more will be asked of us.
	uint64_t start = gdbus_now_ms();
	uint64_t serve_until = start + PASTE_SERVE_MS;
	uint64_t hard_stop = start + PASTE_MAX_MS;
	uint32_t seen = 0;
	while (p->session_open && !gp_quitting(p) && !p->clip_lost && !clip_changed_hands(p)) {
		uint64_t now = gdbus_now_ms();
		if (now >= hard_stop || (now >= serve_until && p->paste_writes > 0)) {
			break;
		}
		gp_pump(p, 10);
		if (p->paste_writes != seen) {
			seen = p->paste_writes;
			uint64_t quiet = gdbus_now_ms() + PASTE_QUIET_MS;
			if (quiet > serve_until) {
				serve_until = quiet;
			}
		}
	}
	gp_pump(p, 0); // a copy (or a write) queued at the very end still counts
	p->paste_armed = false;
	injected(p);
	c->obs.lost_after_key = p->clip_lost || clip_changed_hands(p);
	c->obs.writes_after_key = p->paste_writes;
	return U_DESKTOP_OK;
}

static enum u_desktop_result
cmd_clip_restore(struct gnome_portal *p, struct gp_cmd *c)
{
	(void)c;
	clipboard_restore(p);
	return U_DESKTOP_OK;
}

//! The absolute pointer's regions (one per monitor, stage px), into c->text as packed rects.
static enum u_desktop_result
cmd_monitors(struct gnome_portal *p, struct gp_cmd *c)
{
	enum u_desktop_result r = gp_ensure_session(p, false, c->msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	struct ei_device *d = gp_find_dev(p, EIX_CAP_POINTER_ABSOLUTE, false);
	if (d == NULL) {
		gp_set_msg(c->msg, "the remote-desktop session has no absolute pointer");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	struct ei_region *reg;
	uint32_t n = 0;
	for (size_t i = 0; n < GNOME_MAX_MONITORS && (reg = g_ei.device_get_region(d, i)) != NULL; i++) {
		c->mon[n++] =
		    (struct u_desktop_rect){(int32_t)g_ei.region_get_x(reg), (int32_t)g_ei.region_get_y(reg),
		                            (int32_t)g_ei.region_get_width(reg), (int32_t)g_ei.region_get_height(reg)};
	}
	c->nmon = n;
	return n > 0 ? U_DESKTOP_OK : U_DESKTOP_ERR_FAILED;
}

enum u_desktop_result
gnome_portal_monitors(struct gnome_portal *p, struct u_desktop_rect *out, uint32_t cap, uint32_t *count, char *msg)
{
	*count = 0;
	struct gp_cmd *c = gp_cmd_new(cmd_monitors);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	struct gp_cmd copy;
	enum u_desktop_result r = gp_run_sync_keep(p, c, ACTION_TIMEOUT_MS, msg, &copy);
	if (r == U_DESKTOP_OK) {
		uint32_t n = copy.nmon < cap ? copy.nmon : cap;
		memcpy(out, copy.mon, n * sizeof(*out));
		*count = n;
	}
	return r;
}

enum u_desktop_result
gnome_portal_motion_ex(struct gnome_portal *p, int32_t x, int32_t y, bool restore, char *msg)
{
	struct gp_cmd *c = gp_cmd_new(cmd_motion);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	c->x = x;
	c->y = y;
	c->restore = restore;
	return gp_run_sync(p, c, ACTION_TIMEOUT_MS, msg);
}

enum u_desktop_result
gnome_portal_motion(struct gnome_portal *p, int32_t x, int32_t y, char *msg)
{
	return gnome_portal_motion_ex(p, x, y, true, msg);
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
gnome_portal_press_at(struct gnome_portal *p, uint32_t evdev_button, int32_t x, int32_t y, char *msg)
{
	struct gp_cmd *c = gp_cmd_new(cmd_press_at);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	c->code = evdev_button;
	c->x = x;
	c->y = y;
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
gnome_portal_prepare(struct gnome_portal *p, char *msg)
{
	struct gp_cmd *c = gp_cmd_new(cmd_prepare);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	return gp_run_sync(p, c, ACTION_TIMEOUT_MS, msg);
}

enum u_desktop_result
gnome_portal_resolve_chord(struct gnome_portal *p,
                           uint32_t mods,
                           uint32_t evdev_code,
                           uint32_t ch,
                           uint32_t *out_mods,
                           uint32_t *out_code,
                           char *msg)
{
	struct gp_cmd *c = gp_cmd_new(cmd_resolve);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	c->mods = mods;
	c->code = evdev_code;
	c->ch = ch;
	// gp_run_sync frees c: copy the outputs through the message-free path below.
	struct gp_cmd copy;
	enum u_desktop_result r = gp_run_sync_keep(p, c, ACTION_TIMEOUT_MS, msg, &copy);
	if (r == U_DESKTOP_OK) {
		*out_mods = copy.out_mods;
		*out_code = copy.out_code;
	}
	return r;
}

enum u_desktop_result
gnome_portal_clip_take(struct gnome_portal *p, const char *utf8, struct u_desktop_paste_obs *obs, char *msg)
{
	memset(obs, 0, sizeof(*obs));
	obs->text_check = -1;
	struct gp_cmd *c = gp_cmd_new(cmd_clip_take);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	c->text = strdup(utf8);
	if (c->text == NULL) {
		gp_cmd_free(c);
		return U_DESKTOP_ERR_FAILED;
	}
	struct gp_cmd copy;
	enum u_desktop_result r = gp_run_sync_keep(p, c, ACTION_TIMEOUT_MS, msg, &copy);
	if (r == U_DESKTOP_ERR_REFUSED) {
		obs->lost_before_key = copy.obs.lost_before_key;
		obs->replaced = copy.obs.replaced;
	}
	return r;
}

enum u_desktop_result
gnome_portal_clip_paste(struct gnome_portal *p, bool terminal, struct u_desktop_paste_obs *obs, char *msg)
{
	memset(obs, 0, sizeof(*obs));
	obs->text_check = -1;
	struct gp_cmd *c = gp_cmd_new(cmd_clip_paste);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	c->terminal = terminal;
	struct gp_cmd copy;
	enum u_desktop_result r = gp_run_sync_keep(p, c, ACTION_TIMEOUT_MS, msg, &copy);
	if (r == U_DESKTOP_OK || r == U_DESKTOP_ERR_REFUSED) {
		obs->lost_before_key = copy.obs.lost_before_key;
		obs->lost_after_key = copy.obs.lost_after_key;
		obs->writes_after_key = copy.obs.writes_after_key;
	}
	return r;
}

void
gnome_portal_clip_restore(struct gnome_portal *p)
{
	struct gp_cmd *c = gp_cmd_new(cmd_clip_restore);
	if (c != NULL) {
		gp_run_sync(p, c, ACTION_TIMEOUT_MS, NULL);
	}
}
