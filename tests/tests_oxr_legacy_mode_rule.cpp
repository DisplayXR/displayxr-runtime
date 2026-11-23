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
 * ADR-065: a driver serves two modes at run time — `[0]` = 2D, `[1]` = its 3D
 * kind — so the tables are sim-display's two-mode table per OUTPUT kind. Two
 * things follow for a legacy app: an unpinned Quad kind floors it to 2D (there
 * is no 2-view 3D mode left to fall back to), and a device that STARTS in 2D
 * (sim-display's default) still sizes the app for its 3D mode
 * (oxr_legacy_compromise_mode_index), so the default sizing is what it was
 * before the device started in 2D.
 *
 * NOT pinned here (it is on the Windows hardware leg): the WIRING — that
 * oxr_system_fill_in() feeds the picked mode to the compromise scale, that
 * xrBeginSession switches the device to it, and that the compositor's tile grid
 * follows.
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
};

const xrt_rendering_mode k2D = mk(1, 1.0f, 1.0f, false, 1, 1, "2D");
const xrt_rendering_mode kAnaglyph = mk(2, 0.5f, 0.5f, true, 2, 1, "Anaglyph");
const xrt_rendering_mode kCropped = mk(2, 0.5f, 0.5f, true, 1, 2, "Cropped SBS");
const xrt_rendering_mode kSqueezed = mk(2, 0.5f, 1.0f, true, 2, 1, "Squeezed SBS");
const xrt_rendering_mode kQuad = mk(4, 0.5f, 0.5f, true, 2, 2, "Quad");
const xrt_rendering_mode kInterlaced = mk(2, 0.5f, 0.5f, true, 2, 1, "Interlaced");
const xrt_rendering_mode kBlend = mk(2, 0.5f, 0.5f, true, 2, 1, "Blend");

const std::vector<kind_table> kSimKinds = {
    {"anaglyph", {k2D, kAnaglyph}},     {"sbs", {k2D, kCropped}}, {"squeezed", {k2D, kSqueezed}},
    {"interlaced", {k2D, kInterlaced}}, {"blend", {k2D, kBlend}}, {"quad", {k2D, kQuad}},
};

const xrt_rendering_mode kQuadTable[2] = {k2D, kQuad};
const xrt_rendering_mode kAnaglyphTable[2] = {k2D, kAnaglyph};

//! The vendor shape: 2D + one 2-view 3D mode, so nothing here is ever
//! unfillable and the floor must be a no-op on every index.
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
	// Two (every driver under ADR-065) or more: the flag is raised, for
	// Case A and Case B alike.
	CHECK(oxr_legacy_tile_scaling_applies(2));
	CHECK(oxr_legacy_tile_scaling_applies(5));
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
	for (const kind_table &k : kSimKinds) {
		INFO("OUTPUT=" << k.output);
		CHECK(oxr_legacy_mode_is_fillable(&k.modes[0])); // 2D, 1 view
		CHECK(oxr_legacy_mode_is_fillable(&k.modes[1]) == (k.modes[1].view_count <= 2));
	}
	CHECK_FALSE(oxr_legacy_mode_is_fillable(&kQuad)); // Quad, 4 views — THE BUG
	CHECK_FALSE(oxr_legacy_mode_is_fillable(nullptr));

	// A hypothetical 3-view mode is unfillable too: the rule is a bound, not
	// a check against the one N-view kind that happens to exist today.
	const xrt_rendering_mode three = mk(3, 0.34f, 1.0f, true, 3, 1, "Triple");
	CHECK_FALSE(oxr_legacy_mode_is_fillable(&three));
}

TEST_CASE("the floor leaves every fillable mode alone (#1510)", "[oxr][legacy_mode_rule]")
{
	// The shipping path: nothing moves. This is the regression guard that
	// matters most — the default (Anaglyph) run must be byte-identical to
	// pre-#1510 behaviour.
	for (const kind_table &k : kSimKinds) {
		for (uint32_t i = 0; i < 2; i++) {
			if (!oxr_legacy_mode_is_fillable(&k.modes[i])) {
				continue;
			}
			INFO("OUTPUT=" << k.output << " mode " << i);
			CHECK(oxr_legacy_pick_mode_index(k.modes, 2, i) == i);
		}
	}
	for (uint32_t i = 0; i < kStereoOnlyCount; i++) {
		INFO("stereo-only mode " << i);
		CHECK(oxr_legacy_pick_mode_index(kStereoOnly, kStereoOnlyCount, i) == i);
	}
}

TEST_CASE("the floor demotes an unfillable mode (#1510)", "[oxr][legacy_mode_rule]")
{
	SECTION("the Quad kind floors to 2D: {2D, Quad} has no 2-view 3D mode (ADR-065)")
	{
		CHECK(oxr_legacy_pick_mode_index(kQuadTable, 2, 1) == 0);
		// ...and mode 0 IS fillable, so the floor always terminates somewhere
		// the app can paint the whole canvas (mono, over a 1x1 grid).
		CHECK(oxr_legacy_mode_is_fillable(&kQuadTable[oxr_legacy_pick_mode_index(kQuadTable, 2, 1)]));
	}

	SECTION("on a longer (refused, non-conforming) table a 2-view 3D mode is still preferred")
	{
		const xrt_rendering_mode modes[] = {k2D, kAnaglyph, kQuad};
		CHECK(oxr_legacy_pick_mode_index(modes, 3, 2) == 1);
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

	SECTION("degenerate inputs are returned untouched rather than guessed at")
	{
		CHECK(oxr_legacy_pick_mode_index(nullptr, 0, 3) == 3);
		CHECK(oxr_legacy_pick_mode_index(kQuadTable, 0, 1) == 1);
		CHECK(oxr_legacy_pick_mode_index(kQuadTable, 2, 99) == 99);
	}
}

TEST_CASE("the compromise scale follows the mode the session will run in (#1510)", "[oxr][legacy_mode_rule]")
{
	SECTION("view_count 2, both scales <= 0.5 — Case A, 0.5x1.0 fills the 2x1 grid")
	{
		for (const xrt_rendering_mode *m : {&kAnaglyph, &kInterlaced, &kBlend}) {
			INFO(m->mode_name);
			vscale s{-1.0f, -1.0f};
			CHECK(oxr_legacy_compromise_scale(m, &s.x, &s.y));
			CHECK(s.x == Catch::Approx(0.5f));
			CHECK(s.y == Catch::Approx(1.0f));
		}
		// Cropped SBS is the same shape on a 1x2 grid and takes the same branch.
		vscale s{-1.0f, -1.0f};
		CHECK(oxr_legacy_compromise_scale(&kCropped, &s.x, &s.y));
		CHECK(s.x == Catch::Approx(0.5f));
		CHECK(s.y == Catch::Approx(1.0f));
	}

	SECTION("view_count 2 that is NOT SBS-shaped — Case B, the mode's own scale")
	{
		// Squeezed SBS is already 0.5x1.0: Case A would be a no-op, so the
		// rule must report Case B and hand back the mode's own numbers.
		vscale s{-1.0f, -1.0f};
		CHECK_FALSE(oxr_legacy_compromise_scale(&kSqueezed, &s.x, &s.y));
		CHECK(s.x == Catch::Approx(0.5f));
		CHECK(s.y == Catch::Approx(1.0f));
	}

	SECTION("view_count 1 — Case B, 1.0x1.0, the whole canvas mono")
	{
		vscale s{-1.0f, -1.0f};
		CHECK_FALSE(oxr_legacy_compromise_scale(&k2D, &s.x, &s.y));
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
		CHECK_FALSE(oxr_legacy_compromise_scale(&kQuad, &s.x, &s.y));
		CHECK(s.x == Catch::Approx(0.5f));
		CHECK(s.y == Catch::Approx(0.5f));
	}

	SECTION("end to end on the Quad kind: pick, then scale")
	{
		// OUTPUT=quad START_3D=1, unpinned — the floor takes the app to 2D,
		// and from 2D the 3D mode it would be toggled into floors to 2D too.
		const uint32_t picked = oxr_legacy_pick_mode_index(kQuadTable, 2, 1);
		CHECK(picked == 0);
		const uint32_t scale_idx = oxr_legacy_compromise_mode_index(kQuadTable, 2, picked, false, false);
		CHECK(scale_idx == 0);
		const vscale s = compromise_of(kQuadTable[scale_idx]);
		CHECK(s.x == Catch::Approx(1.0f));
		CHECK(s.y == Catch::Approx(1.0f));

		// OUTPUT=quad FORCE_MODE=1 — pinned, so no pick happens and Case B
		// stands. This is the DOCUMENTED loss, and the session WARN names it.
		CHECK(oxr_legacy_compromise_mode_index(kQuadTable, 2, 1, true, false) == 1);
		const vscale pinned = compromise_of(kQuadTable[1]);
		CHECK(pinned.x == Catch::Approx(0.5f));
		CHECK(pinned.y == Catch::Approx(0.5f));
		CHECK_FALSE(oxr_legacy_mode_is_fillable(&kQuadTable[1]));
	}

	SECTION("null out-params are refused rather than written through")
	{
		float x = 7.0f;
		CHECK_FALSE(oxr_legacy_compromise_scale(&kAnaglyph, &x, nullptr));
		CHECK_FALSE(oxr_legacy_compromise_scale(&kAnaglyph, nullptr, &x));
		CHECK_FALSE(oxr_legacy_compromise_scale(nullptr, &x, &x));
		CHECK(x == Catch::Approx(7.0f));
	}
}

TEST_CASE("a flat start sizes a legacy app for the 3D mode V toggles into (ADR-065)", "[oxr][legacy_mode_rule]")
{
	SECTION("sim-display's default: starts in 2D, kind anaglyph -> Case A, as before")
	{
		const uint32_t idx = oxr_legacy_compromise_mode_index(kAnaglyphTable, 2, 0, false, false);
		CHECK(idx == 1);
		const vscale s = compromise_of(kAnaglyphTable[idx]);
		CHECK(s.x == Catch::Approx(0.5f));
		CHECK(s.y == Catch::Approx(1.0f));
	}
	SECTION("started in the 3D mode: sized for it, exactly as before")
	{
		CHECK(oxr_legacy_compromise_mode_index(kAnaglyphTable, 2, 1, false, false) == 1);
		CHECK(oxr_legacy_compromise_mode_index(kAnaglyphTable, 2, 1, true, false) == 1);
		CHECK(oxr_legacy_compromise_mode_index(kAnaglyphTable, 2, 1, false, true) == 1);
	}
	SECTION("pinned in 2D (FORCE_MODE=0): it can never go 3D, so size for 2D")
	{
		CHECK(oxr_legacy_compromise_mode_index(kAnaglyphTable, 2, 0, true, false) == 0);
	}
	SECTION("service mode: the lease may move the panel to mode 1, unfloored")
	{
		CHECK(oxr_legacy_compromise_mode_index(kAnaglyphTable, 2, 0, false, true) == 1);
		CHECK(oxr_legacy_compromise_mode_index(kQuadTable, 2, 0, false, true) == 1);
	}
	SECTION("every stereo kind from a flat start sizes for its own 3D mode")
	{
		for (const kind_table &k : kSimKinds) {
			INFO("OUTPUT=" << k.output);
			const uint32_t idx = oxr_legacy_compromise_mode_index(k.modes, 2, 0, false, false);
			CHECK(idx == (k.modes[1].view_count <= 2 ? 1u : 0u));
		}
	}
	SECTION("degenerate inputs are returned untouched")
	{
		CHECK(oxr_legacy_compromise_mode_index(nullptr, 0, 0, false, false) == 0);
		CHECK(oxr_legacy_compromise_mode_index(kAnaglyphTable, 1, 0, false, false) == 0);
		CHECK(oxr_legacy_compromise_mode_index(kAnaglyphTable, 2, 5, false, false) == 5);
	}
}

/*
 * #1499 turned the three decisions above into functions of "how many views can
 * this session submit", and re-expressed the legacy ones as wrappers binding
 * that to OXR_LEGACY_MAX_SUBMITTED_VIEWS. This arm is the proof that the
 * generalisation took NOTHING away: index by index, on every shipping mode
 * table, the legacy answer is exactly the general answer at max_views = 2.
 *
 * It lives here rather than in tests_oxr_mode_fillable_rule.cpp on purpose —
 * this is the suite that owns #1510's behaviour, so this is where a future
 * change to the general rule has to come and explain itself.
 */
TEST_CASE("the legacy rule is the general rule at max_views = 2 (#1499 regression)", "[oxr][legacy_mode_rule]")
{
	struct table
	{
		const char *name;
		const xrt_rendering_mode *modes;
		uint32_t count;
	};
	std::vector<table> tables;
	for (const kind_table &k : kSimKinds) {
		tables.push_back({k.output, k.modes, 2});
	}
	tables.push_back({"stereo-only", kStereoOnly, kStereoOnlyCount});

	for (const table &t : tables) {
		for (uint32_t i = 0; i < t.count; i++) {
			INFO(t.name << " mode " << i);
			CHECK(oxr_legacy_mode_is_fillable(&t.modes[i]) ==
			      oxr_mode_fillable_by(&t.modes[i], OXR_LEGACY_MAX_SUBMITTED_VIEWS));
			CHECK(oxr_legacy_pick_mode_index(t.modes, t.count, i) ==
			      oxr_pick_fillable_mode_index(t.modes, t.count, i, OXR_LEGACY_MAX_SUBMITTED_VIEWS));
		}
	}

	// The degenerate inputs too, since those are where a wrapper is easiest to
	// get subtly wrong.
	CHECK(oxr_legacy_mode_is_fillable(nullptr) == oxr_mode_fillable_by(nullptr, OXR_LEGACY_MAX_SUBMITTED_VIEWS));
	CHECK(oxr_legacy_pick_mode_index(nullptr, 0, 3) ==
	      oxr_pick_fillable_mode_index(nullptr, 0, 3, OXR_LEGACY_MAX_SUBMITTED_VIEWS));
	CHECK(oxr_legacy_pick_mode_index(kQuadTable, 2, 99) ==
	      oxr_pick_fillable_mode_index(kQuadTable, 2, 99, OXR_LEGACY_MAX_SUBMITTED_VIEWS));

	// And the demotion gate, whose legacy name is now a pure alias.
	for (int pinned = 0; pinned < 2; pinned++) {
		for (int service = 0; service < 2; service++) {
			INFO("pinned=" << pinned << " service=" << service);
			CHECK(oxr_legacy_may_demote(pinned != 0, service != 0) ==
			      oxr_may_demote(pinned != 0, service != 0));
		}
	}

	// OXR_LEGACY_MAX_SUBMITTED_VIEWS is what makes the two identical; if it
	// ever moved, the equalities above would still hold while #1510's
	// documented behaviour silently changed. Pin the constant itself.
	CHECK(OXR_LEGACY_MAX_SUBMITTED_VIEWS == 2u);
}
