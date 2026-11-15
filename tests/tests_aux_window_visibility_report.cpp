// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-062 D4 (XR_DXR_window_visibility): the session-facing verdict
 *         and the demotion rule with its two guards (foreground, NOT_SHOWN).
 */

#include "catch_amalgamated.hpp"

#include "util/u_window_visibility_report.h"

#include <cstring>

namespace {

u_window_visibility_state
State(bool hidden_every_screen, u_wv_reason reason_every_screen, bool hidden_3d = false)
{
	u_window_visibility_state s;
	std::memset(&s, 0, sizeof(s));
	s.tracked = true;
	s.hidden = hidden_3d;
	s.hidden_every_screen = hidden_every_screen;
	s.reason_every_screen = reason_every_screen;
	s.since_ns = 42;
	return s;
}

u_wv_report
Report(const u_window_visibility_state &s, bool foreground = false)
{
	u_wv_report r;
	u_wv_report_from_state(&s, foreground, &r);
	return r;
}

} // namespace

TEST_CASE("window_visibility_report: an untracked window reads visible", "[window_visibility]")
{
	u_window_visibility_state s;
	std::memset(&s, 0, sizeof(s));
	u_wv_report r = Report(s, true);
	CHECK_FALSE(r.tracked);
	CHECK(r.visible);
	CHECK(r.reason == XRT_WINDOW_VISIBILITY_REASON_VISIBLE);
	CHECK_FALSE(r.foreground);
	CHECK_FALSE(u_wv_report_should_demote(true, U_WV_DEMOTE_FORCE_ALL, &r));
	u_wv_report_from_state(nullptr, false, &r);
	CHECK(r.visible);
}

TEST_CASE("window_visibility_report: hidden reasons map one to one", "[window_visibility]")
{
	struct
	{
		u_wv_reason in;
		xrt_window_visibility_reason out;
	} cases[] = {
	    {U_WV_REASON_MINIMIZED, XRT_WINDOW_VISIBILITY_REASON_MINIMIZED},
	    {U_WV_REASON_NOT_VISIBLE, XRT_WINDOW_VISIBILITY_REASON_WINDOW_HIDDEN},
	    {U_WV_REASON_CLOAKED, XRT_WINDOW_VISIBILITY_REASON_CLOAKED},
	    {U_WV_REASON_LOCKED, XRT_WINDOW_VISIBILITY_REASON_LOCKED},
	    {U_WV_REASON_DISPLAY_OFF, XRT_WINDOW_VISIBILITY_REASON_DISPLAY_OFF},
	    {U_WV_REASON_COVERED, XRT_WINDOW_VISIBILITY_REASON_COVERED},
	};
	for (const auto &c : cases) {
		u_wv_report r = Report(State(true, c.in));
		CHECK_FALSE(r.visible);
		CHECK(r.reason == c.out);
		CHECK(r.since_ns == 42);
	}
	// Visible reasons all read VISIBLE.
	for (u_wv_reason v : {U_WV_REASON_VISIBLE, U_WV_REASON_PARTLY_COVERED, U_WV_REASON_OFF_SCREEN, U_WV_REASON_GONE}) {
		CHECK(u_wv_report_reason(v) == XRT_WINDOW_VISIBILITY_REASON_VISIBLE);
		u_wv_report r = Report(State(false, v));
		CHECK(r.visible);
		CHECK(r.reason == XRT_WINDOW_VISIBILITY_REASON_VISIBLE);
	}
}

TEST_CASE("window_visibility_report: a held hidden verdict with a visible raw reason reads covered",
          "[window_visibility]")
{
	// The debounce holds "hidden" while a window moves; the last raw check
	// already reads partly covered.
	u_wv_report r = Report(State(true, U_WV_REASON_PARTLY_COVERED));
	CHECK_FALSE(r.visible);
	CHECK(r.reason == XRT_WINDOW_VISIBILITY_REASON_COVERED);
}

TEST_CASE("window_visibility_report: edge aggregation is over every screen", "[window_visibility]")
{
	// Hidden on the 3D screen only (the window's part on a 2D screen is seen):
	// tier 1 drops the lens, but the app is told nothing and is never demoted.
	u_wv_report part = Report(State(false, U_WV_REASON_PARTLY_COVERED, /*hidden_3d=*/true));
	CHECK(part.visible);
	CHECK_FALSE(u_wv_report_should_demote(true, U_WV_DEMOTE_FORCE_ALL, &part));

	// Hidden on every screen: told, demoted.
	u_wv_report all = Report(State(true, U_WV_REASON_COVERED, /*hidden_3d=*/true));
	CHECK_FALSE(all.visible);
	CHECK(u_wv_report_should_demote(true, U_WV_DEMOTE_FORCE_OFF, &all));

	// An edge is a change of what the app hears; a foreground change is not.
	u_wv_report all_fg = Report(State(true, U_WV_REASON_COVERED), true);
	CHECK(u_wv_report_same_for_app(&all, &all_fg));
	CHECK_FALSE(u_wv_report_same_for_app(&part, &all));
	// A reason change while hidden is an edge too.
	u_wv_report mini = Report(State(true, U_WV_REASON_MINIMIZED));
	CHECK_FALSE(u_wv_report_same_for_app(&all, &mini));
	// Two visible verdicts are the same whatever the raw reason.
	u_wv_report vis = Report(State(false, U_WV_REASON_VISIBLE));
	CHECK(u_wv_report_same_for_app(&part, &vis));
}

TEST_CASE("window_visibility_report: guard 1 - never demoted before the window is shown", "[window_visibility]")
{
	u_wv_report r = Report(State(false, U_WV_REASON_NOT_SHOWN));
	CHECK_FALSE(r.visible);
	CHECK(r.reason == XRT_WINDOW_VISIBILITY_REASON_NOT_SHOWN);
	CHECK_FALSE(u_wv_report_should_demote(true, U_WV_DEMOTE_FORCE_OFF, &r));
	CHECK_FALSE(u_wv_report_should_demote(false, U_WV_DEMOTE_FORCE_ALL, &r));
	CHECK_FALSE(u_wv_report_should_demote(true, U_WV_DEMOTE_FORCE_ALL, &r));
}

TEST_CASE("window_visibility_report: guard 2 - never demoted while foreground", "[window_visibility]")
{
	for (u_wv_reason h : {U_WV_REASON_COVERED, U_WV_REASON_MINIMIZED, U_WV_REASON_CLOAKED, U_WV_REASON_LOCKED,
	                      U_WV_REASON_DISPLAY_OFF, U_WV_REASON_NOT_VISIBLE}) {
		u_wv_report fg = Report(State(true, h), /*foreground=*/true);
		CHECK(fg.foreground);
		CHECK_FALSE(u_wv_report_should_demote(true, U_WV_DEMOTE_FORCE_ALL, &fg));
		u_wv_report bg = Report(State(true, h), /*foreground=*/false);
		CHECK(u_wv_report_should_demote(true, U_WV_DEMOTE_FORCE_OFF, &bg));
	}
	// The foreground fact is dropped for a visible window (nothing to guard).
	u_wv_report vis = Report(State(false, U_WV_REASON_VISIBLE), true);
	CHECK_FALSE(vis.foreground);
}

TEST_CASE("window_visibility_report: not opted in - only the force lever demotes", "[window_visibility]")
{
	u_wv_report covered = Report(State(true, U_WV_REASON_COVERED));
	u_wv_report mini = Report(State(true, U_WV_REASON_MINIMIZED));
	CHECK_FALSE(u_wv_report_should_demote(false, U_WV_DEMOTE_FORCE_OFF, &covered));
	CHECK_FALSE(u_wv_report_should_demote(false, U_WV_DEMOTE_FORCE_OFF, &mini));
	CHECK_FALSE(u_wv_report_should_demote(false, U_WV_DEMOTE_FORCE_MINIMIZE, &covered));
	CHECK(u_wv_report_should_demote(false, U_WV_DEMOTE_FORCE_MINIMIZE, &mini));
	CHECK(u_wv_report_should_demote(false, U_WV_DEMOTE_FORCE_ALL, &covered));
	CHECK(u_wv_report_should_demote(false, U_WV_DEMOTE_FORCE_ALL, &mini));
	u_wv_report mini_fg = Report(State(true, U_WV_REASON_MINIMIZED), true);
	CHECK_FALSE(u_wv_report_should_demote(false, U_WV_DEMOTE_FORCE_MINIMIZE, &mini_fg));
}

TEST_CASE("window_visibility_report: force lever values", "[window_visibility]")
{
	CHECK(u_wv_demote_force_parse(nullptr) == U_WV_DEMOTE_FORCE_OFF);
	CHECK(u_wv_demote_force_parse("") == U_WV_DEMOTE_FORCE_OFF);
	CHECK(u_wv_demote_force_parse("off") == U_WV_DEMOTE_FORCE_OFF);
	CHECK(u_wv_demote_force_parse("0") == U_WV_DEMOTE_FORCE_OFF);
	CHECK(u_wv_demote_force_parse("bogus") == U_WV_DEMOTE_FORCE_OFF);
	CHECK(u_wv_demote_force_parse("minimize") == U_WV_DEMOTE_FORCE_MINIMIZE);
	CHECK(u_wv_demote_force_parse("MINIMIZE") == U_WV_DEMOTE_FORCE_MINIMIZE);
	CHECK(u_wv_demote_force_parse("1") == U_WV_DEMOTE_FORCE_MINIMIZE);
	CHECK(u_wv_demote_force_parse("all") == U_WV_DEMOTE_FORCE_ALL);
	CHECK(u_wv_demote_force_parse("2") == U_WV_DEMOTE_FORCE_ALL);
	CHECK(std::strcmp(u_wv_demote_force_str(U_WV_DEMOTE_FORCE_ALL), "all") == 0);
}
