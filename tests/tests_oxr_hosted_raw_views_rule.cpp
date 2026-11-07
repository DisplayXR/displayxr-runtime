// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2322: the hosted raw-views opt-in reaches exactly one session class.
 *
 * Creating a hosted session of a display-aware app needs a graphics binding and
 * a window (the headless suite's display-aware sessions are all bridge relays),
 * so, like tests_oxr_tracking_properties_rule.cpp, the decision lives as a pure
 * function in oxr_hosted_raw_views_rule.h and that is what is pinned here. The
 * wiring (oxr_session_create -> oxr_session::hosted_raw_views ->
 * oxr_session_views_display_relative) logs one WARN per session either way.
 */

#include "catch_amalgamated.hpp"

#include "oxr_hosted_raw_views_rule.h"

TEST_CASE("#2322 without the opt-in no session changes class")
{
	for (int ext = 0; ext < 2; ext++) {
		for (int relay = 0; relay < 2; relay++) {
			for (int svc = 0; svc < 2; svc++) {
				for (int di = 0; di < 2; di++) {
					CHECK_FALSE(oxr_hosted_raw_views_eligible(false, ext != 0, relay != 0, svc != 0,
					                                          di != 0));
				}
			}
		}
	}
}

TEST_CASE("#2322 the opt-in reaches an in-process hosted display-aware session")
{
	CHECK(oxr_hosted_raw_views_eligible(true, false, false, false, true));
}

TEST_CASE("#2322 the opt-in leaves every other class alone")
{
	// A legacy hosted app keeps render-ready views.
	CHECK_FALSE(oxr_hosted_raw_views_eligible(true, false, false, false, false));
	// Over IPC the server's hosted per-view poses are head-local.
	CHECK_FALSE(oxr_hosted_raw_views_eligible(true, false, false, true, true));
	// External-window sessions and bridge relays are RAW already.
	CHECK_FALSE(oxr_hosted_raw_views_eligible(true, true, false, false, true));
	CHECK_FALSE(oxr_hosted_raw_views_eligible(true, false, true, false, true));
}
