// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief comp_workspace_screens (ADR-052, the workspace on N screens): which
 *        screens present, the anchor-screen placement mapping, the window ->
 *        presenter split and the workspace-pixel cursor space.
 */

#include "util/comp_workspace_screens.h"

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const uint64_t PANEL = 0x1001; // the system-default 3D panel
const uint64_t SIDE = 0x2002;  // a second 3D panel to its right
const uint64_t FLAT = 0x3003;  // an ordinary 2D monitor

// XRT_DISPLAY_CLAIM_* (kept local: the test needs no plug-in header).
const uint32_t CLAIM_FALLBACK = 10;
const uint32_t CLAIM_EDID = 50;

xrt_screen
screen(uint64_t id, int32_t x, int32_t y, uint32_t w, uint32_t h, float wm, float hm, const char *plugin, uint32_t conf)
{
	xrt_screen s{};
	s.id = id;
	s.desktop_left = x;
	s.desktop_top = y;
	s.desktop_width = w;
	s.desktop_height = h;
	s.native_width = w;
	s.native_height = h;
	s.info.width_m = wm;
	s.info.height_m = hm;
	s.info.nominal_viewer_z_m = 0.6f;
	s.confidence = conf;
	std::snprintf(s.plugin_id, sizeof(s.plugin_id), "%s", plugin);
	return s;
}

/*
 * The rig: a 3840x2160 panel (0.60 x 0.3375 m: 156.25 um/px) at the origin,
 * a 1920x1080 panel (0.40 x 0.225 m: 208.3 um/px) to its right, and a plain
 * 2D monitor above the panel.
 */
xrt_screen_list
rig()
{
	xrt_screen_list l{};
	l.screens[0] = screen(PANEL, 0, 0, 3840, 2160, 0.60f, 0.3375f, "vendor-a", CLAIM_EDID);
	l.screens[0].flags = XRT_SCREEN_FLAG_SYSTEM_DEFAULT | XRT_SCREEN_FLAG_PRIMARY | XRT_SCREEN_FLAG_TRACKED;
	l.screens[1] = screen(SIDE, 3840, 0, 1920, 1080, 0.40f, 0.225f, "vendor-b", CLAIM_EDID);
	l.screens[2] = screen(FLAT, 0, -1080, 1920, 1080, 0.0f, 0.0f, "", 0);
	l.count = 3;
	return l;
}

const uint32_t ALL_DP = 0x7;

bool
approx(float a, float b, float eps = 1e-4f)
{
	return std::fabs(a - b) <= eps;
}

} // namespace

TEST_CASE("comp_ws: the screen set and who presents")
{
	xrt_screen_list l = rig();
	comp_ws_screen_set set{};

	SECTION("claimed: the primary and every claimed 3D screen present; a 2D monitor is listed, never presents")
	{
		REQUIRE(comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set) == 3);
		CHECK(set.primary_index == 0);
		CHECK(set.screens[0].flags ==
		      (COMP_WS_SCREEN_PRIMARY | COMP_WS_SCREEN_PRESENTER | COMP_WS_SCREEN_3D | COMP_WS_SCREEN_TRACKED));
		CHECK(set.screens[1].flags == (COMP_WS_SCREEN_PRESENTER | COMP_WS_SCREEN_3D));
		CHECK(set.screens[2].flags == 0u);
		CHECK(comp_ws_screens_presenter_count(&set) == 2);
	}

	SECTION("single (DXR_SEGMENTS=0): only the primary presents; the other 3D screen is still listed as 3D")
	{
		comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_SINGLE, &set);
		CHECK(comp_ws_screens_presenter_count(&set) == 1);
		CHECK((set.screens[1].flags & COMP_WS_SCREEN_3D) != 0);
		CHECK((set.screens[1].flags & COMP_WS_SCREEN_PRESENTER) == 0);
	}

	SECTION("a fallback claim is not a 3D screen unless the development policy asks for it")
	{
		l.screens[1].confidence = CLAIM_FALLBACK;
		comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set);
		CHECK((set.screens[1].flags & COMP_WS_SCREEN_3D) == 0);
		CHECK(comp_ws_screens_presenter_count(&set) == 1);
		comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED_INCL_FALLBACK, &set);
		CHECK(comp_ws_screens_presenter_count(&set) == 2);
	}

	SECTION("a claim that cannot make a display processor for the screen does not present")
	{
		comp_ws_screens_build(&l, 0x1 /* only the panel */, COMP_WS_POLICY_CLAIMED, &set);
		CHECK((set.screens[1].flags & COMP_WS_SCREEN_3D) == 0);
		CHECK(comp_ws_screens_presenter_count(&set) == 1);
	}

	SECTION("a screen with no physical size borrows the primary's pixel pitch")
	{
		comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set);
		CHECK(approx(set.screens[2].width_m, 0.30f));     // 1920 px * 156.25 um
		CHECK(approx(set.screens[2].height_m, 0.16875f)); // 1080 px * 156.25 um
	}

	SECTION("no system-default flag: the first screen is the primary")
	{
		l.screens[0].flags = 0;
		comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set);
		CHECK(set.primary_index == 0);
		CHECK((set.screens[0].flags & COMP_WS_SCREEN_PRIMARY) != 0);
	}

	SECTION("lookup: id 0 is the primary, an unknown id is absent")
	{
		comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set);
		CHECK(comp_ws_screens_find(&set, 0) == &set.screens[0]);
		CHECK(comp_ws_screens_find(&set, SIDE) == &set.screens[1]);
		CHECK(comp_ws_screens_find(&set, 0xdead) == nullptr);
	}
}

TEST_CASE("comp_ws: a topology change is a change, a mode change is not")
{
	xrt_screen_list l = rig();
	comp_ws_screen_set a{}, b{};
	comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &a);
	b = a;
	CHECK(comp_ws_screens_equal(&a, &b));
	b.screens[0].mode_index = 3;
	b.screens[0].hardware_3d = true;
	CHECK(comp_ws_screens_equal(&a, &b));

	xrt_screen_list moved = rig();
	moved.screens[1].desktop_top = 200;
	comp_ws_screens_build(&moved, ALL_DP, COMP_WS_POLICY_CLAIMED, &b);
	CHECK_FALSE(comp_ws_screens_equal(&a, &b));

	xrt_screen_list reclaimed = rig();
	std::snprintf(reclaimed.screens[1].plugin_id, sizeof(reclaimed.screens[1].plugin_id), "vendor-c");
	comp_ws_screens_build(&reclaimed, ALL_DP, COMP_WS_POLICY_CLAIMED, &b);
	CHECK_FALSE(comp_ws_screens_equal(&a, &b));

	xrt_screen_list unplugged = rig();
	unplugged.count = 2;
	comp_ws_screens_build(&unplugged, ALL_DP, COMP_WS_POLICY_CLAIMED, &b);
	CHECK_FALSE(comp_ws_screens_equal(&a, &b));
}

TEST_CASE("comp_ws: placement mapping goes through desktop pixels")
{
	xrt_screen_list l = rig();
	comp_ws_screen_set set{};
	comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set);
	const comp_ws_screen *panel = &set.screens[0];
	const comp_ws_screen *side = &set.screens[1];

	xrt_pose p{};
	p.orientation.w = 1.0f;

	SECTION("same screen: an exact copy")
	{
		p.position = {0.1f, -0.05f, 0.02f};
		xrt_pose out{};
		float w = 0, h = 0;
		REQUIRE(comp_ws_map_pose(panel, panel, &p, 0.3f, 0.2f, &out, &w, &h));
		CHECK(std::memcmp(&out, &p, sizeof(p)) == 0);
		CHECK(w == 0.3f);
		CHECK(h == 0.2f);
	}

	SECTION("centre of the side panel, placed in the side panel's space, lands right of the main panel")
	{
		// Side panel centre = desktop (4800, 540). In the panel's space:
		// x = (4800 - 1920) * 156.25 um = 0.45 m, y = (1080 - 540) * 156.25 um = 0.084375 m.
		xrt_pose out{};
		float w = 0, h = 0;
		REQUIRE(comp_ws_map_pose(side, panel, &p, 0.20f, 0.10f, &out, &w, &h));
		CHECK(approx(out.position.x, 0.45f));
		CHECK(approx(out.position.y, 0.084375f));
		// 0.20 m on the side panel = 960 px = 0.15 m on the main panel.
		CHECK(approx(w, 0.15f));
		CHECK(approx(h, 0.075f));
	}

	SECTION("z and orientation are not touched; the round trip is the identity")
	{
		p.position = {-0.031f, 0.017f, 0.04f};
		p.orientation = {0.0f, 0.2588190f, 0.0f, 0.9659258f}; // 30 deg yaw
		xrt_pose mid{}, back{};
		float w1, h1, w2, h2;
		REQUIRE(comp_ws_map_pose(side, panel, &p, 0.12f, 0.08f, &mid, &w1, &h1));
		CHECK(mid.position.z == p.position.z);
		CHECK(std::memcmp(&mid.orientation, &p.orientation, sizeof(p.orientation)) == 0);
		REQUIRE(comp_ws_map_pose(panel, side, &mid, w1, h1, &back, &w2, &h2));
		CHECK(approx(back.position.x, p.position.x, 1e-5f));
		CHECK(approx(back.position.y, p.position.y, 1e-5f));
		CHECK(approx(w2, 0.12f, 1e-6f));
		CHECK(approx(h2, 0.08f, 1e-6f));
	}

	SECTION("a straddling window has the same desktop footprint whichever screen describes it")
	{
		// 1200 px wide centred on the seam (desktop x 3240..4440), rows 400..1000.
		xrt_pose in_panel{};
		in_panel.orientation.w = 1.0f;
		in_panel.position.x = (3840.0f - 1920.0f) * 0.00015625f; // seam
		in_panel.position.y = (1080.0f - 700.0f) * 0.00015625f;
		const float w = 1200 * 0.00015625f;
		const float h = 600 * 0.00015625f;
		comp_seg_rect fa{}, fb{};
		REQUIRE(comp_ws_footprint(panel, &in_panel, w, h, &fa));
		CHECK(fa.x == 3240);
		CHECK(fa.y == 400);
		CHECK(fa.w == 1200);
		CHECK(fa.h == 600);

		xrt_pose in_side{};
		float ws, hs;
		REQUIRE(comp_ws_map_pose(panel, side, &in_panel, w, h, &in_side, &ws, &hs));
		REQUIRE(comp_ws_footprint(side, &in_side, ws, hs, &fb));
		CHECK(fb.x == fa.x);
		CHECK(fb.y == fa.y);
		CHECK(fb.w == fa.w);
		CHECK(fb.h == fa.h);
	}

	SECTION("a window rotated 90 degrees in the screen plane swaps its footprint's sides")
	{
		p.position = {0, 0, 0};
		p.orientation = {0.0f, 0.0f, 0.7071068f, 0.7071068f};
		comp_seg_rect f{};
		REQUIRE(comp_ws_footprint(panel, &p, 0.30f, 0.15f, &f)); // 1920 x 960 px unrotated
		CHECK(f.w == 960);
		CHECK(f.h == 1920);
		CHECK(f.x == 1920 - 480);
		CHECK(f.y == 1080 - 960);
	}

	SECTION("no usable pitch: refuse instead of guessing")
	{
		comp_ws_screen bad = *side;
		bad.width_m = 0.0f;
		xrt_pose out{};
		float w, h;
		CHECK_FALSE(comp_ws_map_pose(&bad, panel, &p, 0.1f, 0.1f, &out, &w, &h));
		comp_seg_rect f{};
		CHECK_FALSE(comp_ws_footprint(&bad, &p, 0.1f, 0.1f, &f));
		CHECK_FALSE(comp_ws_footprint(panel, &p, 0.0f, 0.1f, &f));
	}
}

TEST_CASE("comp_ws: window -> presenter split")
{
	xrt_screen_list l = rig();
	comp_ws_screen_set set{};
	comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set);
	comp_ws_part parts[COMP_SEGMENTS_MAX];

	SECTION("a window on the panel alone: one part, the whole window")
	{
		const comp_seg_rect f = {100, 100, 800, 600};
		REQUIRE(comp_ws_split(&set, &f, parts) == 1);
		CHECK(parts[0].screen_id == PANEL);
		CHECK(parts[0].is_primary);
		CHECK(parts[0].window_rect.x == 0);
		CHECK(parts[0].window_rect.w == 800);
		CHECK(parts[0].screen_rect.x == 100);
	}

	SECTION("straddling the seam: two parts, left to right, cut at the seam")
	{
		const comp_seg_rect f = {3240, 400, 1200, 600};
		REQUIRE(comp_ws_split(&set, &f, parts) == 2);
		CHECK(parts[0].screen_id == PANEL);
		CHECK(parts[0].window_rect.x == 0);
		CHECK(parts[0].window_rect.w == 600);
		CHECK(parts[0].screen_rect.x == 3240);
		CHECK(parts[1].screen_id == SIDE);
		CHECK_FALSE(parts[1].is_primary);
		CHECK(parts[1].window_rect.x == 600);
		CHECK(parts[1].window_rect.w == 600);
		CHECK(parts[1].screen_rect.x == 0);
		CHECK(parts[1].screen_rect.y == 400);
	}

	SECTION("the part over a 2D monitor is not drawn by anyone (D4)")
	{
		const comp_seg_rect f = {100, -300, 800, 600}; // half on FLAT, half on the panel
		REQUIRE(comp_ws_split(&set, &f, parts) == 1);
		CHECK(parts[0].screen_id == PANEL);
		CHECK(parts[0].window_rect.y == 300);
		CHECK(parts[0].window_rect.h == 300);
	}

	SECTION("single-screen workspace: a straddler is cut to the primary")
	{
		comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_SINGLE, &set);
		const comp_seg_rect f = {3240, 400, 1200, 600};
		REQUIRE(comp_ws_split(&set, &f, parts) == 1);
		CHECK(parts[0].screen_id == PANEL);
		CHECK(parts[0].window_rect.w == 600);
	}

	SECTION("a window on no presenter: no parts")
	{
		const comp_seg_rect f = {-5000, -5000, 100, 100};
		CHECK(comp_ws_split(&set, &f, parts) == 0);
	}
}

TEST_CASE("comp_ws: workspace pixels and the screen under the cursor")
{
	// The 3D panel is NOT at the desktop origin here: a 2D monitor sits left of it.
	xrt_screen_list l{};
	l.screens[0] = screen(PANEL, 1920, 0, 3840, 2160, 0.60f, 0.3375f, "vendor-a", CLAIM_EDID);
	l.screens[0].flags = XRT_SCREEN_FLAG_SYSTEM_DEFAULT;
	l.screens[1] = screen(FLAT, 0, 0, 1920, 1080, 0.5f, 0.28f, "", 0);
	l.screens[2] = screen(SIDE, 5760, 0, 1920, 1080, 0.40f, 0.225f, "vendor-b", CLAIM_EDID);
	l.count = 3;
	comp_ws_screen_set set{};
	comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set);

	int32_t wx, wy, lx, ly;
	comp_ws_desktop_to_workspace(&set, 1920 + 10, 20, &wx, &wy);
	CHECK(wx == 10); // one presenter's worth: the service window's client px
	CHECK(wy == 20);

	CHECK(comp_ws_screen_at(&set, 10, 20, &lx, &ly) == 0);
	CHECK(lx == 10);
	CHECK(ly == 20);
	// Left of the panel: negative workspace x, the 2D monitor.
	CHECK(comp_ws_screen_at(&set, -100, 50, &lx, &ly) == 1);
	CHECK(lx == 1820);
	// The seam column belongs to the right-hand screen (half-open).
	CHECK(comp_ws_screen_at(&set, 3840, 0, &lx, &ly) == 2);
	CHECK(lx == 0);
	CHECK(comp_ws_screen_at(&set, 3839, 0, &lx, &ly) == 0);
	// Below the side panel: nowhere.
	CHECK(comp_ws_screen_at(&set, 3900, 1500, &lx, &ly) == -1);
}

TEST_CASE("comp_ws: policy from the environment")
{
	CHECK(comp_ws_policy_from_env(nullptr, nullptr) == COMP_WS_POLICY_CLAIMED);
	CHECK(comp_ws_policy_from_env("1", nullptr) == COMP_WS_POLICY_CLAIMED);
	CHECK(comp_ws_policy_from_env("0", nullptr) == COMP_WS_POLICY_SINGLE);
	CHECK(comp_ws_policy_from_env("0", "fallback") == COMP_WS_POLICY_SINGLE); // the kill switch wins
	CHECK(comp_ws_policy_from_env(nullptr, "fallback") == COMP_WS_POLICY_CLAIMED_INCL_FALLBACK);
	CHECK(comp_ws_policy_from_env(nullptr, "single") == COMP_WS_POLICY_SINGLE);
}
