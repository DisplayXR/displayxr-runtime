// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Which display processor answers an eye query, for one calling client
 *         — pure, and therefore testable.
 * @ingroup comp_d3d11_service
 *
 * The rule this header states is the whole of #625, and #1414 asks for it to be
 * pinned: an IPC client that CREATED a session but never began one — the
 * inline-3D weave client, which stays frame-loop-free on purpose so the service
 * never presents over its window — must still have its eyes resolved from ITS
 * OWN display processor.
 *
 * That DP exists: init_client_render_resources gives every client compositor one
 * at session-CREATE time, bound to the client's HWND, whether or not the client
 * ever renders. What does NOT exist for such a client is `sys->active_compositor`
 * — that pointer is only assigned inside the per-frame render path. Resolving
 * through it alone (the pre-#625 shape) made the eye query fail for a submit-less
 * client, which made ipc_try_get_sr_view_poses bail, which made the whole rig
 * locate fall back to device_get_view_poses: `rig_applied == IPC_VIEW_RIG_NONE`,
 * a static symmetric frustum, the rig and any zone rect silently ignored.
 *
 * The decision is stated here — free of D3D, of the 25k-line service unit, and of
 * any need for a device, a window or a panel — so `tests/tests_comp_eye_dp_policy.cpp`
 * can assert it on a host with no GPU. comp_d3d11_service.cpp CALLS it; it does
 * not restate it.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Where the eyes for one query come from.
 *
 * Deliberately a SOURCE, not a pointer: the pointers live in the service unit
 * (and two of them are only safe to read under a lock), so the policy names the
 * slot and the caller dereferences it.
 */
enum comp_eye_dp_source
{
	//! No DP at all — the caller falls back to its default eye pair.
	COMP_EYE_DP_NONE = 0,
	//! The one panel DP owned by the multi-compositor.
	COMP_EYE_DP_PANEL,
	//! The calling client's own DP. Correct whether or not the client renders.
	COMP_EYE_DP_CLIENT,
	//! Last resort: whichever compositor rendered most recently.
	COMP_EYE_DP_ACTIVE,
};

/*!
 * Everything the choice depends on, read from the service system + the caller's
 * compositor. Nothing here is a graphics type, so a test can state a situation
 * instead of building one.
 */
struct comp_eye_dp_state
{
	//! #1002: the D3D device is gone; every DP on it is dead.
	bool device_removed;
	//! workspace_mode || pipeline_always_on(): clients own no DP, the panel does.
	bool panel_dp_owns_clients;
	//! A multi-compositor exists (its DP pointer may still be NULL).
	bool have_multi_comp;
	//! The multi-compositor exists AND holds a DP.
	bool have_panel_dp;
	//! The caller passed a client compositor and that client holds its own DP.
	bool have_client_dp;
};

/*!
 * Resolve @p s to the slot that answers the eye query.
 *
 * @ref COMP_EYE_DP_ACTIVE means "consult the last-rendered compositor" — the
 * caller reads that pointer under `active_compositor_mutex` and gets NULL when
 * nothing has rendered yet. It is precisely the slot a submit-less client must
 * NOT be routed to.
 */
static inline enum comp_eye_dp_source
comp_eye_dp_select(const struct comp_eye_dp_state *s)
{
	if (s == 0 || s->device_removed) {
		return COMP_EYE_DP_NONE;
	}

	// Workspace mode / #964 pipeline: per-client compositors have no DP of
	// their own — the multi-compositor owns the one panel DP (D-4). Taken on
	// have_multi_comp, not have_panel_dp: under this mode the panel DP is the
	// only correct answer, so a momentarily NULL one is still that answer and
	// must not silently degrade into another client's DP.
	if (s->panel_dp_owns_clients && s->have_multi_comp) {
		return COMP_EYE_DP_PANEL;
	}

	// #625 / #1414: the caller's own DP, ahead of every fallback. This is the
	// branch a created-but-never-begun client takes, and the reason its rig
	// locate reports rig_applied != IPC_VIEW_RIG_NONE.
	if (s->have_client_dp) {
		return COMP_EYE_DP_CLIENT;
	}

	// Shared DP for a client that has none of its own (weave_submit's fallback).
	if (s->have_panel_dp) {
		return COMP_EYE_DP_PANEL;
	}

	return COMP_EYE_DP_ACTIVE;
}

#ifdef __cplusplus
}
#endif
