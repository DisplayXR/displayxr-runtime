// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-062 D4 (tier 3): the session-facing window-visibility verdict
 *         and the demotion rule.
 * @ingroup aux_util
 */

#include "util/u_window_visibility_report.h"
#include "util/u_debug.h"

#include <string.h>

#ifdef _WIN32
#define wvr_stricmp _stricmp
#else
#include <strings.h>
#define wvr_stricmp strcasecmp
#endif

DEBUG_GET_ONCE_OPTION(hidden_demote_force, U_WINDOW_VISIBILITY_ENV_DEMOTE_FORCE, "off")

enum xrt_window_visibility_reason
u_wv_report_reason(enum u_wv_reason r)
{
	switch (r) {
	case U_WV_REASON_MINIMIZED: return XRT_WINDOW_VISIBILITY_REASON_MINIMIZED;
	case U_WV_REASON_NOT_VISIBLE: return XRT_WINDOW_VISIBILITY_REASON_WINDOW_HIDDEN;
	case U_WV_REASON_CLOAKED: return XRT_WINDOW_VISIBILITY_REASON_CLOAKED;
	case U_WV_REASON_LOCKED: return XRT_WINDOW_VISIBILITY_REASON_LOCKED;
	case U_WV_REASON_DISPLAY_OFF: return XRT_WINDOW_VISIBILITY_REASON_DISPLAY_OFF;
	case U_WV_REASON_COVERED: return XRT_WINDOW_VISIBILITY_REASON_COVERED;
	case U_WV_REASON_NOT_SHOWN: return XRT_WINDOW_VISIBILITY_REASON_NOT_SHOWN;
	default: return XRT_WINDOW_VISIBILITY_REASON_VISIBLE;
	}
}

void
u_wv_report_from_state(const struct u_window_visibility_state *s, bool foreground, struct u_wv_report *out)
{
	memset(out, 0, sizeof(*out));
	out->visible = true;
	out->reason = XRT_WINDOW_VISIBILITY_REASON_VISIBLE;
	if (s == NULL || !s->tracked) {
		return;
	}
	out->tracked = true;
	out->since_ns = s->since_ns;
	if (s->hidden_every_screen) {
		out->visible = false;
		// The debounce may hold "hidden" while the last raw check reads a
		// visible reason (a move or size in progress): the window was covered.
		out->reason = u_wv_reason_is_hidden(s->reason_every_screen) ? u_wv_report_reason(s->reason_every_screen)
		                                                            : XRT_WINDOW_VISIBILITY_REASON_COVERED;
	} else if (s->reason_every_screen == U_WV_REASON_NOT_SHOWN) {
		out->visible = false;
		out->reason = XRT_WINDOW_VISIBILITY_REASON_NOT_SHOWN;
	}
	out->foreground = !out->visible && foreground;
}

bool
u_wv_report_should_demote(bool opted_in, enum u_wv_demote_force force, const struct u_wv_report *r)
{
	if (r == NULL || !r->tracked || r->visible) {
		return false;
	}
	// Guard 1: never before the window has been shown once.
	if (r->reason == XRT_WINDOW_VISIBILITY_REASON_NOT_SHOWN || r->reason == XRT_WINDOW_VISIBILITY_REASON_VISIBLE) {
		return false;
	}
	// Guard 2: never while it is the OS foreground window (its input works).
	if (r->foreground) {
		return false;
	}
	if (opted_in) {
		return true;
	}
	switch (force) {
	case U_WV_DEMOTE_FORCE_ALL: return true;
	case U_WV_DEMOTE_FORCE_MINIMIZE: return r->reason == XRT_WINDOW_VISIBILITY_REASON_MINIMIZED;
	default: return false;
	}
}

enum u_wv_demote_force
u_wv_demote_force_parse(const char *value)
{
	if (value == NULL) {
		return U_WV_DEMOTE_FORCE_OFF;
	}
	if (wvr_stricmp(value, "minimize") == 0 || wvr_stricmp(value, "minimized") == 0 || strcmp(value, "1") == 0) {
		return U_WV_DEMOTE_FORCE_MINIMIZE;
	}
	if (wvr_stricmp(value, "all") == 0 || strcmp(value, "2") == 0) {
		return U_WV_DEMOTE_FORCE_ALL;
	}
	return U_WV_DEMOTE_FORCE_OFF;
}

const char *
u_wv_demote_force_str(enum u_wv_demote_force f)
{
	switch (f) {
	case U_WV_DEMOTE_FORCE_MINIMIZE: return "minimize";
	case U_WV_DEMOTE_FORCE_ALL: return "all";
	default: return "off";
	}
}

enum u_wv_demote_force
u_wv_demote_force_get(void)
{
	return u_wv_demote_force_parse(debug_get_option_hidden_demote_force());
}

void
u_wv_report_get(const struct u_window_visibility *wv, struct u_wv_report *out)
{
	struct u_window_visibility_state s;
	u_window_visibility_get_state(wv, &s);
	u_wv_report_from_state(&s, false, out);
	if (!out->visible) {
		out->foreground = u_window_visibility_foreground(wv);
	}
}
