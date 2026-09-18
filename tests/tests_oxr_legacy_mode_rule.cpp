// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1510: the legacy mode floor and the compromise view scale it implies.
 *
 * Driving the real decision needs an instance, a system, a head device and a
 * plug-in — none of which a headless CI runner has — so the decision itself is
 * a set of pure functions in oxr_legacy_mode_rule.h and that is what is pinned
 * here. Same shape as tests_oxr_view_config_rule.cpp and tests_oxr_weave_latch.cpp.
 *
 * NOT pinned here (it is on the Windows hardware leg): the WIRING — that
 * oxr_system_fill_in() feeds the picked mode to the compromise scale, that
 * xrBeginSession switches the device to it, and that the compositor's tile grid
 * follows.
 */

#include "catch_amalgamated.hpp"

#include "oxr_legacy_mode_rule.h"

namespace {

xrt_rendering_mode
mk(uint32_t views, float sx, float sy, bool hw3d, uint32_t cols, uint32_t rows, const char *name)
{
	xrt_rendering_mode m{};
	m.view_count = views;
	m.view_scale_x = sx;
	m.view_scale_y = sy;
	m.hardware_display_3d = hw3d;
	m.tile_columns = cols;
	m.tile_rows = rows;
	// mode_name is a fixed char array; the tests only read it via INFO().
	for (size_t i = 0; name[i] != '\0' && i + 1 < sizeof(m.mode_name); i++) {
		m.mode_name[i] = name[i];
	}
	return m;
}

//! sim-display's five modes, verbatim from sim_display_device.c.
//! 0=2D 1=Anaglyph(default 3D) 2=Cropped SBS 3=Squeezed SBS 4=Quad.
const xrt_rendering_mode kSim[] = {
    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),          //
    mk(2, 0.5f, 0.5f, true, 2, 1, "Anaglyph"),     //
    mk(2, 0.5f, 0.5f, true, 1, 2, "Cropped SBS"),  //
    mk(2, 0.5f, 1.0f, true, 2, 1, "Squeezed SBS"), //
    mk(4, 0.5f, 0.5f, true, 2, 2, "Quad"),         //
};
constexpr uint32_t kSimCount = 5;

//! The Leia shape: the plug-in hardcodes view_count = 2, so nothing here is
//! ever unfillable and the floor must be a no-op on every index.
const xrt_rendering_mode kStereoOnly[] = {
    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
    mk(2, 0.5f, 1.0f, true, 2, 1, "SBS"),
};
constexpr uint32_t kStereoOnlyCount = 2;

struct vscale
{
	float x;
	float y;
};

vscale
compromise_of(const xrt_rendering_mode &m)
{
	vscale s{-1.0f, -1.0f};
	oxr_legacy_compromise_scale(&m, &s.x, &s.y);
	return s;
}

} // namespace

TEST_CASE("legacy tile scaling applies only to a multi-mode device (#1510)", "[oxr][legacy_mode_rule]")
{
	// A single-mode device has nothing to compromise BETWEEN, so
	// legacy_app_tile_scaling stays false and the compositor's own
	// recommended scale stands.
	CHECK_FALSE(oxr_legacy_tile_scaling_applies(0));
	CHECK_FALSE(oxr_legacy_tile_scaling_applies(1));
	// Two or more: the flag is raised, for Case A and Case B alike.
	CHECK(oxr_legacy_tile_scaling_applies(2));
	CHECK(oxr_legacy_tile_scaling_applies(kSimCount));
}

TEST_CASE("only an unpinned, non-service device may be floored (#1510)", "[oxr][legacy_mode_rule]")
{
	CHECK(oxr_legacy_may_demote(false, false));
	// SIM_DISPLAY_FORCE_MODE: the dev pin outranks the floor, deliberately —
	// it is what keeps the N-view under-submit path testable.
	CHECK_FALSE(oxr_legacy_may_demote(true, false));
	// Service mode: the panel lease owns the display-global mode.
	CHECK_FALSE(oxr_legacy_may_demote(false, true));
	CHECK_FALSE(oxr_legacy_may_demote(true, true));
}

TEST_CASE("fillability is 'at most the two views a legacy app submits' (#1510)", "[oxr][legacy_mode_rule]")
{
	CHECK(oxr_legacy_mode_is_fillable(&kSim[0])); // 2D, 1 view
	CHECK(oxr_legacy_mode_is_fillable(&kSim[1])); // Anaglyph, 2 views
	CHECK(oxr_legacy_mode_is_fillable(&kSim[2]));
	CHECK(oxr_legacy_mode_is_fillable(&kSim[3]));
	CHECK_FALSE(oxr_legacy_mode_is_fillable(&kSim[4])); // Quad, 4 views — THE BUG
	CHECK_FALSE(oxr_legacy_mode_is_fillable(nullptr));

	// A hypothetical 3-view mode is unfillable too: the rule is a bound, not
	// a check against the one N-view mode that happens to exist today.
	const xrt_rendering_mode three = mk(3, 0.34f, 1.0f, true, 3, 1, "Triple");
	CHECK_FALSE(oxr_legacy_mode_is_fillable(&three));
}

TEST_CASE("the floor leaves every fillable mode alone (#1510)", "[oxr][legacy_mode_rule]")
{
	// The shipping path: nothing moves. This is the regression guard that
	// matters most — the default (Anaglyph) run must be byte-identical to
	// pre-#1510 behaviour.
	for (uint32_t i = 0; i < 4; i++) {
		INFO("sim-display mode " << i);
		CHECK(oxr_legacy_pick_mode_index(kSim, kSimCount, i) == i);
	}
	for (uint32_t i = 0; i < kStereoOnlyCount; i++) {
		INFO("stereo-only mode " << i);
		CHECK(oxr_legacy_pick_mode_index(kStereoOnly, kStereoOnlyCount, i) == i);
	}
}

TEST_CASE("the floor demotes an unfillable mode to the best stereo 3D mode (#1510)", "[oxr][legacy_mode_rule]")
{
	SECTION("sim-display Quad falls back to Anaglyph, the first 2-view 3D mode")
	{
		CHECK(oxr_legacy_pick_mode_index(kSim, kSimCount, 4) == 1);
	}

	SECTION("with no 2-view mode at all, a 1-view 3D mode is still preferred over plain 2D")
	{
		const xrt_rendering_mode modes[] = {
		    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
		    mk(1, 1.0f, 1.0f, true, 1, 1, "Mono 3D"),
		    mk(4, 0.5f, 0.5f, true, 2, 2, "Quad"),
		};
		CHECK(oxr_legacy_pick_mode_index(modes, 3, 2) == 1);
	}

	SECTION("a panel whose ONLY 3D mode is N-view falls all the way back to mode 0 (2D)")
	{
		const xrt_rendering_mode modes[] = {
		    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
		    mk(4, 0.5f, 0.5f, true, 2, 2, "Quad"),
		};
		CHECK(oxr_legacy_pick_mode_index(modes, 2, 1) == 0);
		// ...and mode 0 IS fillable, so the floor always terminates somewhere
		// the app can paint the whole canvas (mono, over a 1x1 grid).
		CHECK(oxr_legacy_mode_is_fillable(&modes[oxr_legacy_pick_mode_index(modes, 2, 1)]));
	}

	SECTION("degenerate inputs are returned untouched rather than guessed at")
	{
		CHECK(oxr_legacy_pick_mode_index(nullptr, 0, 3) == 3);
		CHECK(oxr_legacy_pick_mode_index(kSim, 0, 3) == 3);
		CHECK(oxr_legacy_pick_mode_index(kSim, kSimCount, 99) == 99);
	}
}

TEST_CASE("the compromise scale follows the mode the session will run in (#1510)", "[oxr][legacy_mode_rule]")
{
	SECTION("view_count 2, both scales <= 0.5 — Case A, 0.5x1.0 fills the 2x1 grid")
	{
		vscale s{-1.0f, -1.0f};
		CHECK(oxr_legacy_compromise_scale(&kSim[1], &s.x, &s.y)); // Anaglyph 0.5x0.5
		CHECK(s.x == Catch::Approx(0.5f));
		CHECK(s.y == Catch::Approx(1.0f));
		// Cropped SBS is the same shape on a 1x2 grid and takes the same branch.
		CHECK(oxr_legacy_compromise_scale(&kSim[2], &s.x, &s.y));
		CHECK(s.x == Catch::Approx(0.5f));
		CHECK(s.y == Catch::Approx(1.0f));
	}

	SECTION("view_count 2 that is NOT SBS-shaped — Case B, the mode's own scale")
	{
		// Squeezed SBS is already 0.5x1.0: Case A would be a no-op, so the
		// rule must report Case B and hand back the mode's own numbers.
		vscale s{-1.0f, -1.0f};
		CHECK_FALSE(oxr_legacy_compromise_scale(&kSim[3], &s.x, &s.y));
		CHECK(s.x == Catch::Approx(0.5f));
		CHECK(s.y == Catch::Approx(1.0f));
	}

	SECTION("view_count 1 — Case B, 1.0x1.0, the whole canvas mono")
	{
		vscale s{-1.0f, -1.0f};
		CHECK_FALSE(oxr_legacy_compromise_scale(&kSim[0], &s.x, &s.y));
		CHECK(s.x == Catch::Approx(1.0f));
		CHECK(s.y == Catch::Approx(1.0f));
	}

	SECTION("view_count 4 — Case B, NOT widened to 0.5x1.0")
	{
		// The rejected alternative. 0.5x1.0 only makes sense on a 2x1 grid;
		// the compositor and the DP would still be on the mode's 2x2 one, so
		// widening Case A here trades two clean unpainted quadrants for a
		// wrong-stride weave. A 4-view mode only reaches this function when
		// the floor could not be applied (pinned device / service mode).
		vscale s{-1.0f, -1.0f};
		CHECK_FALSE(oxr_legacy_compromise_scale(&kSim[4], &s.x, &s.y));
		CHECK(s.x == Catch::Approx(0.5f));
		CHECK(s.y == Catch::Approx(0.5f));
	}

	SECTION("end to end on sim-display: pick, then scale")
	{
		// Quad default (SIM_DISPLAY_OUTPUT=quad, unpinned) — what the fix buys.
		const uint32_t picked = oxr_legacy_pick_mode_index(kSim, kSimCount, 4);
		const vscale s = compromise_of(kSim[picked]);
		CHECK(picked == 1);
		CHECK(s.x == Catch::Approx(0.5f));
		CHECK(s.y == Catch::Approx(1.0f));

		// SIM_DISPLAY_FORCE_MODE=4 — pinned, so no pick happens and Case B
		// stands. This is the DOCUMENTED loss, and the session WARN names it.
		const vscale pinned = compromise_of(kSim[4]);
		CHECK(pinned.x == Catch::Approx(0.5f));
		CHECK(pinned.y == Catch::Approx(0.5f));
		CHECK_FALSE(oxr_legacy_mode_is_fillable(&kSim[4]));
	}

	SECTION("null out-params are refused rather than written through")
	{
		float x = 7.0f;
		CHECK_FALSE(oxr_legacy_compromise_scale(&kSim[1], &x, nullptr));
		CHECK_FALSE(oxr_legacy_compromise_scale(&kSim[1], nullptr, &x));
		CHECK_FALSE(oxr_legacy_compromise_scale(nullptr, &x, &x));
		CHECK(x == Catch::Approx(7.0f));
	}
}
