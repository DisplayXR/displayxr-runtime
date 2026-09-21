// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief #1414 regression assert: a submit-less IPC client's rig locate must
 *        report rig_applied != IPC_VIEW_RIG_NONE.
 *
 * WHAT IS ACTUALLY PINNED, AND WHY THIS SHAPE
 * -------------------------------------------
 * The whole of `rig_applied` on the IPC route hangs off ONE decision. The rig
 * reply is written inside ipc_try_get_sr_view_poses (ipc_server_handler.c), and
 * that function returns EARLY — before any rig field is touched — when its eye
 * query fails:
 *
 *     if (!comp_d3d11_service_get_predicted_eye_positions_for_client(s->xsysc, xc, ...))
 *             return false;                    // -> device_get_view_poses,
 *                                              //    rig_applied == IPC_VIEW_RIG_NONE
 *
 * The query fails exactly when no display processor can be resolved FOR THIS
 * CLIENT, and that resolution is comp_eye_dp_select() (called by
 * resolve_eye_display_processor in comp_d3d11_service.cpp). So the assert below
 * is the regression assert the issue asks for, one call frame down from the
 * `VIEW-RIG IPC client: locate ok, rig_applied=…` line: route a submit-less
 * client to anything but its own DP and rig_applied goes to 0.
 *
 * It is not a mock: this suite compiles the SAME comp_eye_dp_select() the
 * service unit calls, from the same header, with no second copy of the rule.
 *
 * The end-to-end shape — a real IPC client chaining XrDisplayRigDXR against a
 * running displayxr-service — is NOT reachable in ctest. It needs the
 * XRT_FEATURE_SERVICE build AND a live D3D11 service compositor (the eye path is
 * gated on comp_d3d11_service_is_d3d11_service(), so the null / headless
 * compositor answers rig_applied == 0 by construction), which means a device, a
 * window, a panel and a registered vendor DP. tests_oxr_view_space.cpp says the
 * same thing in its header — "the IPC route ... needs a service / an HWND" — and
 * leaves it to the hardware eyeball list. This is the most faithful headless
 * shape available: the real rule, the real caller, no GPU.
 *
 * The situation each case states is a CLIENT LIFECYCLE, not a plumbing detail:
 * every client compositor is handed its own DP in init_client_render_resources
 * at session-CREATE time (bound to the client's HWND), while
 * `sys->active_compositor` is only assigned inside the per-frame render path.
 * "Created but never begun" is therefore precisely
 * `have_client_dp && !rendered_yet` — the browser's weave session, and the shape
 * that regressed in #625.
 */

#include "catch_amalgamated.hpp"

#include "comp_eye_dp_policy.h"

#include "shared/ipc_protocol.h" // IPC_VIEW_RIG_NONE

namespace {

//! A client that has created a session (so it owns a DP) and never begun one.
constexpr struct comp_eye_dp_state
submit_less_client()
{
	return comp_eye_dp_state{
	    /* device_removed */ false,
	    /* panel_dp_owns_clients */ false,
	    /* have_multi_comp */ false,
	    /* have_panel_dp */ false,
	    /* have_client_dp */ true,
	};
}

} // namespace

TEST_CASE("eye_dp_policy: submit-less client resolves to its OWN dp (#1414)")
{
	// The regression assert. A created-but-never-begun client has a DP of its
	// own and is NOT the active compositor (nothing of its has ever rendered).
	// Routing it to the active slot returns NULL there, the eye query fails,
	// and the rig locate silently degrades to device defaults.
	struct comp_eye_dp_state st = submit_less_client();

	const enum comp_eye_dp_source src = comp_eye_dp_select(&st);

	INFO("a submit-less client routed to " << (int)src << " gets rig_applied == IPC_VIEW_RIG_NONE");
	CHECK(src == COMP_EYE_DP_CLIENT);
	// Said the other way round, in the terms the issue uses: anything that
	// depends on the client having RENDERED is a rig_applied == 0 regression.
	CHECK(src != COMP_EYE_DP_ACTIVE);
	CHECK(src != COMP_EYE_DP_NONE);

	// "rig_applied != 0" is only a meaningful test while NONE is the zero.
	STATIC_REQUIRE(IPC_VIEW_RIG_NONE == 0);
}

TEST_CASE("eye_dp_policy: the client's own dp wins over a rendering peer's")
{
	// A second client IS rendering, so an active compositor exists and a panel
	// DP may too. Neither may answer for this client: its eyes come from its
	// own DP, bound to its own window.
	struct comp_eye_dp_state st = submit_less_client();
	st.have_multi_comp = true;
	st.have_panel_dp = true;

	CHECK(comp_eye_dp_select(&st) == COMP_EYE_DP_CLIENT);
}

TEST_CASE("eye_dp_policy: a client with no dp of its own falls back")
{
	struct comp_eye_dp_state st = submit_less_client();
	st.have_client_dp = false;

	SECTION("shared panel dp when the multi-compositor holds one")
	{
		st.have_multi_comp = true;
		st.have_panel_dp = true;
		CHECK(comp_eye_dp_select(&st) == COMP_EYE_DP_PANEL);
	}

	SECTION("last-rendered compositor otherwise (headless relay, xc == NULL)")
	{
		CHECK(comp_eye_dp_select(&st) == COMP_EYE_DP_ACTIVE);
	}
}

TEST_CASE("eye_dp_policy: workspace / always-on pipeline routes to the panel dp")
{
	// D-4: under a workspace controller (and under the #964 always-on pipeline)
	// no client owns a DP — the multi-compositor owns the one panel DP. It is
	// the answer even while its pointer is momentarily NULL, so a stale
	// per-client DP can never be substituted for it.
	struct comp_eye_dp_state st = submit_less_client();
	st.panel_dp_owns_clients = true;
	st.have_multi_comp = true;

	SECTION("panel dp present")
	{
		st.have_panel_dp = true;
		CHECK(comp_eye_dp_select(&st) == COMP_EYE_DP_PANEL);
	}

	SECTION("panel dp not yet created — still the panel slot, never the client")
	{
		st.have_panel_dp = false;
		CHECK(comp_eye_dp_select(&st) == COMP_EYE_DP_PANEL);
	}

	SECTION("no multi-compositor at all — the normal chain applies")
	{
		st.have_multi_comp = false;
		CHECK(comp_eye_dp_select(&st) == COMP_EYE_DP_CLIENT);
	}
}

TEST_CASE("eye_dp_policy: a removed device hands out no dp (#1002)")
{
	// Callers fall back to their default eye pair rather than faulting inside
	// the vendor weaver — the one case where rig_applied == 0 is correct.
	struct comp_eye_dp_state st = submit_less_client();
	st.device_removed = true;
	st.have_multi_comp = true;
	st.have_panel_dp = true;

	CHECK(comp_eye_dp_select(&st) == COMP_EYE_DP_NONE);
}

TEST_CASE("eye_dp_policy: a NULL state is not a dp")
{
	CHECK(comp_eye_dp_select(nullptr) == COMP_EYE_DP_NONE);
}
