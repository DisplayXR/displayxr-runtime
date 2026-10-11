// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Session-level "is anyone tracked": the OR over every display
 *         processor that presents for a session, debounced on hand-over.
 * @ingroup aux_util
 *
 * ## Why this exists (ADR-047 Amendment 7)
 *
 * On a multi-screen rig each screen's display processor (DP) has its own
 * tracker and reports its own eye validity, but the SESSION-level tracking
 * state (`XrViewEyeTrackingStateDXR.isTracking`, the
 * `XrEventDataEyeTrackingStateChangedDXR` edge, the `XR_DXR_weave` output's
 * `eyesTracking`, the service's display status) used to come from the
 * primary screen's DP only — "it feels like only the primary camera wakes the
 * tracker" (David, two-panel rig, 2026-10-10). A MANUAL app that drops to 2D
 * on `isTracking == false` stayed 2D while the viewer sat in front of the
 * second screen its window was woven on.
 *
 * The session's state is now the OR over the DPs that currently present for
 * it: the session's own DP plus every live segment / workspace-presenter /
 * present-owner weave-segment DP. Eyes stay per screen (each segment keeps
 * its own); only the boolean and what hangs off it change.
 *
 * ## Debounce
 *
 * Each DP's signal is already the vendor's (MANAGED: grace period; MANUAL:
 * immediate, ADR / eye-tracking-modes.md). The OR adds exactly one new way to
 * flap: a viewer moving from screen A to screen B, where A lets go a moment
 * before B acquires. So the combinator:
 *
 * - **rises immediately** (any DP tracked -> tracked, as today);
 * - with **no other DP presenting** (`other_count == 0`, a window on one
 *   screen, every window on a single-screen box) **falls immediately**:
 *   byte for byte the pre-amendment pass-through, MANUAL's "immediate" intact;
 * - with other DPs presenting, **falls only after the OR has been false for
 *   @p hold_ns** (@ref U_TRACKING_ANY_HOLD_NS), so a hand-over between screens
 *   never produces a false edge, and alternating per-screen states that
 *   overlap or leave gaps shorter than the hold never flap the session.
 *
 * Pure state, no clock, no locks: the caller passes `now_ns` and serialises
 * calls (one per session/client thread). Header-only so the C state tracker,
 * the C IPC server and the C++ service all share the one rule.
 *
 * ## HOOK: per-display user presence (SR_EVENT_TYPE_DISPLAY_USER_PRESENCE)
 *
 * @ref u_tracking_dp_viewer_present is the ONE place the runtime turns a DP's
 * answer into "a viewer is present at this screen". Today it is the DP's eye
 * validity (`valid && is_tracking`). LeiaSR is landing a per-display presence
 * event (`SR_EVENT_TYPE_DISPLAY_USER_PRESENCE = 20`: display id +
 * present/absent, capability-gated). When it ships, the plan is an
 * append-only optional DP slot (ADR-020 rule; e.g. `get_user_presence(dp,
 * *present)` returning false when the capability is absent) that the plug-in
 * fills from that event, and this function grows a `presence` input that wins
 * over eye validity when the DP answered. No SR SDK dependency here — the
 * runtime only ever sees the vendor-neutral boolean.
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_display_metrics.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! How long the OR must stay false before a multi-DP session reports
//! untracked (the screen hand-over window). ~0.5 s, the same order as the
//! FRAME_TICK viewer's loss threshold and Amendment 2's owner hysteresis.
#define U_TRACKING_ANY_HOLD_NS ((uint64_t)500 * 1000 * 1000)

/*!
 * Combinator state. Zero-init (or @ref u_tracking_any_reset) = untracked,
 * nothing pending.
 */
struct u_tracking_any
{
	bool state;             //!< The debounced session-level answer.
	bool falling;           //!< The OR went false while @ref state was true; waiting out the hold.
	uint64_t fall_since_ns; //!< When @ref falling started.
};

static inline void
u_tracking_any_reset(struct u_tracking_any *t)
{
	t->state = false;
	t->falling = false;
	t->fall_since_ns = 0;
}

/*!
 * HOOK (see file comment): does this DP's answer mean "a viewer is present at
 * its screen"? Today: its eye validity. The SR per-display presence event
 * (SR_EVENT_TYPE_DISPLAY_USER_PRESENCE = 20) replaces this per DP once a
 * capability-gated DP slot carries it.
 */
static inline bool
u_tracking_dp_viewer_present(const struct xrt_eye_positions *eyes)
{
	return eyes != NULL && eyes->valid && eyes->is_tracking;
}

/*!
 * Feed one sample; returns the session-level tracking state.
 *
 * @param t            Combinator state (one per session / client).
 * @param now_ns       Monotonic time of this sample.
 * @param own          The session's own (primary / majority) DP reports a viewer.
 * @param others       Any OTHER DP presenting for the session reports a viewer.
 * @param other_count  How many other DPs presented and answered this sample
 *                     (0 = the session presents through one DP: pass-through).
 * @param hold_ns      Falling-edge hold for the multi-DP case
 *                     (@ref U_TRACKING_ANY_HOLD_NS).
 */
static inline bool
u_tracking_any_update(
    struct u_tracking_any *t, uint64_t now_ns, bool own, bool others, uint32_t other_count, uint64_t hold_ns)
{
	const bool raw = own || (other_count > 0 && others);
	if (raw) {
		t->state = true;
		t->falling = false;
		return true;
	}
	if (other_count == 0 || !t->state) {
		// One DP presents (the single-screen rule, unchanged), or nothing
		// to hold: follow the OR exactly.
		t->state = false;
		t->falling = false;
		return false;
	}
	if (!t->falling) {
		t->falling = true;
		t->fall_since_ns = now_ns;
	}
	if (now_ns - t->fall_since_ns >= hold_ns) {
		t->state = false;
		t->falling = false;
	}
	return t->state;
}

#ifdef __cplusplus
}
#endif
