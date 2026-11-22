// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Window-drag phase snap for an IPC handle client: when may the D3D11
 *        service snap the client's window?
 *
 * WHAT IS PINNED
 * --------------
 * comp_d3d11_service_app_snap_window_rect() asks comp_app_snap_decide() whether
 * the panel display processor weaves this client's window into this client's
 * window right now; only then does it ask the DP for a snap. Every other case
 * must answer "not snapped" so the window moves freely, which is the behaviour
 * before the call existed. This suite compiles the same header the service unit
 * calls; there is no second copy of the rule.
 *
 * The end-to-end shape (a forced-IPC handle app dragged across a 3D panel while
 * the service weaves it) needs a device, a window, a panel DP and a running
 * service; it is a hardware check, not a ctest.
 */

#include "catch_amalgamated.hpp"

#include <string>

#include "comp_app_snap_policy.h"

namespace {

//! A forced-IPC handle app alone on the box: focused, presented into its own
//! window, the panel DP bound to that window.
comp_app_snap_state
presented_handle_app()
{
	comp_app_snap_state s{};
	s.pipeline_always_on = true;
	s.presenter_is_app_hwnd = true;
	s.workspace_mode = false;
	s.client_slot = 2;
	s.focused_slot = 2;
	s.foreground_override_slot = -1;
	s.window_gone = false;
	s.have_panel_dp = true;
	s.panel_dp_on_client_window = true;
	return s;
}

} // namespace

TEST_CASE("app_snap: the presented handle client asks the DP")
{
	comp_app_snap_state s = presented_handle_app();
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_ASK_DP);
}

TEST_CASE("app_snap: null state and the legacy standalone path never snap")
{
	CHECK(comp_app_snap_decide(nullptr) == COMP_APP_SNAP_NO_PIPELINE);
	comp_app_snap_state s = presented_handle_app();
	s.pipeline_always_on = false;
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_NO_PIPELINE);
}

TEST_CASE("app_snap: a client the service does not present into its own window is not snapped")
{
	// Hosted (service window), client texture, present-owner, none.
	comp_app_snap_state s = presented_handle_app();
	s.presenter_is_app_hwnd = false;
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_NOT_APP_HWND);
}

TEST_CASE("app_snap: an unfocused client is not snapped")
{
	comp_app_snap_state s = presented_handle_app();
	s.focused_slot = 0; // another app holds the panel
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_NOT_PRESENTED);

	s.focused_slot = -1; // nobody does
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_NOT_PRESENTED);

	s = presented_handle_app();
	s.client_slot = -1; // not registered in the slot table
	s.focused_slot = -1;
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_NOT_PRESENTED);
}

TEST_CASE("app_snap: under a workspace controller only a foreground override snaps")
{
	comp_app_snap_state s = presented_handle_app();
	s.workspace_mode = true;
	// The controller's focus is this client, but the panel is composed: the
	// client's window is not what the DP weaves into.
	s.focused_slot = 2;
	s.foreground_override_slot = -1;
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_NOT_PRESENTED);

	// The user Alt-Tabbed to this client's own window: the direct path runs
	// for it and the DP is re-bound to its window.
	s.foreground_override_slot = 2;
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_ASK_DP);

	// ... to another client's window.
	s.foreground_override_slot = 1;
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_NOT_PRESENTED);
}

TEST_CASE("app_snap: an override that just ended does not snap while the DP is still on the window")
{
	// The restore re-bind can defer a frame or two; the compose path already
	// runs, so the slot test refuses even though the DP still names the window.
	comp_app_snap_state s = presented_handle_app();
	s.workspace_mode = true;
	s.foreground_override_slot = -1;
	s.panel_dp_on_client_window = true;
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_NOT_PRESENTED);
}

TEST_CASE("app_snap: a closed or minimized window is not snapped")
{
	comp_app_snap_state s = presented_handle_app();
	s.window_gone = true;
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_WINDOW_GONE);
}

TEST_CASE("app_snap: no panel DP (device removed, no plug-in, mid-rebind) is not snapped")
{
	comp_app_snap_state s = presented_handle_app();
	s.have_panel_dp = false;
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_NO_DP);
}

TEST_CASE("app_snap: a panel DP bound to another window is not asked")
{
	comp_app_snap_state s = presented_handle_app();
	s.panel_dp_on_client_window = false;
	CHECK(comp_app_snap_decide(&s) == COMP_APP_SNAP_DP_ELSEWHERE);
}

TEST_CASE("app_snap: every verdict has a log name")
{
	for (int v = COMP_APP_SNAP_ASK_DP; v <= COMP_APP_SNAP_DP_ELSEWHERE; v++) {
		CHECK(std::string(comp_app_snap_verdict_str((comp_app_snap_verdict)v)) != "?");
	}
}
