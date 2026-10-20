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

enum u_desktop_result
u_desktop_paste_result(enum u_desktop_paste_verdict v)
{
	switch (v) {
	case U_DESKTOP_PASTE_DELIVERED:
	case U_DESKTOP_PASTE_UNVERIFIED: return U_DESKTOP_OK;
	case U_DESKTOP_PASTE_CHANGED_HANDS: return U_DESKTOP_ERR_REFUSED;
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
