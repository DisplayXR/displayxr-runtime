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
 * ADR-065: a driver serves two modes at run time — `[0]` = 2D, `[1]` = its 3D
 * kind — so the tables below are sim-display's two-mode table for each kind its
 * OUTPUT setting can select (sim_display_modes.h), not the old five-mode table.
 * The consequence the floor now has: with only {2D, kind}, a stereo session
 * against a 4-view kind has no 2-view 3D mode to fall back to and is floored to
 * 2D, and a request for mode 1 is denied (fillable == false).
 *
 * Driving the real decision needs an instance, a system, a head device and a
 * plug-in, so the decision itself is a set of pure functions in
 * oxr_legacy_mode_rule.h and that is what is pinned here. Same shape as
 * tests_oxr_legacy_mode_rule.cpp, whose mk()/table helpers this file repeats
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

#include <vector>

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

//! sim-display's run-time table for one OUTPUT kind: [0] = 2D, [1] = the kind
//! (verbatim from sim_display_modes.h).
struct kind_table
{
	const char *output;
	xrt_rendering_mode modes[2];
	uint32_t max_views; //!< the device max — what PRIMARY_MULTIVIEW_DXR reports (#1486)
};

const xrt_rendering_mode k2D = mk(1, 1.0f, 1.0f, false, 1, 1, "2D");

const std::vector<kind_table> kSimKinds = {
    {"anaglyph", {k2D, mk(2, 0.5f, 0.5f, true, 2, 1, "Anaglyph")}, 2},
    {"sbs", {k2D, mk(2, 0.5f, 0.5f, true, 1, 2, "Cropped SBS")}, 2},
    {"squeezed", {k2D, mk(2, 0.5f, 1.0f, true, 2, 1, "Squeezed SBS")}, 2},
    {"interlaced", {k2D, mk(2, 0.5f, 0.5f, true, 2, 1, "Interlaced")}, 2},
    {"blend", {k2D, mk(2, 0.5f, 0.5f, true, 2, 1, "Blend")}, 2},
    {"quad", {k2D, mk(4, 0.5f, 0.5f, true, 2, 2, "Quad")}, 4},
};

//! The 4-view inspection kind — the only N-view path that runs without hardware.
const kind_table &
quad()
{
	return kSimKinds.back();
}

//! The vendor shape: 2D + one 2-view 3D mode, so nothing here is ever
//! unfillable by a stereo session and the floor must be a no-op.
const xrt_rendering_mode kStereoOnly[] = {
    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
    mk(2, 0.5f, 1.0f, true, 2, 1, "SBS"),
};
constexpr uint32_t kStereoOnlyCount = 2;

} // namespace

TEST_CASE("fillability is 'the mode needs no more views than the session submits' (#1499)", "[oxr][mode_fillable]")
{
	SECTION("max_views = 2 — a PRIMARY_STEREO session, and the legacy case")
	{
		for (const kind_table &k : kSimKinds) {
			INFO("OUTPUT=" << k.output);
			CHECK(oxr_mode_fillable_by(&k.modes[0], 2)); // 2D, 1 view
			CHECK(oxr_mode_fillable_by(&k.modes[1], 2) == (k.max_views <= 2));
		}
		CHECK_FALSE(oxr_mode_fillable_by(&quad().modes[1], 2)); // Quad, 4 views — THE BUG
	}

	SECTION("max_views = the device max — a PRIMARY_MULTIVIEW_DXR session fills everything")
	{
		for (const kind_table &k : kSimKinds) {
			for (uint32_t i = 0; i < 2; i++) {
				INFO("OUTPUT=" << k.output << " mode " << i);
				CHECK(oxr_mode_fillable_by(&k.modes[i], k.max_views));
			}
		}
	}

	SECTION("max_views = 1 — a mono session fills only the 1-view mode")
	{
		for (const kind_table &k : kSimKinds) {
			INFO("OUTPUT=" << k.output);
			CHECK(oxr_mode_fillable_by(&k.modes[0], 1));
			CHECK_FALSE(oxr_mode_fillable_by(&k.modes[1], 1));
		}
	}

	SECTION("degenerate inputs are false, never a crash")
	{
		CHECK_FALSE(oxr_mode_fillable_by(nullptr, 2));
		CHECK_FALSE(oxr_mode_fillable_by(nullptr, 0));
		// max_views == 0 is not a session: nothing is fillable by it. The
		// pick below refuses to act on it at all rather than flooring to 2D.
		CHECK_FALSE(oxr_mode_fillable_by(&k2D, 0));
	}

	SECTION("the bound is a bound, not a check against the one N-view kind that exists today")
	{
		const xrt_rendering_mode three = mk(3, 0.34f, 1.0f, true, 3, 1, "Triple");
		CHECK_FALSE(oxr_mode_fillable_by(&three, 2));
		CHECK(oxr_mode_fillable_by(&three, 3));
		CHECK(oxr_mode_fillable_by(&three, 4));
	}
}

TEST_CASE("the pick leaves every fillable mode alone (#1499)", "[oxr][mode_fillable]")
{
	for (const kind_table &k : kSimKinds) {
		for (uint32_t i = 0; i < 2; i++) {
			if (!oxr_mode_fillable_by(&k.modes[i], 2)) {
				continue;
			}
			INFO("OUTPUT=" << k.output << " mode " << i);
			CHECK(oxr_pick_fillable_mode_index(k.modes, 2, i, 2) == i);
		}
	}
	for (uint32_t i = 0; i < kStereoOnlyCount; i++) {
		INFO("stereo-only mode " << i);
		CHECK(oxr_pick_fillable_mode_index(kStereoOnly, kStereoOnlyCount, i, 2) == i);
	}
}

TEST_CASE("a stereo session against the 4-view kind is floored to 2D and denied mode 1 (#1499, ADR-065)",
          "[oxr][mode_fillable]")
{
	// THE two-mode consequence. The old five-mode table let the floor keep the
	// session in 3D (Quad fell back to Anaglyph); with {2D, Quad} there is no
	// 2-view 3D mode, so the session is floored all the way to 2D...
	const kind_table &q = quad();
	const uint32_t picked = oxr_pick_fillable_mode_index(q.modes, 2, 1, 2);
	CHECK(picked == 0);
	CHECK_FALSE(q.modes[picked].hardware_display_3d);
	// ...which it can paint whole, so the floor stays safe to apply blindly...
	CHECK(oxr_mode_fillable_by(&q.modes[picked], 2));
	// ...and a request for mode 1 is exactly what the request path denies
	// (XRT_DISPLAY_MODE_DENIAL_REASON_VIEW_CONFIG_CANNOT_FILL): the denial gate
	// is `!oxr_mode_fillable_by(&modes[modeIndex], max_submit)`.
	CHECK_FALSE(oxr_mode_fillable_by(&q.modes[1], 2));
	// A MULTIVIEW session against the same kind is neither floored nor denied.
	CHECK(oxr_pick_fillable_mode_index(q.modes, 2, 1, q.max_views) == 1);
	CHECK(oxr_mode_fillable_by(&q.modes[1], q.max_views));
}

TEST_CASE("the pick's preferences, on tables longer than a driver serves", "[oxr][mode_fillable]")
{
	// The rule is general (a refused, non-conforming table can still be long),
	// so its preference order stays pinned on synthetic tables.
	SECTION("a 2-view 3D mode is preferred over 2D")
	{
		const xrt_rendering_mode modes[] = {
		    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
		    mk(2, 0.5f, 0.5f, true, 2, 1, "Anaglyph"),
		    mk(4, 0.5f, 0.5f, true, 2, 2, "Quad"),
		};
		CHECK(oxr_pick_fillable_mode_index(modes, 3, 2, 2) == 1);
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
}

TEST_CASE("a PRIMARY_MULTIVIEW_DXR session is never floored (#1499)", "[oxr][mode_fillable]")
{
	// THE invariant of this change: an app that opted into the device's full
	// width sees no floor, no denial and no WARN. If this ever fails, #1499
	// has taken capability away from the exact apps #1486 added it for.
	for (const kind_table &k : kSimKinds) {
		for (uint32_t i = 0; i < 2; i++) {
			INFO("OUTPUT=" << k.output << " mode " << i << ", max_views = " << k.max_views);
			CHECK(oxr_pick_fillable_mode_index(k.modes, 2, i, k.max_views) == i);
			CHECK(oxr_mode_fillable_by(&k.modes[i], k.max_views));
		}
		// Same statement one step more general: max_views at or above the
		// widest mode in the table is inert, whatever the table.
		for (uint32_t max = k.max_views; max <= k.max_views + 4; max++) {
			for (uint32_t i = 0; i < 2; i++) {
				INFO("OUTPUT=" << k.output << " mode " << i << ", max_views = " << max);
				CHECK(oxr_pick_fillable_mode_index(k.modes, 2, i, max) == i);
			}
		}
	}
}

TEST_CASE("a 1-view session narrows to the 2D mode (#1499)", "[oxr][mode_fillable]")
{
	// Not reachable today (PRIMARY_MONO is only advertised by a device whose
	// modes are all 1-view), but the rule must not misbehave if it ever is:
	// no sim kind is a 1-view 3D mode, so step 2 finds nothing and the
	// fallback is mode 0.
	for (const kind_table &k : kSimKinds) {
		INFO("OUTPUT=" << k.output);
		CHECK(oxr_pick_fillable_mode_index(k.modes, 2, 1, 1) == 0);
		CHECK(oxr_pick_fillable_mode_index(k.modes, 2, 0, 1) == 0);
	}
}

TEST_CASE("degenerate pick inputs are returned untouched (#1499)", "[oxr][mode_fillable]")
{
	const kind_table &q = quad();
	CHECK(oxr_pick_fillable_mode_index(nullptr, 0, 3, 2) == 3);
	CHECK(oxr_pick_fillable_mode_index(q.modes, 0, 1, 2) == 1);
	CHECK(oxr_pick_fillable_mode_index(q.modes, 2, 99, 2) == 99);
	// max_views == 0: a session that has not begun a view configuration. The
	// rule may narrow a real choice, never invent one, so the active index
	// stands - including on the Quad index, which a naive "0 fills nothing"
	// reading would have floored to 2D.
	CHECK(oxr_pick_fillable_mode_index(q.modes, 2, 1, 0) == 1);
	CHECK(oxr_pick_fillable_mode_index(q.modes, 2, 0, 0) == 0);
}

/*
 * The carve-out both call sites share.
 *
 * The runtime-side helper is oxr_session_may_move_display_mode(), which is NOT
 * host-drivable - it queries the head device for
 * XRT_DEVICE_PROPERTY_OUTPUT_MODE_PINNED and reads the system compositor's
 * is_service_mode. Its DECISION, though, is this pure function, and that is the
 * half that has to be right: the begin-time floor and the request denial are
 * the same rule pointing in two directions, and when they disagreed (the first
 * cut of the denial gate skipped these entirely) the result was a pinned
 * session being refused permission to re-request the mode it was already in,
 * and a service-mode client's request never reaching the lease holder.
 */
TEST_CASE("only an unpinned, non-service device may be floored (#1499)", "[oxr][mode_fillable]")
{
	CHECK(oxr_may_demote(false, false));
	// SIM_DISPLAY_FORCE_MODE: the dev pin outranks the floor, deliberately —
	// it is what keeps the N-view under-submit path testable (OUTPUT=quad
	// FORCE_MODE=1 holds a stereo session in Quad). It equally outranks the
	// DENIAL: the device holds the mode, so the runtime has no business
	// refusing a request for the mode the session is already in.
	CHECK_FALSE(oxr_may_demote(true, false));
	// Service mode: the panel lease owns the display-global mode, so the
	// request must be FORWARDED, not answered locally. #1499 tells the session
	// (S4's observation WARN) instead of clamping it.
	CHECK_FALSE(oxr_may_demote(false, true));
	CHECK_FALSE(oxr_may_demote(true, true));

	// Stated as the truth table both call sites are required to share: the
	// runtime may act on its own opinion of the mode in exactly one of the
	// four states.
	uint32_t may = 0;
	for (int pinned = 0; pinned < 2; pinned++) {
		for (int service = 0; service < 2; service++) {
			may += oxr_may_demote(pinned != 0, service != 0) ? 1u : 0u;
		}
	}
	CHECK(may == 1u);
}
