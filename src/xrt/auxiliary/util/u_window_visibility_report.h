// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-062 D4 (tier 3, XR_DXR_window_visibility): what a session is
 *         told about its window, and whether it is demoted to SYNCHRONIZED.
 *
 * The tracker (@ref u_window_visibility.h) judges the window; this file turns
 * its snapshot into the session-facing verdict — visible or not, and why —
 * and holds the demotion rule with its two guards:
 * - never while the window is the OS foreground window (input keeps working
 *   for a covered window that still has focus);
 * - never before the window has been shown once (NOT_SHOWN: an app that
 *   renders its first frame into a hidden window must not deadlock).
 *
 * Everything here except @ref u_wv_report_get and @ref u_wv_demote_force_get
 * is pure and unit-tested (tests_aux_window_visibility_report.cpp).
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_session.h"
#include "util/u_window_visibility.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The demotion for sessions that did NOT opt in through XR_DXR_window_visibility
 * (a persisted setting, read once per process in the app's process):
 * `minimize` (default: a minimized window stops rendering) | `off` (the kill
 * switch: no session-state change from visibility) | `all` (any hidden reason;
 * a compatibility-test lever, not a feature). The two guards always apply. A
 * session that chains XrSessionWindowVisibilityInfoDXR with
 * demoteWhenHidden = XR_FALSE opts out explicitly and is never demoted.
 */
#define U_WINDOW_VISIBILITY_ENV_DEMOTE_FORCE "DXR_HIDDEN_DEMOTE_FORCE"

//! Default of @ref U_WINDOW_VISIBILITY_ENV_DEMOTE_FORCE (ADR-062 D4, approved 2026-11-15).
#define U_WINDOW_VISIBILITY_DEMOTE_FORCE_DEFAULT "minimize"

//! Values of @ref U_WINDOW_VISIBILITY_ENV_DEMOTE_FORCE.
enum u_wv_demote_force
{
	U_WV_DEMOTE_FORCE_OFF = 0,
	U_WV_DEMOTE_FORCE_MINIMIZE = 1,
	U_WV_DEMOTE_FORCE_ALL = 2,
};

//! The session-facing verdict for one window.
struct u_wv_report
{
	bool tracked;    //!< a live tracker answered (false: visible, nothing known)
	bool visible;    //!< false: hidden on every screen it covers, or not shown yet
	bool foreground; //!< the window is the OS foreground window (asked only when not visible)
	enum xrt_window_visibility_reason reason;
	uint64_t since_ns; //!< monotonic time of the tracker's last debounced transition
};

//! Map a tracker reason to the reason an app is told (non-hidden reasons: VISIBLE).
enum xrt_window_visibility_reason
u_wv_report_reason(enum u_wv_reason r);

/*!
 * The verdict from a tracker snapshot: hidden on every screen → not visible
 * with the every-screen reason (COVERED when the debounce holds a hidden verdict
 * the last raw check no longer explains); never shown → not visible, NOT_SHOWN;
 * otherwise visible. Pure.
 */
void
u_wv_report_from_state(const struct u_window_visibility_state *s, bool foreground, struct u_wv_report *out);

//! Same thing as the app hears it: visibility and reason (foreground excluded). Pure.
static inline bool
u_wv_report_same_for_app(const struct u_wv_report *a, const struct u_wv_report *b)
{
	return a->visible == b->visible && a->reason == b->reason;
}

/*!
 * Should the session be held at SYNCHRONIZED? Only for a tracked, not-visible
 * window that has been shown and is not the foreground window; then when the
 * session opted in, or the force lever covers this reason. Pure.
 */
bool
u_wv_report_should_demote(bool opted_in, enum u_wv_demote_force force, const struct u_wv_report *r);

//! Parse a lever value ("off", "minimize", "all"; also "0"/"1"/"2"). Unknown: off. Pure.
enum u_wv_demote_force
u_wv_demote_force_parse(const char *value);

//! Lower-case name of a force value.
const char *
u_wv_demote_force_str(enum u_wv_demote_force f);

//! The lever, read once per process through the settings chain.
enum u_wv_demote_force
u_wv_demote_force_get(void);

/*!
 * The verdict for @p wv now (NULL: untracked, visible). The foreground fact is
 * asked only when the window is not visible. Any thread; a short shared lock.
 */
void
u_wv_report_get(const struct u_window_visibility *wv, struct u_wv_report *out);


#ifdef __cplusplus
}
#endif
