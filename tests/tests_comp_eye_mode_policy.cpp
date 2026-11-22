// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The eye-tracking mode (MANAGED / MANUAL) the D3D11 service's shared
 *        panel display processor carries.
 *
 * WHAT IS PINNED
 * --------------
 * pipeline_eye_mode_sync() in comp_d3d11_service.cpp asks comp_eye_mode_decide()
 * every render tick (and at a holder's teardown) which mode the panel DP should
 * carry and whether the vendor has to be told. This suite compiles the same
 * header; there is no second copy of the rule. The cases replay the sequences
 * that matter: a client asks for MANUAL while it holds the panel, loses it,
 * leaves, and a controller composes.
 *
 * The end-to-end shape (a forced-IPC app calling xrRequestEyeTrackingModeDXR
 * against a running service on a tracked panel) needs a device, a panel and a
 * display processor that offers both modes; it is a hardware check.
 */

#include "catch_amalgamated.hpp"

#include "comp_eye_mode_policy.h"

namespace {

constexpr uint32_t MANAGED = 0;
constexpr uint32_t MANUAL = 1;

//! The panel DP, as the service tracks it across ticks.
struct panel
{
	uint32_t default_mode = MANAGED;
	uint32_t told_plus1 = 0; // fresh DP
	bool nondefault_told = false;
	int tells = 0;
	uint32_t last_told = 99;

	//! One tick: holder (woven by the panel DP or not) with its wish.
	comp_eye_mode_result
	tick(bool holder_woven, int32_t wish)
	{
		comp_eye_mode_state s{};
		s.holder_woven_by_panel_dp = holder_woven;
		s.holder_wish = wish;
		s.default_mode = default_mode;
		s.told_plus1 = told_plus1;
		s.nondefault_told = nondefault_told;
		comp_eye_mode_result r = comp_eye_mode_decide(&s);
		// What pipeline_eye_mode_sync does with it.
		told_plus1 = r.mode + 1u;
		if (r.tell_dp) {
			tells++;
			last_told = r.mode;
			nondefault_told = r.mode != default_mode;
		}
		return r;
	}

	//! The DP instance was destroyed / re-bound (pipeline_dp_forget_beliefs).
	void
	recreate()
	{
		told_plus1 = 0;
	}
};

} // namespace

TEST_CASE("eye_mode: nobody ever asks — the vendor is never called")
{
	panel p;
	for (int i = 0; i < 5; i++) {
		comp_eye_mode_result r = p.tick(true, -1);
		CHECK(r.mode == MANAGED);
		CHECK_FALSE(r.from_holder);
	}
	p.tick(false, -1); // compose
	p.recreate();
	p.tick(true, -1);
	CHECK(p.tells == 0);
}

TEST_CASE("eye_mode: the presented client's MANUAL is applied once")
{
	panel p;
	comp_eye_mode_result r = p.tick(true, MANUAL);
	CHECK(r.tell_dp);
	CHECK(r.mode == MANUAL);
	CHECK(r.from_holder);
	p.tick(true, MANUAL);
	p.tick(true, MANUAL);
	CHECK(p.tells == 1);
}

TEST_CASE("eye_mode: the MANUAL client loses the panel — MANAGED is restored")
{
	panel p;
	p.tick(true, MANUAL);
	// Another client (never asked) takes focus.
	comp_eye_mode_result r = p.tick(true, -1);
	CHECK(r.tell_dp);
	CHECK(r.mode == MANAGED);
	CHECK_FALSE(r.from_holder);
	CHECK(p.tells == 2);
	CHECK_FALSE(p.nondefault_told);
}

TEST_CASE("eye_mode: the MANUAL client leaves with nobody after it — MANAGED is restored")
{
	panel p;
	p.tick(true, MANUAL);
	comp_eye_mode_result r = p.tick(false, -1); // teardown / nobody presented
	CHECK(r.tell_dp);
	CHECK(r.mode == MANAGED);
}

TEST_CASE("eye_mode: a controller composes — the workspace owns mode, the default is restored")
{
	panel p;
	p.tick(true, MANUAL); // direct path before the controller attached
	comp_eye_mode_result r = p.tick(false, MANUAL); // compose: holder not woven
	CHECK(r.tell_dp);
	CHECK(r.mode == MANAGED);
	// A foreground override hands the panel back to the client: its wish returns.
	r = p.tick(true, MANUAL);
	CHECK(r.tell_dp);
	CHECK(r.mode == MANUAL);
}

TEST_CASE("eye_mode: a present-owner or client-texture presenter is not woven by the panel DP")
{
	panel p;
	comp_eye_mode_result r = p.tick(false, MANUAL);
	CHECK_FALSE(r.tell_dp);
	CHECK(r.mode == MANAGED);
	CHECK(p.tells == 0);
}

TEST_CASE("eye_mode: a DP recreated while MANUAL is held gets the wish again")
{
	panel p;
	p.tick(true, MANUAL);
	p.recreate();
	comp_eye_mode_result r = p.tick(true, MANUAL);
	CHECK(r.tell_dp);
	CHECK(r.mode == MANUAL);
}

TEST_CASE("eye_mode: a DP recreated after MANUAL was told is told the default explicitly")
{
	// The vendor's tracking policy may outlive one DP instance.
	panel p;
	p.tick(true, MANUAL);
	p.recreate();
	comp_eye_mode_result r = p.tick(true, -1);
	CHECK(r.tell_dp);
	CHECK(r.mode == MANAGED);
	// ... and once the default is told, the next fresh DP is left alone.
	p.recreate();
	r = p.tick(true, -1);
	CHECK_FALSE(r.tell_dp);
}

TEST_CASE("eye_mode: a MANUAL-default system restores MANUAL, not MANAGED")
{
	panel p;
	p.default_mode = MANUAL;
	comp_eye_mode_result r = p.tick(true, MANAGED);
	CHECK(r.tell_dp);
	CHECK(r.mode == MANAGED);
	r = p.tick(true, -1);
	CHECK(r.tell_dp);
	CHECK(r.mode == MANUAL);
}

TEST_CASE("eye_mode: switching back and forth tells the DP each change only")
{
	panel p;
	p.tick(true, MANUAL);
	p.tick(true, MANAGED);
	p.tick(true, MANAGED);
	p.tick(true, MANUAL);
	CHECK(p.tells == 3);
	CHECK(p.last_told == MANUAL);
}
