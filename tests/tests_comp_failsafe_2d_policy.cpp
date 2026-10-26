// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief #814 / #2416 — the hardware-2D teardown failsafe's decision.
 *
 * WHAT IS PINNED
 * --------------
 * comp_d3d11_service.cpp asks comp_failsafe_2d_decide() whether the lens may
 * go flat at two points: when a client compositor is torn down (#814), and —
 * #2416 — once more after the workspace controller's IPC connection was lost
 * and the workspace deactivated. This suite compiles the same header the
 * service unit calls; there is no second copy of the rule.
 *
 * #2416 was an ORDERING bug, so the regression cases replay the two exit
 * orderings as the service runs them:
 *
 *   orderly   — workspace_deactivate (IPC) → controller compositor teardown
 *               → failsafe sees workspace_mode == false → 2D.
 *   unclean   — controller compositor teardown FIRST (workspace still active,
 *               failsafe gated off) → common_shutdown deactivates the workspace
 *               → (#2416) controller-lost pass → 2D.
 *
 * The end-to-end shape (a real controller process force-killed under a running
 * displayxr-service on a 3D panel) needs a device, a window, a panel DP and a
 * spawned workspace controller; it is a hardware check, not a ctest.
 */

#include "catch_amalgamated.hpp"

#include "comp_failsafe_2d_policy.h"

namespace {

/*!
 * The service's view of the box, reduced to the failsafe's inputs, plus the
 * one fact the ordering bug hinges on: whether the workspace is active.
 */
struct box
{
	bool workspace_mode = true;     // the controller activated it (Ctrl+Space)
	bool hardware_display_3d = true; // ... and the lens went 3D
	bool bridge_relay_live = false;
	uint32_t apps = 0; // renderable clients other than the controller
};

//! The #814 teardown pass for the controller's own compositor (c = controller).
comp_failsafe_2d_verdict
teardown_controller(const box &b)
{
	comp_failsafe_2d_state s{};
	s.have_panel_dp = true;
	s.workspace_mode = b.workspace_mode;
	s.bridge_relay_live = b.bridge_relay_live;
	s.hardware_display_3d = b.hardware_display_3d;
	s.departing_is_weave_owner = false; // a controller is not a weave present-owner
	s.survivors = b.apps;               // the controller has been unregistered
	return comp_failsafe_2d_decide(&s);
}

//! The #2416 controller-lost pass (c = NULL), run after the deactivate.
comp_failsafe_2d_verdict
controller_lost(const box &b)
{
	comp_failsafe_2d_state s{};
	s.have_panel_dp = true;
	s.workspace_mode = b.workspace_mode;
	s.bridge_relay_live = b.bridge_relay_live;
	s.hardware_display_3d = b.hardware_display_3d;
	s.departing_is_weave_owner = false; // no departing client at all
	s.survivors = b.apps;
	return comp_failsafe_2d_decide(&s);
}

} // namespace

TEST_CASE("failsafe_2d: unclean controller death requests hardware 2D (#2416)")
{
	box b; // controller up, workspace active, lens 3D, zero apps

	// 1. ReadFile fails → compositor_destroy runs FIRST, workspace still active.
	CHECK(teardown_controller(b) == COMP_FAILSAFE_2D_SKIP_MODE_OWNED);

	// 2. common_shutdown: "Workspace controller disconnected — deactivating".
	b.workspace_mode = false;

	// 3. #2416: the controller-lost pass. Before the fix nothing ran here and
	//    the lens stayed 3D with zero clients.
	CHECK(controller_lost(b) == COMP_FAILSAFE_2D_REQUEST_2D);
}

TEST_CASE("failsafe_2d: orderly controller exit is unchanged (#814)")
{
	box b;

	// 1. workspace_deactivate over IPC — before any teardown.
	b.workspace_mode = false;

	// 2. The controller's compositor is torn down; the failsafe flattens.
	CHECK(teardown_controller(b) == COMP_FAILSAFE_2D_REQUEST_2D);
	// common_shutdown's controller branch is not taken (the server already
	// cleared workspace_mode), so the controller-lost pass never runs here.
}

TEST_CASE("failsafe_2d: the unclean exit lands where the orderly one does")
{
	// Whatever the survivors, the controller-lost pass after the deactivate
	// must give the orderly teardown's verdict — that is the whole fix.
	for (uint32_t apps : {0u, 1u, 3u}) {
		for (bool hw3d : {false, true}) {
			box b;
			b.apps = apps;
			b.hardware_display_3d = hw3d;
			b.workspace_mode = false;
			INFO("apps=" << apps << " hw3d=" << hw3d);
			CHECK(controller_lost(b) == teardown_controller(b));
		}
	}
}

TEST_CASE("failsafe_2d: controller lost with surviving apps leaves the lens to them")
{
	box b;
	b.apps = 2;
	b.workspace_mode = false;
	CHECK(controller_lost(b) == COMP_FAILSAFE_2D_SKIP_SURVIVORS_REMAIN);
}

TEST_CASE("failsafe_2d: a live bridge relay owns the mode — no controller-loss 2D")
{
	box b;
	b.workspace_mode = false;
	b.bridge_relay_live = true;
	CHECK(controller_lost(b) == COMP_FAILSAFE_2D_SKIP_MODE_OWNED);
}

TEST_CASE("failsafe_2d: a re-activated workspace owns the mode again")
{
	box b; // workspace_mode still / again true (a new controller already up)
	CHECK(controller_lost(b) == COMP_FAILSAFE_2D_SKIP_MODE_OWNED);
}

TEST_CASE("failsafe_2d: already flat is a no-op")
{
	box b;
	b.workspace_mode = false;
	b.hardware_display_3d = false;
	CHECK(controller_lost(b) == COMP_FAILSAFE_2D_SKIP_ALREADY_FLAT);
}

TEST_CASE("failsafe_2d: survivor rules (#814 / #939)")
{
	comp_failsafe_2d_state s{};
	s.have_panel_dp = true;
	s.hardware_display_3d = true;

	SECTION("no panel DP — nothing to ask")
	{
		s.have_panel_dp = false;
		CHECK(comp_failsafe_2d_decide(&s) == COMP_FAILSAFE_2D_SKIP_NO_DP);
	}
	SECTION("last client leaving flattens")
	{
		CHECK(comp_failsafe_2d_decide(&s) == COMP_FAILSAFE_2D_REQUEST_2D);
	}
	SECTION("a surviving weave present-owner keeps the lens")
	{
		s.weave_owner_survives = true;
		s.survivors = 1;
		CHECK(comp_failsafe_2d_decide(&s) == COMP_FAILSAFE_2D_SKIP_WEAVE_OWNER_SURVIVES);
	}
	SECTION("#939: a presenting survivor that wants 3D keeps the lens, even after a weave owner leaves")
	{
		s.departing_is_weave_owner = true;
		s.presenting_survivor_wants_3d = true;
		s.survivors = 1;
		CHECK(comp_failsafe_2d_decide(&s) == COMP_FAILSAFE_2D_SKIP_SURVIVOR_WANTS_3D);
	}
	SECTION("a departing weave owner flattens despite 2D survivors")
	{
		s.departing_is_weave_owner = true;
		s.survivors = 2;
		CHECK(comp_failsafe_2d_decide(&s) == COMP_FAILSAFE_2D_REQUEST_2D);
	}
	SECTION("an ordinary client leaving beside others does not")
	{
		s.survivors = 2;
		CHECK(comp_failsafe_2d_decide(&s) == COMP_FAILSAFE_2D_SKIP_SURVIVORS_REMAIN);
	}
	SECTION("the gate reports PROCEED only when the scan must run")
	{
		CHECK(comp_failsafe_2d_gate(&s) == COMP_FAILSAFE_2D_PROCEED);
		CHECK(comp_failsafe_2d_gate(nullptr) == COMP_FAILSAFE_2D_SKIP_NO_DP);
	}
}
