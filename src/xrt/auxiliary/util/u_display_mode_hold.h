// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A runtime-side hardware-2D hold that restores the session's own
 *         2D/3D choice when it clears.
 * @ingroup aux_util
 *
 * ## Why this exists
 *
 * Some runtime paths must put the panel in hardware 2D for a reason that is
 * not the app's (the desktop-Linux refuse-rather-than-resample degrade, #1595).
 * When the reason clears, the panel must go back to **what the session last
 * asked for**, not to 3D: an app that chose 2D (`xrRequestDisplayModeDXR`, a
 * 2D rendering mode, the V key) must stay 2D.
 *
 * With some vendors that is the only thing that will ever restore it. Under
 * the Leia srSDK on Linux (LeiaSR #266) the first explicit lens call on an SR
 * context takes the lens preference away from the weaver for good, so a
 * runtime degrade that turns the lens off and relies on the weaver to turn it
 * back on leaves the panel flat forever. Every hardware-2D hold therefore pairs
 * with an explicit restore, and the restore is the session's choice.
 * docs/specs/vendor/lens-preference-ownership.md has the full contract and the
 * audit of every caller.
 *
 * ## Shape
 *
 * Two inputs, one output. The session's requests always update its choice and
 * are forwarded only while no hold is active; a hold's rising edge asks for 2D
 * and its falling edge asks for the recorded choice. Pure state: the caller
 * owns the DP call, the logging and the locking.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_display_mode_hold
{
	//! The session's own last hardware 2D/3D request (app, V key, zones).
	bool wanted_3d;
	//! A runtime reason is holding the panel in hardware 2D.
	bool held;
};

//! Start with no hold and the session wanting @p initial_3d.
static inline void
u_display_mode_hold_init(struct u_display_mode_hold *h, bool initial_3d)
{
	h->wanted_3d = initial_3d;
	h->held = false;
}

/*!
 * The session asked for hardware 2D or 3D. Always recorded; returns true when
 * it should be forwarded to the display processor now, false when a hold is
 * active (it is then applied when the hold clears).
 */
static inline bool
u_display_mode_hold_request(struct u_display_mode_hold *h, bool want_3d)
{
	h->wanted_3d = want_3d;
	return !h->held;
}

/*!
 * Set or clear the hold. Returns the hardware state to request from the
 * display processor for this edge: 2D while held, the session's recorded
 * choice once released. Call on transitions only.
 */
static inline bool
u_display_mode_hold_set(struct u_display_mode_hold *h, bool hold)
{
	h->held = hold;
	return hold ? false : h->wanted_3d;
}

#ifdef __cplusplus
}
#endif
