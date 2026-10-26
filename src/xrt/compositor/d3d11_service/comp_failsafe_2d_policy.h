// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief #814 / #2416 — the hardware-2D teardown failsafe's decision, as a pure
 *        function, so ctest can pin it without a device, a DP or a service.
 *
 * The service asks "should the lens go flat now?" at two points:
 *
 *  - a client compositor is torn down (#814 — the dying client is excluded
 *    from the survivor scan);
 *  - the workspace controller's IPC connection was lost and the workspace has
 *    just been deactivated (#2416 — there is no dying client left: the
 *    controller's compositor was torn down earlier, while the workspace was
 *    still active, and that pass was gated off because a live controller owns
 *    the display mode).
 *
 * Both run the SAME rule below; comp_d3d11_service.cpp fills the state from the
 * system / slot table and acts on the verdict. Pure C, no allocation.
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum comp_failsafe_2d_verdict
{
	//! Continue to the survivor rules (only returned by comp_failsafe_2d_gate).
	COMP_FAILSAFE_2D_PROCEED = 0,
	//! No panel DP to ask (device removed, legacy path with no client DP).
	COMP_FAILSAFE_2D_SKIP_NO_DP,
	//! A live workspace controller or an authoritative bridge relay owns the mode.
	COMP_FAILSAFE_2D_SKIP_MODE_OWNED,
	//! The panel is already in hardware 2D — nothing stranded.
	COMP_FAILSAFE_2D_SKIP_ALREADY_FLAT,
	//! A weave present-owner survives; it re-asserts 3D merely by existing.
	COMP_FAILSAFE_2D_SKIP_WEAVE_OWNER_SURVIVES,
	//! #939: a presenting survivor's standing wish is 3D.
	COMP_FAILSAFE_2D_SKIP_SURVIVOR_WANTS_3D,
	//! An ordinary client left while others remain; one of them may own the 3D.
	COMP_FAILSAFE_2D_SKIP_SURVIVORS_REMAIN,
	//! Ask the panel for hardware 2D (ADR-028 hardware-only channel).
	COMP_FAILSAFE_2D_REQUEST_2D,
};

struct comp_failsafe_2d_state
{
	/* Gate inputs (cheap; read before any survivor scan). */

	//! The panel DP resolves (panel_dp() != NULL).
	bool have_panel_dp;
	//! sys->workspace_mode at the time of the call.
	bool workspace_mode;
	//! bridge_relay_is_live_authoritative().
	bool bridge_relay_live;
	//! sys->hardware_display_3d.
	bool hardware_display_3d;

	/* Survivor inputs (the #814/#939 scan, dying client excluded). */

	//! The departing client is a weave present-owner. False for the #2416
	//! controller-loss call, which has no departing client.
	bool departing_is_weave_owner;
	//! At least one surviving client is a weave present-owner.
	bool weave_owner_survives;
	//! #939: a presenting slot survivor wants hardware 3D.
	bool presenting_survivor_wants_3d;
	//! Number of surviving renderable clients.
	uint32_t survivors;
};

/*!
 * The cheap gates, in the order the service evaluates them. Returns
 * COMP_FAILSAFE_2D_PROCEED when the survivor scan has to run.
 */
static inline enum comp_failsafe_2d_verdict
comp_failsafe_2d_gate(const struct comp_failsafe_2d_state *s)
{
	if (s == 0 || !s->have_panel_dp) {
		return COMP_FAILSAFE_2D_SKIP_NO_DP;
	}
	// Workspace: a live controller owns the display mode (a client leaving is
	// routine there). Bridge: the relay owns it via the HWND path.
	if (s->workspace_mode || s->bridge_relay_live) {
		return COMP_FAILSAFE_2D_SKIP_MODE_OWNED;
	}
	if (!s->hardware_display_3d) {
		return COMP_FAILSAFE_2D_SKIP_ALREADY_FLAT;
	}
	return COMP_FAILSAFE_2D_PROCEED;
}

/*!
 * The full verdict: the gates, then the survivor rules.
 */
static inline enum comp_failsafe_2d_verdict
comp_failsafe_2d_decide(const struct comp_failsafe_2d_state *s)
{
	enum comp_failsafe_2d_verdict g = comp_failsafe_2d_gate(s);
	if (g != COMP_FAILSAFE_2D_PROCEED) {
		return g;
	}
	if (s->weave_owner_survives) {
		return COMP_FAILSAFE_2D_SKIP_WEAVE_OWNER_SURVIVES;
	}
	if (s->presenting_survivor_wants_3d) {
		return COMP_FAILSAFE_2D_SKIP_SURVIVOR_WANTS_3D;
	}
	// Only the departure of the weave present-owner itself, or of the last
	// client on the box, releases the lens. The controller-loss call has no
	// departing client, so it releases exactly when the orderly exit would.
	if (!s->departing_is_weave_owner && s->survivors > 0) {
		return COMP_FAILSAFE_2D_SKIP_SURVIVORS_REMAIN;
	}
	return COMP_FAILSAFE_2D_REQUEST_2D;
}

#ifdef __cplusplus
}
#endif
