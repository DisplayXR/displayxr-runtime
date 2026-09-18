// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1499: the GENERAL mode floor — a session never runs in a rendering
 *         mode it cannot fill, whatever its view configuration.
 *
 * #1510 pinned the legacy half of this rule (a fixed two-view submission).
 * #1499 parameterises it on `max_views` = the view count of the primary view
 * configuration the session BEGAN, so `PRIMARY_STEREO` gets the same treatment
 * a legacy app already had and `PRIMARY_MULTIVIEW_DXR` gets none at all.
 *
 * Driving the real decision needs an instance, a system, a head device and a
 * plug-in, so the decision itself is a set of pure functions in
 * oxr_legacy_mode_rule.h and that is what is pinned here. Same shape as
 * tests_oxr_legacy_mode_rule.cpp, whose mk()/kSim tables this file repeats
 * deliberately rather than sharing: the two suites must be able to disagree if
 * someone breaks the generalisation, which a shared fixture would hide.
 *
 * NOT pinned here (it is on the Windows hardware leg, and partly on the
 * headless arms in tests_oxr_view_space.cpp): the WIRING — that xrBeginSession
 * reads `sess->view_config_view_count`, moves the device, refreshes the
 * recommended view scales and pushes XrEventDataRenderingModeChangedDXR, and
 * that xrRequestDisplayRenderingModeDXR answers with a denial event.
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
//! ever unfillable by a stereo session and the floor must be a no-op.
const xrt_rendering_mode kStereoOnly[] = {
    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
    mk(2, 0.5f, 1.0f, true, 2, 1, "SBS"),
};
constexpr uint32_t kStereoOnlyCount = 2;

//! The device max across kSim — what PRIMARY_MULTIVIEW_DXR reports (#1486).
constexpr uint32_t kSimDeviceMax = 4;

} // namespace

TEST_CASE("fillability is 'the mode needs no more views than the session submits' (#1499)", "[oxr][mode_fillable]")
{
	SECTION("max_views = 2 — a PRIMARY_STEREO session, and the legacy case")
	{
		CHECK(oxr_mode_fillable_by(&kSim[0], 2)); // 2D, 1 view
		CHECK(oxr_mode_fillable_by(&kSim[1], 2)); // Anaglyph, 2 views
		CHECK(oxr_mode_fillable_by(&kSim[2], 2));
		CHECK(oxr_mode_fillable_by(&kSim[3], 2));
		CHECK_FALSE(oxr_mode_fillable_by(&kSim[4], 2)); // Quad, 4 views — THE BUG
	}

	SECTION("max_views = 4 — a PRIMARY_MULTIVIEW_DXR session on this device fills everything")
	{
		for (uint32_t i = 0; i < kSimCount; i++) {
			INFO("sim-display mode " << i);
			CHECK(oxr_mode_fillable_by(&kSim[i], kSimDeviceMax));
		}
	}

	SECTION("max_views = 1 — a mono session fills only the 1-view mode")
	{
		CHECK(oxr_mode_fillable_by(&kSim[0], 1));
		for (uint32_t i = 1; i < kSimCount; i++) {
			INFO("sim-display mode " << i);
			CHECK_FALSE(oxr_mode_fillable_by(&kSim[i], 1));
		}
	}

	SECTION("degenerate inputs are false, never a crash")
	{
		CHECK_FALSE(oxr_mode_fillable_by(nullptr, 2));
		CHECK_FALSE(oxr_mode_fillable_by(nullptr, 0));
		// max_views == 0 is not a session: nothing is fillable by it. The
		// pick below refuses to act on it at all rather than flooring to 2D.
		CHECK_FALSE(oxr_mode_fillable_by(&kSim[0], 0));
	}

	SECTION("the bound is a bound, not a check against the one N-view mode that exists today")
	{
		const xrt_rendering_mode three = mk(3, 0.34f, 1.0f, true, 3, 1, "Triple");
		CHECK_FALSE(oxr_mode_fillable_by(&three, 2));
		CHECK(oxr_mode_fillable_by(&three, 3));
		CHECK(oxr_mode_fillable_by(&three, 4));
	}
}

TEST_CASE("the pick narrows a 2-view session out of Quad (#1499)", "[oxr][mode_fillable]")
{
	SECTION("every fillable mode is left alone")
	{
		for (uint32_t i = 0; i < 4; i++) {
			INFO("sim-display mode " << i);
			CHECK(oxr_pick_fillable_mode_index(kSim, kSimCount, i, 2) == i);
		}
		for (uint32_t i = 0; i < kStereoOnlyCount; i++) {
			INFO("stereo-only mode " << i);
			CHECK(oxr_pick_fillable_mode_index(kStereoOnly, kStereoOnlyCount, i, 2) == i);
		}
	}

	SECTION("Quad falls back to Anaglyph, the first 2-view 3D mode")
	{
		CHECK(oxr_pick_fillable_mode_index(kSim, kSimCount, 4, 2) == 1);
	}

	SECTION("with no 2-view mode at all, a 1-view 3D mode beats plain 2D")
	{
		const xrt_rendering_mode modes[] = {
		    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
		    mk(1, 1.0f, 1.0f, true, 1, 1, "Mono 3D"),
		    mk(4, 0.5f, 0.5f, true, 2, 2, "Quad"),
		};
		CHECK(oxr_pick_fillable_mode_index(modes, 3, 2, 2) == 1);
	}

	SECTION("a panel whose ONLY 3D mode is N-view falls all the way back to mode 0 (2D)")
	{
		const xrt_rendering_mode modes[] = {
		    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
		    mk(4, 0.5f, 0.5f, true, 2, 2, "Quad"),
		};
		const uint32_t picked = oxr_pick_fillable_mode_index(modes, 2, 1, 2);
		CHECK(picked == 0);
		// ...and the floor always terminates somewhere the session can paint
		// the whole canvas, which is what makes it safe to apply blindly.
		CHECK(oxr_mode_fillable_by(&modes[picked], 2));
	}
}

TEST_CASE("a PRIMARY_MULTIVIEW_DXR session is never floored (#1499)", "[oxr][mode_fillable]")
{
	// THE invariant of this change: an app that opted into the device's full
	// width sees no floor, no denial and no WARN. If this ever fails, #1499
	// has taken capability away from the exact apps #1486 added it for.
	for (uint32_t i = 0; i < kSimCount; i++) {
		INFO("sim-display mode " << i << ", max_views = " << kSimDeviceMax);
		CHECK(oxr_pick_fillable_mode_index(kSim, kSimCount, i, kSimDeviceMax) == i);
		CHECK(oxr_mode_fillable_by(&kSim[i], kSimDeviceMax));
	}

	// Same statement one step more general: max_views at or above the widest
	// mode in the table is inert, whatever the table.
	for (uint32_t max = kSimDeviceMax; max <= kSimDeviceMax + 4; max++) {
		for (uint32_t i = 0; i < kSimCount; i++) {
			INFO("mode " << i << ", max_views = " << max);
			CHECK(oxr_pick_fillable_mode_index(kSim, kSimCount, i, max) == i);
		}
	}
}

TEST_CASE("a 1-view session narrows to the 2D mode (#1499)", "[oxr][mode_fillable]")
{
	// Not reachable today (PRIMARY_MONO is only advertised by a device whose
	// modes are all 1-view), but the rule must not misbehave if it ever is:
	// there is no 1-view 3D mode on sim-display, so step 2 finds nothing and
	// the fallback is mode 0.
	for (uint32_t i = 1; i < kSimCount; i++) {
		INFO("sim-display mode " << i);
		CHECK(oxr_pick_fillable_mode_index(kSim, kSimCount, i, 1) == 0);
	}
	CHECK(oxr_pick_fillable_mode_index(kSim, kSimCount, 0, 1) == 0);
}

TEST_CASE("degenerate pick inputs are returned untouched (#1499)", "[oxr][mode_fillable]")
{
	CHECK(oxr_pick_fillable_mode_index(nullptr, 0, 3, 2) == 3);
	CHECK(oxr_pick_fillable_mode_index(kSim, 0, 3, 2) == 3);
	CHECK(oxr_pick_fillable_mode_index(kSim, kSimCount, 99, 2) == 99);
	// max_views == 0: a session that has not begun a view configuration. The
	// rule may narrow a real choice, never invent one, so the active index
	// stands - including on the Quad index, which a naive "0 fills nothing"
	// reading would have floored to 2D.
	CHECK(oxr_pick_fillable_mode_index(kSim, kSimCount, 4, 0) == 4);
	CHECK(oxr_pick_fillable_mode_index(kSim, kSimCount, 1, 0) == 1);
}

TEST_CASE("only an unpinned, non-service device may be floored (#1499)", "[oxr][mode_fillable]")
{
	CHECK(oxr_may_demote(false, false));
	// SIM_DISPLAY_FORCE_MODE: the dev pin outranks the floor, deliberately —
	// it is what keeps the N-view under-submit path testable.
	CHECK_FALSE(oxr_may_demote(true, false));
	// Service mode: the panel lease owns the display-global mode. #1499 tells
	// the session (S4's observation WARN) instead of clamping it.
	CHECK_FALSE(oxr_may_demote(false, true));
	CHECK_FALSE(oxr_may_demote(true, true));
}
