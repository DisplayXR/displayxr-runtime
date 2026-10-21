// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: the backend's decisions as pure functions
 *         (ADR-056). See u_desktop_gnome_logic.h.
 */

#include "u_desktop_gnome_logic.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

int
u_desktop_text_check(const struct u_desktop_text_state *before, const struct u_desktop_text_state *after)
{
	if (!before->found || !after->found) {
		return -1;
	}
	return before->count != after->count || before->caret != after->caret || before->hash != after->hash ? 1 : 0;
}

//! ASCII case-insensitive equality (strcasecmp is POSIX; this file builds everywhere).
static bool
ascii_ieq(const char *a, const char *b)
{
	for (; *a != '\0' && *b != '\0'; a++, b++) {
		char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
		char y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
		if (x != y) {
			return false;
		}
	}
	return *a == *b;
}

uint32_t
u_desktop_gnome_key_code(const char *name)
{
	// evdev codes (linux/input-event-codes.h — a stable kernel ABI; the values are
	// spelled out because this file builds on every OS, and tests_desktop_control
	// checks each against the header on Linux).
	static const struct
	{
		const char *name;
		uint32_t code;
	} t[] = {
	    {"enter", 28},               // KEY_ENTER
	    {"tab", 15},                 // KEY_TAB
	    {"escape", 1},               // KEY_ESC
	    {"backspace", 14},           // KEY_BACKSPACE
	    {"delete", 111},             // KEY_DELETE
	    {"insert", 110},             // KEY_INSERT
	    {"home", 102},               // KEY_HOME
	    {"end", 107},                // KEY_END
	    {"pageup", 104},             // KEY_PAGEUP
	    {"pagedown", 109},           // KEY_PAGEDOWN
	    {"left", 105},               // KEY_LEFT
	    {"right", 106},              // KEY_RIGHT
	    {"up", 103},                 // KEY_UP
	    {"down", 108},               // KEY_DOWN
	    {"space", 57},               // KEY_SPACE
	    {"capslock", 58},            // KEY_CAPSLOCK
	    {"printscreen", 99},         // KEY_SYSRQ
	    {"menu", 127},               // KEY_COMPOSE
	    {"numpad0", 82},             // KEY_KP0
	    {"numpad1", 79},             // KEY_KP1
	    {"numpad2", 80},             // KEY_KP2
	    {"numpad3", 81},             // KEY_KP3
	    {"numpad4", 75},             // KEY_KP4
	    {"numpad5", 76},             // KEY_KP5
	    {"numpad6", 77},             // KEY_KP6
	    {"numpad7", 71},             // KEY_KP7
	    {"numpad8", 72},             // KEY_KP8
	    {"numpad9", 73},             // KEY_KP9
	    {"numpadmultiply", 55},      // KEY_KPASTERISK
	    {"numpadadd", 78},           // KEY_KPPLUS
	    {"numpadsubtract", 74},      // KEY_KPMINUS
	    {"numpaddivide", 98},        // KEY_KPSLASH
	    {"numpaddecimal", 83},       // KEY_KPDOT
	    {"numpadenter", 96},         // KEY_KPENTER
	    {"pause", 119},              // KEY_PAUSE
	    {"numlock", 69},             // KEY_NUMLOCK
	    {"scrolllock", 70},          // KEY_SCROLLLOCK
	    {"mediaplaypause", 164},     // KEY_PLAYPAUSE
	    {"mediatracknext", 163},     // KEY_NEXTSONG
	    {"mediatrackprevious", 165}, // KEY_PREVIOUSSONG
	    {"mediastop", 166},          // KEY_STOPCD
	    {"audiovolumeup", 115},      // KEY_VOLUMEUP
	    {"audiovolumedown", 114},    // KEY_VOLUMEDOWN
	    {"audiovolumemute", 113},    // KEY_MUTE
	    {"rightctrl", 97},           // KEY_RIGHTCTRL
	    {"rightshift", 54},          // KEY_RIGHTSHIFT
	    {"rightalt", 100},           // KEY_RIGHTALT
	    {"rightsuper", 126},         // KEY_RIGHTMETA
	};
	if (name == NULL) {
		return 0;
	}
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (ascii_ieq(name, t[i].name)) {
			return t[i].code;
		}
	}
	// f1..f24: KEY_F1..KEY_F10 = 59..68, KEY_F11 = 87, KEY_F12 = 88, KEY_F13..KEY_F24 = 183..194.
	if ((name[0] == 'f' || name[0] == 'F') && name[1] >= '1' && name[1] <= '9' &&
	    (name[2] == '\0' || (name[2] >= '0' && name[2] <= '9' && name[3] == '\0'))) {
		int n = name[1] - '0';
		if (name[2] != '\0') {
			n = n * 10 + (name[2] - '0');
		}
		if (n >= 1 && n <= 10) {
			return 59u + (uint32_t)(n - 1);
		}
		if (n == 11) {
			return 87;
		}
		if (n == 12) {
			return 88;
		}
		if (n >= 13 && n <= 24) {
			return 183u + (uint32_t)(n - 13);
		}
	}
	return 0;
}

uint32_t
u_desktop_gnome_key_char(const char *name)
{
	if (name == NULL) {
		return 0;
	}
	if (strlen(name) == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9'))) {
		return (uint32_t)name[0];
	}
	if (strlen(name) == 1 && name[0] >= 'A' && name[0] <= 'Z') {
		return (uint32_t)(name[0] - 'A' + 'a'); // the key, not Shift+key: modifiers are explicit
	}
	static const struct
	{
		const char *name;
		char ch;
	} t[] = {
	    {"minus", '-'},       {"equal", '='},        {"comma", ','},     {"period", '.'},
	    {"slash", '/'},       {"backslash", '\\'},   {"semicolon", ';'}, {"quote", '\''},
	    {"bracketleft", '['}, {"bracketright", ']'}, {"grave", '`'},
	};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (ascii_ieq(name, t[i].name)) {
			return (uint32_t)(unsigned char)t[i].ch;
		}
	}
	return 0;
}

uint64_t
u_desktop_fnv1a(const char *s, size_t len)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < len; i++) {
		h ^= (uint8_t)s[i];
		h *= 1099511628211ull;
	}
	return h;
}

enum u_desktop_paste_verdict
u_desktop_paste_decide(const struct u_desktop_paste_obs *o, bool *restore)
{
	bool lost = o->lost_before_key || o->lost_after_key;
	*restore = !lost;
	if (o->lost_before_key) {
		return U_DESKTOP_PASTE_CHANGED_HANDS;
	}
	if (o->lost_after_key) {
		// The user copied after the keystroke. A field that changed proves
		// nothing on its own: the target may have read the USER's new clipboard
		// (GNOME 50.1: 37/80 such trials changed the field with the user's text).
		if (o->writes_after_key == 0) {
			return U_DESKTOP_PASTE_LOST_NOT_SERVED; // nobody ever got the agent's text
		}
		return o->text_check > 0 ? U_DESKTOP_PASTE_DELIVERED : U_DESKTOP_PASTE_LOST_UNCERTAIN;
	}
	if (o->text_check > 0) {
		return U_DESKTOP_PASTE_DELIVERED;
	}
	if (o->writes_after_key == 0) {
		return U_DESKTOP_PASTE_NOT_ASKED;
	}
	if (o->text_check == 0) {
		return U_DESKTOP_PASTE_FIELD_UNCHANGED;
	}
	return U_DESKTOP_PASTE_UNVERIFIED;
}

enum u_desktop_clip_end
u_desktop_clip_end_decide(bool changed_hands, bool owner, bool have_prev)
{
	if (!owner) {
		return U_DESKTOP_CLIP_END_LEAVE; // someone else's (the user's), or nobody's
	}
	if (changed_hands || !have_prev) {
		return U_DESKTOP_CLIP_END_RELEASE; // never the old text over a user copy; never owned + empty
	}
	return U_DESKTOP_CLIP_END_RESTORE;
}

enum u_desktop_result
u_desktop_paste_result(enum u_desktop_paste_verdict v)
{
	switch (v) {
	case U_DESKTOP_PASTE_DELIVERED:
	case U_DESKTOP_PASTE_UNVERIFIED: return U_DESKTOP_OK;
	case U_DESKTOP_PASTE_CHANGED_HANDS:
	case U_DESKTOP_PASTE_CHANGED_HANDS_REPLACED: return U_DESKTOP_ERR_REFUSED;
	default: return U_DESKTOP_ERR_FAILED;
	}
}

const char *
u_desktop_paste_message(enum u_desktop_paste_verdict v)
{
	switch (v) {
	case U_DESKTOP_PASTE_DELIVERED: return "";
	case U_DESKTOP_PASTE_UNVERIFIED:
		return U_DESKTOP_MSG_UNVERIFIED
		    "the window read the text after the paste, but no accessible text "
		    "field confirmed it was inserted (a clipboard manager reads too)";
	case U_DESKTOP_PASTE_CHANGED_HANDS:
		return "the clipboard changed hands (the user copied something); nothing was pasted";
	case U_DESKTOP_PASTE_LOST_UNCERTAIN:
		return "lost the clipboard during the paste (the user copied something) — whether the text "
		       "arrived is unknown; the user's clipboard was left as is";
	case U_DESKTOP_PASTE_NOT_ASKED:
		return "the focused window did not ask for the pasted text (does it accept text?)";
	case U_DESKTOP_PASTE_FIELD_UNCHANGED:
		return "the focused text field did not change — the paste was not seen to arrive";
	case U_DESKTOP_PASTE_CHANGED_HANDS_REPLACED:
		return "the clipboard changed hands (the user copied something) just as the agent's text was "
		       "being offered — nothing was pasted, but the offer had already replaced what the user "
		       "copied, which is gone (copy it again); the clipboard was released and is empty";
	case U_DESKTOP_PASTE_LOST_NOT_SERVED:
		return "the clipboard changed hands right after the paste keystroke (the user copied something) "
		       "and the agent's text was never read — whatever the window pasted was the user's "
		       "clipboard, not the agent's text; the user's clipboard was left as is";
	}
	return "";
}

void
u_desktop_success_message(const char *backend_msg, char *out, size_t cap)
{
	const size_t pre = sizeof(U_DESKTOP_MSG_UNVERIFIED) - 1;
	if (backend_msg != NULL && strncmp(backend_msg, U_DESKTOP_MSG_UNVERIFIED, pre) == 0) {
		snprintf(out, cap, "ok (unverified: %s)", backend_msg + pre);
	} else {
		snprintf(out, cap, "ok");
	}
}

uint32_t
u_desktop_cross_dwell_left_ms(uint64_t crossed_at_ms, uint64_t now_ms, uint32_t dwell_ms)
{
	if (crossed_at_ms == 0 || now_ms >= crossed_at_ms + dwell_ms) {
		return 0;
	}
	if (now_ms < crossed_at_ms) {
		return dwell_ms; // a clock that went backwards: wait the whole dwell
	}
	return (uint32_t)(crossed_at_ms + dwell_ms - now_ms);
}

bool
u_desktop_frame_moved(const struct u_desktop_rect *before, const struct u_desktop_rect *after, int32_t tol)
{
	const int64_t a[4] = {before->x, before->y, (int64_t)before->x + before->w, (int64_t)before->y + before->h};
	const int64_t b[4] = {after->x, after->y, (int64_t)after->x + after->w, (int64_t)after->y + after->h};
	for (int i = 0; i < 4; i++) {
		int64_t d = a[i] - b[i];
		if (d > tol || -d > tol) {
			return true;
		}
	}
	return false;
}

bool
u_desktop_dbus_error_is_no_answer(const char *error_name)
{
	if (error_name == NULL || error_name[0] == '\0') {
		return true;
	}
	// The portal (or the bus on its behalf) answered: it does not have this.
	static const char *const answers[] = {
	    "org.freedesktop.DBus.Error.UnknownInterface", "org.freedesktop.DBus.Error.UnknownProperty",
	    "org.freedesktop.DBus.Error.UnknownObject",    "org.freedesktop.DBus.Error.UnknownMethod",
	    "org.freedesktop.DBus.Error.InvalidArgs", // GDBus: "No such interface …" on Properties.Get
	    "org.freedesktop.DBus.Error.AccessDenied",
	};
	for (size_t i = 0; i < sizeof(answers) / sizeof(answers[0]); i++) {
		if (strcmp(error_name, answers[i]) == 0) {
			return false;
		}
	}
	// NoReply / Timeout / TimedOut / ServiceUnknown / NameHasNoOwner / Spawn.* /
	// Disconnected / NoMemory / anything unknown: no answer yet — retry.
	return true;
}

bool
u_desktop_retry_due(const struct u_desktop_retry *r, uint64_t now_ms)
{
	return !r->known && now_ms >= r->next_ms;
}

void
u_desktop_retry_record(struct u_desktop_retry *r, bool answered, uint64_t now_ms, uint32_t period_ms)
{
	if (answered) {
		r->known = true;
		r->next_ms = 0;
	} else {
		r->next_ms = now_ms + period_ms;
	}
}

void
u_desktop_layout_settle_reset(struct u_desktop_layout_settle *s, uint64_t fingerprint)
{
	s->fingerprint = fingerprint;
	s->changed_ms = 0;
	s->present_ms = 0;
	s->was_missing = false;
}

uint32_t
u_desktop_layout_settle_step(
    struct u_desktop_layout_settle *s, uint64_t now_ms, uint64_t fingerprint, uint32_t missing, uint32_t settle_ms)
{
	if (s->fingerprint != 0 && fingerprint != s->fingerprint) {
		s->changed_ms = now_ms; // the LAST change, not the first
	}
	s->fingerprint = fingerprint;
	if (missing > 0) {
		s->was_missing = true;
		return U_DESKTOP_LAYOUT_MISSING;
	}
	if (s->was_missing) {
		s->was_missing = false;
		s->present_ms = now_ms; // all covered monitors present from now
	}
	uint64_t since = s->changed_ms > s->present_ms ? s->changed_ms : s->present_ms;
	if (since == 0 || now_ms >= since + settle_ms) {
		return 0;
	}
	if (now_ms < since) {
		return settle_ms; // a clock that went backwards: wait the whole time
	}
	return (uint32_t)(since + settle_ms - now_ms);
}

bool
u_desktop_shell_cover_appeared(const uint8_t *before, const uint8_t *after, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		// A point not read before (a failed pick) proves nothing: never ours to dismiss.
		bool known_before = before[i] <= U_DESKTOP_PICK_OUTSIDE;
		if (after[i] == U_DESKTOP_PICK_SHELL && known_before && before[i] != U_DESKTOP_PICK_SHELL) {
			return true;
		}
	}
	return false;
}

#ifndef _WIN32
ptrdiff_t
u_desktop_write_nosigpipe(int fd, const void *buf, size_t len)
{
	sigset_t pipe_set, old_set, pending;
	sigemptyset(&pipe_set);
	sigaddset(&pipe_set, SIGPIPE);
	// A SIGPIPE already pending (blocked by the thread before us) is not ours to eat.
	sigpending(&pending);
	bool was_pending = sigismember(&pending, SIGPIPE) == 1;
	pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);

	ssize_t w = write(fd, buf, len);
	int saved = errno;

	if (w < 0 && saved == EPIPE && !was_pending) {
		// The write raised SIGPIPE at this (now blocking) thread: consume it.
#ifdef __linux__
		struct timespec zero = {0, 0};
		while (sigtimedwait(&pipe_set, NULL, &zero) < 0 && errno == EINTR) {
		}
#else
		// No sigtimedwait (macOS): sigwait returns at once for a pending signal.
		sigpending(&pending);
		if (sigismember(&pending, SIGPIPE) == 1) {
			int sig = 0;
			sigwait(&pipe_set, &sig);
		}
#endif
	}
	pthread_sigmask(SIG_SETMASK, &old_set, NULL);
	errno = saved;
	return (ptrdiff_t)w;
}
#endif
