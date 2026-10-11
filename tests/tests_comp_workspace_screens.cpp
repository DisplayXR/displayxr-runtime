// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief comp_workspace_screens (ADR-052, the workspace on N screens): which
 *        screens present, the anchor-screen placement mapping, the window ->
 *        presenter split and the workspace-pixel cursor space.
 */

#include "util/comp_workspace_screens.h"

#include "oxr_segment_views.h" // the client side of a per-segment table (ADR-052 D2)

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

TEST_CASE("comp_ws: a secondary presenter's compose pass (two synthetic screens)")
{
	xrt_screen_list l = rig();
	comp_ws_screen_set set{};
	comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set);
	const comp_ws_screen *panel = &set.screens[0];
	const comp_ws_screen *side = &set.screens[1];

	comp_ws_frame f{};
	REQUIRE(comp_ws_presenter_frame(panel, side, &f));
	// The panel's centre (desktop 1920,1080) is 1920 px LEFT of the side panel's origin.
	CHECK(approx(f.cx_px, -1920.0f));
	CHECK(approx(f.cy_px, 1080.0f));
	CHECK(approx(f.ppm_x, 6400.0f, 1e-2f)); // 1 / 156.25 um
	CHECK(approx(f.ppm_y, 6400.0f, 1e-2f));

	SECTION("the primary's own frame is the identity: centre of its panel, its pitch")
	{
		comp_ws_frame g{};
		REQUIRE(comp_ws_presenter_frame(panel, panel, &g));
		CHECK(approx(g.cx_px, 1920.0f));
		CHECK(approx(g.cy_px, 1080.0f));
	}

	SECTION("drawing in the primary's space through the frame == mapping, then the presenter's own frame")
	{
		const float pts[][2] = {
		    {0.0f, 0.0f}, {0.45f, 0.084375f}, {0.31f, -0.02f}, {0.6f, 0.15f}, {-0.1f, 0.3f}};
		for (const auto &pt : pts) {
			xrt_pose a{};
			a.orientation.w = 1.0f;
			a.position.x = pt[0];
			a.position.y = pt[1];
			xrt_pose b{};
			float wb, hb;
			REQUIRE(comp_ws_map_pose(panel, side, &a, 0.1f, 0.1f, &b, &wb, &hb));
			const float own_x = side->desktop.w * 0.5f + b.position.x * (side->desktop.w / side->width_m);
			const float own_y = side->desktop.h * 0.5f - b.position.y * (side->desktop.h / side->height_m);
			CHECK(approx(f.cx_px + pt[0] * f.ppm_x, own_x, 1e-2f));
			CHECK(approx(f.cy_px - pt[1] * f.ppm_y, own_y, 1e-2f));
		}
	}

	SECTION("a window straddling the seam: the presenter draws exactly the split's part")
	{
		// Centred on the seam, 1200 x 600 px in the panel's space (as in the
		// footprint test): footprint desktop x 3240..4440.
		const float pitch = 0.00015625f;
		const float wx = (3840.0f - 1920.0f) * pitch;
		const float ww = 1200 * pitch;
		const float left_px = f.cx_px + (wx - ww * 0.5f) * f.ppm_x;
		const float right_px = f.cx_px + (wx + ww * 0.5f) * f.ppm_x;
		CHECK(approx(left_px, -600.0f, 1e-2f)); // hangs 600 px off the side panel's left edge
		CHECK(approx(right_px, 600.0f, 1e-2f));

		const comp_seg_rect fp = {3240, 400, 1200, 600};
		comp_ws_part parts[COMP_SEGMENTS_MAX];
		REQUIRE(comp_ws_split(&set, &fp, parts) == 2);
		CHECK(parts[1].screen_id == SIDE);
		CHECK(parts[1].screen_rect.x == 0);
		CHECK(approx((float)(parts[1].screen_rect.x + (int32_t)parts[1].screen_rect.w), right_px, 1e-2f));
	}

	SECTION("the side panel's viewer, mapped into the primary's space for the pass")
	{
		xrt_pose eye{};
		eye.orientation.w = 1.0f;
		eye.position = {0.0f, 0.0f, 0.6f}; // nominal, side panel's own space
		xrt_pose in_panel{};
		float w, h;
		REQUIRE(comp_ws_map_pose(side, panel, &eye, 1.0f, 1.0f, &in_panel, &w, &h));
		CHECK(approx(in_panel.position.x, 0.45f));
		CHECK(approx(in_panel.position.y, 0.084375f));
		CHECK(in_panel.position.z == 0.6f);
	}

	SECTION("no pitch on the primary: no frame")
	{
		comp_ws_screen bad = *panel;
		bad.width_m = 0.0f;
		CHECK_FALSE(comp_ws_presenter_frame(&bad, side, &f));
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

/*
 * ADR-052 D2 — per-screen views. The rig: PANEL (3840x2160, 156.25 um/px) at
 * the origin, SIDE (1920x1080, 208.3 um/px) to its right at desktop x 3840.
 * Every pose is in the PRIMARY's display space (where a slot's pose lives).
 */
namespace {

const float PITCH = 0.00015625f; // the panel's metres per pixel

//! The primary-space pose of a window whose footprint centre is desktop (cx, cy).
xrt_pose
pose_at(float cx, float cy, float z = 0.0f)
{
	xrt_pose p{};
	p.orientation.w = 1.0f;
	p.position.x = (cx - 1920.0f) * PITCH;
	p.position.y = (1080.0f - cy) * PITCH;
	p.position.z = z;
	return p;
}

} // namespace

TEST_CASE("comp_ws: per-screen views — which screen frames a window")
{
	xrt_screen_list l = rig();
	comp_ws_screen_set set{};
	comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set);
	const comp_ws_screen *panel = &set.screens[0];
	const comp_ws_screen *side = &set.screens[1];
	comp_ws_view_framing f{};

	SECTION("single screen, unchanged: a window on the panel is framed from the panel, as before")
	{
		xrt_pose p = pose_at(1000.0f, 900.0f, 0.02f);
		p.orientation = {0.0f, 0.1305262f, 0.0f, 0.9914449f}; // 15 deg yaw: any pose, any tilt
		REQUIRE(comp_ws_frame_window(&set, &p, 0.1f, 0.05f, XRT_MAX_SEGMENTS, &f));
		CHECK(f.part_count == 1);
		CHECK(f.parts[0].screen_id == PANEL);
		CHECK_FALSE(f.off_primary);
		CHECK_FALSE(f.per_segment);
		CHECK(f.frame_screen_index == set.primary_index);

		// ... and the metrics from the panel are the single-screen path's: the
		// pose verbatim, the size verbatim, the panel as the display.
		xrt_window_metrics wm{};
		REQUIRE(comp_ws_window_metrics(&set, panel, &p, 0.1f, 0.05f, &wm));
		CHECK(wm.window_center_offset_x_m == p.position.x);
		CHECK(wm.window_center_offset_y_m == p.position.y);
		CHECK(wm.window_center_offset_z_m == p.position.z);
		CHECK(wm.window_width_m == 0.1f);
		CHECK(wm.window_height_m == 0.05f);
		CHECK(wm.window_orientation.y == p.orientation.y);
		CHECK(wm.display_width_m == 0.60f);
		CHECK(wm.display_pixel_width == 3840);
		CHECK(wm.window_pixel_width == 640);
		CHECK(wm.window_pixel_height == 320);
	}

	SECTION("a one-presenter workspace never frames off the primary, even over the side panel")
	{
		comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_SINGLE, &set);
		const xrt_pose p = pose_at(4800.0f, 540.0f);
		REQUIRE(comp_ws_frame_window(&set, &p, 0.1f, 0.05f, XRT_MAX_SEGMENTS, &f));
		CHECK(f.part_count == 0); // nothing presents there
		CHECK_FALSE(f.off_primary);
		CHECK(f.frame_screen_index == set.primary_index);
	}

	SECTION("wholly on screen 2: screen 2's eyes and framing")
	{
		// Centred on the side panel (desktop 4800, 540), 640 x 320 px.
		const xrt_pose p = pose_at(4800.0f, 540.0f, 0.03f);
		REQUIRE(comp_ws_frame_window(&set, &p, 640 * PITCH, 320 * PITCH, XRT_MAX_SEGMENTS, &f));
		REQUIRE(f.part_count == 1);
		CHECK(f.parts[0].screen_id == SIDE);
		CHECK(f.off_primary);
		CHECK_FALSE(f.per_segment); // one part: one view set, framed from SIDE
		REQUIRE(f.frame_screen_index < set.count);
		CHECK(set.screens[f.frame_screen_index].id == SIDE);

		xrt_window_metrics wm{};
		REQUIRE(comp_ws_window_metrics(&set, side, &p, 640 * PITCH, 320 * PITCH, &wm));
		// The side panel is the display: its size, its pixels, its origin.
		CHECK(wm.display_width_m == 0.40f);
		CHECK(wm.display_pixel_width == 1920);
		CHECK(wm.display_screen_left == 3840);
		// Centred on it, in ITS metres (208.3 um/px): 640 px = 0.1333 m.
		CHECK(approx(wm.window_center_offset_x_m, 0.0f));
		CHECK(approx(wm.window_center_offset_y_m, 0.0f));
		CHECK(wm.window_center_offset_z_m == 0.03f); // depth carries over
		CHECK(approx(wm.window_width_m, 640.0f * 0.40f / 1920.0f));
		CHECK(approx(wm.window_height_m, 320.0f * 0.225f / 1080.0f));
		CHECK(wm.window_pixel_width == 640);
		CHECK(wm.window_screen_left == 4800 - 320);
		CHECK(wm.window_screen_top == 540 - 160);

		// The primary framing it had before this fix was 0.45 m off-centre on
		// a panel the window is not on — the rig's wrong perspective.
		xrt_window_metrics old{};
		REQUIRE(comp_ws_window_metrics(&set, panel, &p, 640 * PITCH, 320 * PITCH, &old));
		CHECK(approx(old.window_center_offset_x_m, 0.45f));
	}

	SECTION("straddling, a tie: the primary is the majority; a per-segment table is allowed")
	{
		// Footprint 3240..4440 x 400..1000: 600 px on each side of the seam.
		const xrt_pose p = pose_at(3840.0f, 700.0f);
		REQUIRE(comp_ws_frame_window(&set, &p, 1200 * PITCH, 600 * PITCH, XRT_MAX_SEGMENTS, &f));
		REQUIRE(f.part_count == 2);
		CHECK(f.footprint.x == 3240);
		CHECK(f.footprint.w == 1200);
		CHECK(f.majority == 0);
		CHECK(f.parts[f.majority].screen_id == PANEL);
		CHECK_FALSE(f.off_primary);
		CHECK(f.per_segment);
	}

	SECTION("straddling, mostly on screen 2: the majority is screen 2")
	{
		const xrt_pose p = pose_at(3850.0f, 700.0f); // 590 px | 610 px
		REQUIRE(comp_ws_frame_window(&set, &p, 1200 * PITCH, 600 * PITCH, XRT_MAX_SEGMENTS, &f));
		REQUIRE(f.part_count == 2);
		CHECK(f.parts[0].window_rect.w == 590);
		CHECK(f.parts[1].window_rect.w == 610);
		CHECK(f.parts[f.majority].screen_id == SIDE);
		CHECK(f.off_primary);
		CHECK(f.per_segment);
	}

	SECTION("a tilted straddler keeps one view set (the footprint is not the window)")
	{
		xrt_pose p = pose_at(3850.0f, 700.0f);
		p.orientation = {0.0f, 0.1305262f, 0.0f, 0.9914449f};
		REQUIRE(comp_ws_frame_window(&set, &p, 1200 * PITCH, 600 * PITCH, XRT_MAX_SEGMENTS, &f));
		CHECK(f.part_count == 2);
		CHECK_FALSE(f.per_segment);
	}

	SECTION("more parts than view sets: one view set")
	{
		const xrt_pose p = pose_at(3850.0f, 700.0f);
		REQUIRE(comp_ws_frame_window(&set, &p, 1200 * PITCH, 600 * PITCH, 1, &f));
		CHECK(f.part_count == 2);
		CHECK_FALSE(f.per_segment);
	}
}

TEST_CASE("comp_ws: per-screen views — the straddler's per-segment table")
{
	xrt_screen_list l = rig();
	l.screens[1].info.nominal_viewer_y_m = 0.05f; // SIDE's own viewer, distinct from the panel's
	l.screens[1].info.nominal_viewer_z_m = 0.45f;
	comp_ws_screen_set set{};
	comp_ws_screens_build(&l, ALL_DP, COMP_WS_POLICY_CLAIMED, &set);
	const comp_ws_screen *side = &set.screens[1];
	comp_ws_view_framing f{};

	const xrt_pose p = pose_at(3850.0f, 700.0f, 0.04f); // 590 | 610, 4 cm in front
	REQUIRE(comp_ws_frame_window(&set, &p, 1200 * PITCH, 600 * PITCH, XRT_MAX_SEGMENTS, &f));
	xrt_segment_metrics m{};
	REQUIRE(comp_ws_segment_table(&set, &f, p.position.z, &m) == 2);

	SECTION("one segment per part, in the shape the state tracker reads")
	{
		CHECK(m.canvas.offset.w == 0);
		CHECK(m.canvas.extent.w == 1200);
		CHECK(m.canvas.extent.h == 600);
		CHECK(m.window_screen_left == 3250);
		CHECK(m.window_screen_top == 400);
		CHECK(m.window_pixel_width == 1200);
		CHECK(m.window_depth_m == 0.04f);

		CHECK(m.seg[0].screen_id == PANEL);
		CHECK(m.seg[0].is_primary);
		CHECK(m.seg[0].window_rect.offset.w == 0);
		CHECK(m.seg[0].window_rect.extent.w == 590);
		CHECK(m.seg[0].screen_rect.offset.w == 3250);
		CHECK(m.seg[0].screen_desktop_width == 3840);
		CHECK(m.seg[0].screen_width_m == 0.60f);

		CHECK(m.seg[1].screen_id == SIDE);
		CHECK_FALSE(m.seg[1].is_primary);
		CHECK(m.seg[1].has_dp);
		CHECK(m.seg[1].window_rect.offset.w == 590);
		CHECK(m.seg[1].window_rect.extent.w == 610);
		CHECK(m.seg[1].screen_rect.offset.w == 0);
		CHECK(m.seg[1].screen_rect.offset.h == 400);
		CHECK(m.seg[1].screen_desktop_left == 3840);
		CHECK(m.seg[1].screen_width_m == 0.40f);
		CHECK(m.seg[1].nominal_viewer_y_m == 0.05f); // SIDE's viewer, not the panel's
		CHECK(m.seg[1].nominal_viewer_z_m == 0.45f);
		CHECK_FALSE(m.seg[1].have_eyes); // the service fills them from the presenter DP
	}

	SECTION("the state tracker's per-segment canvases: each part on its own screen, at the window's depth")
	{
		oxr_segment_layout lay{};
		REQUIRE(oxr_segment_views_layout(&m, &lay));
		CHECK(lay.majority == 1); // SIDE holds most of it

		// Segment 1 = the part on SIDE: desktop 3840..4450 x 400..1000, i.e.
		// SIDE px 0..610 x 400..1000, centre (305, 700) — in SIDE's metres.
		xrt_window_metrics w1{};
		oxr_segment_views_window_metrics(&m, &lay, 1, &w1);
		REQUIRE(w1.valid);
		const float spx = 0.40f / 1920.0f;
		const float spy = 0.225f / 1080.0f;
		CHECK(approx(w1.window_center_offset_x_m, (305.0f - 960.0f) * spx));
		CHECK(approx(w1.window_center_offset_y_m, -(700.0f - 540.0f) * spy));
		CHECK(w1.window_center_offset_z_m == 0.04f);
		CHECK(approx(w1.window_width_m, 610.0f * spx));
		CHECK(w1.display_width_m == 0.40f);

		// Screen 2's tracked eyes reach segment 1 unshifted (it is the
		// reference frame); the panel's part is framed in the same frame.
		m.seg[1].eyes.count = 2;
		m.seg[1].eyes.valid = true;
		m.seg[1].eyes.eyes[0] = {-0.03f, 0.02f, 0.5f};
		m.seg[1].eyes.eyes[1] = {0.03f, 0.02f, 0.5f};
		m.seg[1].have_eyes = true;
		float dx = 0.0f, dy = 0.0f;
		oxr_segment_views_own_to_ref(&lay, 1, &dx, &dy);
		xrt_eye_positions e{};
		REQUIRE(oxr_segment_views_segment_eyes(&m.seg[1], dx, dy, 2, &e));
		CHECK(approx(e.eyes[0].x, -0.03f));
		CHECK(approx(e.eyes[1].z, 0.5f));
	}

	SECTION("a stereo client's one view set from the majority == the server's framing of that screen")
	{
		// The client reads the table (oxr_segment_views_whole_window_metrics on
		// the majority); a client that never asks for a table gets the server's
		// framing (comp_ws_window_metrics on the same screen). Same canvas.
		xrt_window_metrics client{};
		oxr_segment_views_whole_window_metrics(&m, 1, &client);
		xrt_window_metrics server{};
		REQUIRE(comp_ws_window_metrics(&set, side, &p, 1200 * PITCH, 600 * PITCH, &server));
		REQUIRE(client.valid);
		CHECK(approx(client.window_center_offset_x_m, server.window_center_offset_x_m));
		CHECK(approx(client.window_center_offset_y_m, server.window_center_offset_y_m));
		CHECK(client.window_center_offset_z_m == server.window_center_offset_z_m);
		CHECK(approx(client.window_width_m, server.window_width_m));
		CHECK(approx(client.window_height_m, server.window_height_m));
		CHECK(client.window_screen_left == server.window_screen_left);
		CHECK(client.window_screen_top == server.window_screen_top);
	}

	SECTION("no table for a window that is not per-segment")
	{
		comp_ws_view_framing one{};
		const xrt_pose q = pose_at(4800.0f, 540.0f);
		REQUIRE(comp_ws_frame_window(&set, &q, 640 * PITCH, 320 * PITCH, XRT_MAX_SEGMENTS, &one));
		xrt_segment_metrics none{};
		CHECK(comp_ws_segment_table(&set, &one, 0.0f, &none) == 0);
		CHECK(none.count == 0);
	}
}

TEST_CASE("comp_ws: the pointer capture goes to the presenter the press went down on")
{
	// Each presenter window has its own thread; Win32 capture only takes on
	// the window the button went down on. A title-bar drag that starts on a
	// secondary panel must grab THAT presenter, not the primary (ADR-052).
	comp_ws_pointer_owner o{};

	SECTION("before any press: the primary (the service window)")
	{
		CHECK(comp_ws_pointer_owner_screen(&o) == 0);
		CHECK(comp_ws_pointer_owner_screen(nullptr) == 0);
		comp_ws_pointer_owner_note_down(nullptr, SIDE, 5); // tolerated
	}

	SECTION("a press on the secondary panel makes it the owner, a later press on the primary takes it back")
	{
		comp_ws_pointer_owner_note_down(&o, SIDE, 1000);
		CHECK(comp_ws_pointer_owner_screen(&o) == SIDE);
		comp_ws_pointer_owner_note_down(&o, 0, 1016);
		CHECK(comp_ws_pointer_owner_screen(&o) == 0);
		comp_ws_pointer_owner_note_down(&o, SIDE, 2000);
		CHECK(comp_ws_pointer_owner_screen(&o) == SIDE);
	}

	SECTION("drain order: the primary's ring is drained first, an older secondary DOWN does not steal the owner")
	{
		comp_ws_pointer_owner_note_down(&o, 0, 5000);
		comp_ws_pointer_owner_note_down(&o, SIDE, 4990);
		CHECK(comp_ws_pointer_owner_screen(&o) == 0);
	}

	SECTION("same tick: the later-noted DOWN wins (the presenters drain after the primary)")
	{
		comp_ws_pointer_owner_note_down(&o, 0, 7000);
		comp_ws_pointer_owner_note_down(&o, SIDE, 7000);
		CHECK(comp_ws_pointer_owner_screen(&o) == SIDE);
	}

	SECTION("GetTickCount wrap: a press just after the wrap is newer")
	{
		comp_ws_pointer_owner_note_down(&o, 0, 0xFFFFFFF0u);
		comp_ws_pointer_owner_note_down(&o, SIDE, 0x00000010u);
		CHECK(comp_ws_pointer_owner_screen(&o) == SIDE);
		comp_ws_pointer_owner_note_down(&o, 0, 0xFFFFFFF8u); // pre-wrap = older
		CHECK(comp_ws_pointer_owner_screen(&o) == SIDE);
	}
}
