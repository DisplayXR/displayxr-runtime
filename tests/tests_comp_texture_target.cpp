// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief comp_texture_target (ADR-060, XR_DXR_win32_window_binding v9): the
 *        one resolve of (reference window, rect) that feeds phase, window
 *        metrics and segments.
 */

#include "util/comp_texture_target.h"

#include "catch_amalgamated.hpp"

namespace {

comp_texture_target
bound(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	comp_texture_target tt{};
	tt.bound = true;
	tt.x = x;
	tt.y = y;
	tt.w = w;
	tt.h = h;
	return tt;
}

} // namespace

TEST_CASE("texture target: unbound or empty never resolves")
{
	comp_texture_target_resolved r{};
	comp_texture_target tt{};
	CHECK_FALSE(comp_texture_target_compose(&tt, 0, 0, 3840, 2160, &r));
	CHECK_FALSE(r.valid);

	tt = bound(0, 0, 0, 100);
	CHECK_FALSE(comp_texture_target_compose(&tt, 0, 0, 3840, 2160, &r));
	CHECK_FALSE(comp_texture_target_compose(nullptr, 0, 0, 3840, 2160, &r));
}

TEST_CASE("texture target: the rect is offset by the reference window's client origin")
{
	// An editor viewport at (300,120) inside a window whose client area sits at (916,72).
	comp_texture_target tt = bound(300, 120, 1280, 720);
	comp_texture_target_resolved r{};
	REQUIRE(comp_texture_target_compose(&tt, 916, 72, 3840, 2160, &r));
	CHECK(r.valid);
	CHECK_FALSE(r.clamped);
	CHECK(r.screen.x == 1216);
	CHECK(r.screen.y == 192);
	CHECK(r.screen.w == 1280);
	CHECK(r.screen.h == 720);
}

TEST_CASE("texture target: a NULL reference means desktop px (origin 0,0)")
{
	comp_texture_target tt = bound(-1920, 40, 800, 600); // on a monitor left of the primary
	comp_texture_target_resolved r{};
	REQUIRE(comp_texture_target_compose(&tt, 0, 0, 3840, 2160, &r));
	CHECK(r.screen.x == -1920);
	CHECK(r.screen.y == 40);
}

TEST_CASE("texture target: a rect larger than the shared texture is clamped and reported")
{
	comp_texture_target tt = bound(0, 0, 4000, 2000);
	comp_texture_target_resolved r{};
	REQUIRE(comp_texture_target_compose(&tt, 0, 0, 3840, 2160, &r));
	CHECK(r.clamped);
	CHECK(r.screen.w == 3840);
	CHECK(r.screen.h == 2000);

	// Unknown texture size: no clamp.
	REQUIRE(comp_texture_target_compose(&tt, 0, 0, 0, 0, &r));
	CHECK_FALSE(r.clamped);
	CHECK(r.screen.w == 4000);
}

TEST_CASE("texture target: present origin is relative to the weaving screen")
{
	comp_texture_target tt = bound(10, 20, 640, 480);
	comp_texture_target_resolved r{};
	REQUIRE(comp_texture_target_compose(&tt, 316, 200, 3840, 2160, &r));
	int32_t ox = 0, oy = 0;
	// Panel at the desktop origin: the origin is the target's desktop origin.
	comp_texture_target_present_origin(&r, 0, 0, &ox, &oy);
	CHECK(ox == 326);
	CHECK(oy == 220);
	// Panel to the right of a 2560-wide primary.
	comp_texture_target_present_origin(&r, 2560, 0, &ox, &oy);
	CHECK(ox == 326 - 2560);
	CHECK(oy == 220);
	// A +1 px move of the window moves the origin by exactly +1 (the phase unit).
	comp_texture_target_resolved r2{};
	REQUIRE(comp_texture_target_compose(&tt, 317, 200, 3840, 2160, &r2));
	int32_t ox2 = 0, oy2 = 0;
	comp_texture_target_present_origin(&r2, 0, 0, &ox2, &oy2);
	CHECK(ox2 - 326 == 1);
	CHECK(oy2 == 220);
}

TEST_CASE("texture target: anchor fallback weaves the canvas at the target's offset in the anchor")
{
	// A DCC viewport at (113,67) inside the anchor window whose client area sits at (26,82).
	comp_texture_target tt = bound(113, 67, 2636, 1034);
	comp_texture_target_resolved r{};
	REQUIRE(comp_texture_target_compose(&tt, 26, 82, 3840, 2160, &r));
	int32_t ox = -1, oy = -1;
	REQUIRE(comp_texture_target_anchor_offset(&r, 26, 82, 0, 0, 16384, &ox, &oy));
	CHECK(ox == 113);
	CHECK(oy == 67);

	// The reference differs from the anchor: the offset is relative to the anchor.
	REQUIRE(comp_texture_target_anchor_offset(&r, 0, 0, 0, 0, 16384, &ox, &oy));
	CHECK(ox == 139);
	CHECK(oy == 149);

	// Nothing to correct when the canvas starts at the anchor's client origin.
	CHECK_FALSE(comp_texture_target_anchor_offset(&r, 139, 149, 0, 0, 16384, &ox, &oy));
	CHECK(ox == 0);
	CHECK(oy == 0);
	// Left of / above the anchor's client area: no render target pixel can hold it.
	CHECK_FALSE(comp_texture_target_anchor_offset(&r, 140, 0, 0, 0, 16384, &ox, &oy));
	CHECK_FALSE(comp_texture_target_anchor_offset(&r, 0, 150, 0, 0, 16384, &ox, &oy));
	// Inside the anchor's client area (partial overlap is fine)...
	CHECK(comp_texture_target_anchor_offset(&r, 26, 82, 1280, 720, 16384, &ox, &oy));
	// ...but starting at or past its right/bottom edge leaves the weaver nothing to weave.
	CHECK_FALSE(comp_texture_target_anchor_offset(&r, 26, 82, 113, 720, 16384, &ox, &oy));
	CHECK_FALSE(comp_texture_target_anchor_offset(&r, 26, 82, 1280, 67, 16384, &ox, &oy));
	// The padded render target would exceed the device limit.
	CHECK_FALSE(comp_texture_target_anchor_offset(&r, 0, 0, 0, 0, 2700, &ox, &oy));
	CHECK_FALSE(comp_texture_target_anchor_offset(nullptr, 0, 0, 0, 0, 16384, &ox, &oy));
}
