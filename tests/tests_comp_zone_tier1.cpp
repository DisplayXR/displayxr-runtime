// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-027 tier-1 — which DP answers the zone-support question (#1331).
 * @ingroup tests
 *
 * #1331's VK half shipped **code-read, not measured**, and the reason is worth
 * stating because it is what this file exists to change: nothing on any box was
 * simultaneously a zones app and on the VK split, and the app-level arm cannot
 * run on a hosted CI runner at all — `vulkan-1.dll` is present but **zero**
 * Vulkan ICDs are registered and the only adapter is "Microsoft Hyper-V Video",
 * so `cube_zones_vk_win` cannot start there.
 *
 * The gate itself needs a compositor, a device and a weaver. The DISPATCH needs
 * three booleans, and the dispatch is where the bug was. So that is what gets
 * pinned here, hardware-free, in CI.
 *
 * The load-bearing case is `weaver_on_arm && arm_consumes && !own_dp_consumes`.
 * Pre-#1331 the code read `own_dp_consumes` unconditionally, so that case
 * returned false, tier-1 forced hardware 3D on the first zones frame, and a
 * zones app silently lost its already-honoured 2D request for the rest of the
 * session — on the SHIPPED defaults (VK split + same-adapter d3d12 reroute), not
 * just under a prototype flag.
 *
 * Verified on hardware before being written down here (2026-09-03,
 * `cube_zones_vk_win` + `DXR_ZONES_MODE_2D=1`, same app, only the runtime
 * differing):
 *
 *   pre-#1331 predicate : 3D, 2D, 3D, 3D  -> final witness mode=3d
 *   post-#1331          : 3D, 2D, 3D      -> final witness mode=2d
 *
 * i.e. the app's request IS honoured and then yanked back. This file is that
 * behaviour reduced to the part a runner can execute.
 */

#include "util/comp_zone_tier1.h"

#include "catch_amalgamated.hpp"

TEST_CASE("#1331: tier-1 asks the arm that OWNS the weaver")
{
	SECTION("THE REGRESSION — split session, arm supports zones, tier has no DP")
	{
		// The whole bug, in one line. Pre-#1331 this was `own_dp_consumes`
		// unconditionally, i.e. false, i.e. force 3D over the app's 2D request.
		CHECK(comp_zone_tier1_dp_consumes_zones(/*weaver_on_arm=*/true,
		                                        /*arm_consumes=*/true,
		                                        /*own_dp_consumes=*/false));

		// And therefore tier-1 must NOT force 3D on that frame.
		CHECK_FALSE(comp_zone_tier1_should_force_3d(
		    /*zones_frame=*/true, /*already_requested=*/false,
		    comp_zone_tier1_dp_consumes_zones(true, true, false)));
	}

	SECTION("a split session whose ARM is genuinely legacy still forces 3D")
	{
		// The fallback must survive the fix: a weaver with no zone slots is
		// exactly what tier-1 is for.
		CHECK_FALSE(comp_zone_tier1_dp_consumes_zones(true, false, false));
		CHECK(comp_zone_tier1_should_force_3d(true, false,
		                                     comp_zone_tier1_dp_consumes_zones(true, false, false)));
	}

	SECTION("the arm's answer is ignored when the tier owns its own weaver")
	{
		// Own-legs sessions must be byte-for-byte unchanged by #1331, so the
		// arm's answer may not leak in either direction.
		CHECK(comp_zone_tier1_dp_consumes_zones(/*weaver_on_arm=*/false, /*arm_consumes=*/false,
		                                        /*own_dp_consumes=*/true));
		CHECK_FALSE(comp_zone_tier1_dp_consumes_zones(false, true, false));
	}

	SECTION("full truth table, so a later 'tidy' cannot silently invert it")
	{
		for (int arm = 0; arm < 2; arm++) {
			for (int own = 0; own < 2; own++) {
				CHECK(comp_zone_tier1_dp_consumes_zones(true, arm != 0, own != 0) == (arm != 0));
				CHECK(comp_zone_tier1_dp_consumes_zones(false, arm != 0, own != 0) == (own != 0));
			}
		}
	}
}

TEST_CASE("#1331: the tier-1 rising edge fires once, and only on a zones frame")
{
	SECTION("no zones frame, no force — whatever the DP says")
	{
		CHECK_FALSE(comp_zone_tier1_should_force_3d(false, false, false));
		CHECK_FALSE(comp_zone_tier1_should_force_3d(false, false, true));
	}

	SECTION("already requested means once, not per frame")
	{
		// This latch is why the app-level arm has to request 2D BEFORE zones
		// activate: the gate fires on the FIRST zones frame and never again, so a
		// later request overrides the force and a broken runtime looks healthy.
		// Measured 2026-09-03 — a frame-90 request passed against the pre-#1331
		// predicate; a frame-3 request caught it.
		CHECK_FALSE(comp_zone_tier1_should_force_3d(true, true, false));
	}

	SECTION("legacy DP on a zones frame is the case the fallback exists for")
	{
		CHECK(comp_zone_tier1_should_force_3d(true, false, false));
	}
}
