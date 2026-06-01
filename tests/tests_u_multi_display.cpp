// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Multi-display slice geometry tests (issue #69 Phase 3a.2).
 * @author David Fattal
 */

#include "util/u_multi_display.h"

#include "catch_amalgamated.hpp"

// Monitor fixtures (screen-space, half-open):
//   A: primary 1920x1080 at origin
//   B: 1920x1080 directly right of A (side-by-side, seam at x=1920)
//   C: 640x1040 right of A but offset upward (different size + negative top)
static const u_md_rect A{0, 0, 1920, 1080};
static const u_md_rect B{1920, 0, 3840, 1080};
static const u_md_rect C{1920, -200, 2560, 840};

TEST_CASE("u_multi_display_compute_slices")
{
	u_md_slice out[U_MD_MAX_SLICES];

	SECTION("window fully on one monitor -> 1 slice == whole window")
	{
		u_md_rect win{100, 100, 500, 400}; // 400 x 300
		u_md_rect mons[] = {A};
		uint32_t n = u_multi_display_compute_slices(win, mons, 1, out, U_MD_MAX_SLICES);
		REQUIRE(n == 1);
		CHECK(out[0].atlas_x == 0);
		CHECK(out[0].atlas_y == 0);
		CHECK(out[0].atlas_w == 400);
		CHECK(out[0].atlas_h == 300);
		CHECK(out[0].canvas_offset_x == 100);
		CHECK(out[0].canvas_offset_y == 100);
		CHECK(out[0].backbuffer_offset_x == 0);
		CHECK(out[0].backbuffer_offset_y == 0);
		CHECK(out[0].monitor_index == 0);
	}

	SECTION("window spanning two side-by-side monitors")
	{
		u_md_rect win{1820, 200, 2020, 500}; // 200 x 300, straddles seam at x=1920
		u_md_rect mons[] = {A, B};
		uint32_t n = u_multi_display_compute_slices(win, mons, 2, out, U_MD_MAX_SLICES);
		REQUIRE(n == 2);

		// Monitor A: x in [1820,1920) -> 100 wide at atlas origin.
		CHECK(out[0].monitor_index == 0);
		CHECK(out[0].atlas_x == 0);
		CHECK(out[0].atlas_y == 0);
		CHECK(out[0].atlas_w == 100);
		CHECK(out[0].atlas_h == 300);
		CHECK(out[0].canvas_offset_x == 1820); // 1820 - 0
		CHECK(out[0].canvas_offset_y == 200);
		CHECK(out[0].backbuffer_offset_x == 0);
		CHECK(out[0].backbuffer_offset_y == 0);

		// Monitor B: x in [1920,2020) -> 100 wide, atlas offset 100.
		CHECK(out[1].monitor_index == 1);
		CHECK(out[1].atlas_x == 100);
		CHECK(out[1].atlas_y == 0);
		CHECK(out[1].atlas_w == 100);
		CHECK(out[1].atlas_h == 300);
		CHECK(out[1].canvas_offset_x == 0); // 1920 - 1920
		CHECK(out[1].canvas_offset_y == 200);
		CHECK(out[1].backbuffer_offset_x == 100);
		CHECK(out[1].backbuffer_offset_y == 0);
	}

	SECTION("spanning monitors of different size/origin (negative coords)")
	{
		u_md_rect win{1800, -100, 2200, 600}; // 400 x 700
		u_md_rect mons[] = {A, C};
		uint32_t n = u_multi_display_compute_slices(win, mons, 2, out, U_MD_MAX_SLICES);
		REQUIRE(n == 2);

		// A ∩ win: l=1800 t=0 r=1920 b=600 -> 120 x 600 @ screen (1800,0)
		CHECK(out[0].monitor_index == 0);
		CHECK(out[0].atlas_x == 0);   // 1800 - 1800
		CHECK(out[0].atlas_y == 100); // 0 - (-100)
		CHECK(out[0].atlas_w == 120);
		CHECK(out[0].atlas_h == 600);
		CHECK(out[0].canvas_offset_x == 1800);
		CHECK(out[0].canvas_offset_y == 0);
		CHECK(out[0].backbuffer_offset_x == 0);
		CHECK(out[0].backbuffer_offset_y == 100);

		// C ∩ win: l=1920 t=-100 r=2200 b=600 -> 280 x 700 @ screen (1920,-100)
		CHECK(out[1].monitor_index == 1);
		CHECK(out[1].atlas_x == 120); // 1920 - 1800
		CHECK(out[1].atlas_y == 0);   // -100 - (-100)
		CHECK(out[1].atlas_w == 280);
		CHECK(out[1].atlas_h == 700);
		CHECK(out[1].canvas_offset_x == 0);   // 1920 - 1920
		CHECK(out[1].canvas_offset_y == 100); // -100 - (-200)
		CHECK(out[1].backbuffer_offset_x == 120);
		CHECK(out[1].backbuffer_offset_y == 0);
	}

	SECTION("window partly off all monitors (partial coverage)")
	{
		u_md_rect win{-50, 1000, 50, 1200}; // 100 x 200, off-left + below-bottom
		u_md_rect mons[] = {A};
		uint32_t n = u_multi_display_compute_slices(win, mons, 1, out, U_MD_MAX_SLICES);
		REQUIRE(n == 1);
		// A ∩ win: l=0 t=1000 r=50 b=1080 -> 50 x 80 @ screen (0,1000)
		CHECK(out[0].atlas_x == 50); // 0 - (-50)
		CHECK(out[0].atlas_y == 0);  // 1000 - 1000
		CHECK(out[0].atlas_w == 50);
		CHECK(out[0].atlas_h == 80);
		CHECK(out[0].canvas_offset_x == 0);
		CHECK(out[0].canvas_offset_y == 1000);
		CHECK(out[0].backbuffer_offset_x == 50);
		CHECK(out[0].backbuffer_offset_y == 0);
		// Only 50*80=4000 of 100*200=20000 px covered; the rest is off-screen.
	}

	SECTION("empty / zero monitor list -> 0 slices")
	{
		u_md_rect win{0, 0, 100, 100};
		CHECK(u_multi_display_compute_slices(win, nullptr, 0, out, U_MD_MAX_SLICES) == 0);
		u_md_rect mons[] = {A};
		CHECK(u_multi_display_compute_slices(win, mons, 0, out, U_MD_MAX_SLICES) == 0);
	}

	SECTION("window fully off-screen -> 0 slices")
	{
		u_md_rect win{5000, 5000, 5100, 5100};
		u_md_rect mons[] = {A, B};
		CHECK(u_multi_display_compute_slices(win, mons, 2, out, U_MD_MAX_SLICES) == 0);
	}

	SECTION("zero-area window -> 0 slices")
	{
		u_md_rect mons[] = {A};
		CHECK(u_multi_display_compute_slices(u_md_rect{10, 10, 10, 200}, mons, 1, out, U_MD_MAX_SLICES) == 0);
		CHECK(u_multi_display_compute_slices(u_md_rect{10, 10, 200, 10}, mons, 1, out, U_MD_MAX_SLICES) == 0);
	}

	SECTION("exact-edge adjacency -> no double-count")
	{
		u_md_rect mons[] = {A, B};

		// Window exactly == A; B starts at the exclusive right edge -> A only.
		uint32_t n = u_multi_display_compute_slices(u_md_rect{0, 0, 1920, 1080}, mons, 2, out, U_MD_MAX_SLICES);
		REQUIRE(n == 1);
		CHECK(out[0].monitor_index == 0);
		CHECK(out[0].atlas_w == 1920);
		CHECK(out[0].atlas_h == 1080);

		// Window exactly == B -> B only.
		n = u_multi_display_compute_slices(u_md_rect{1920, 0, 3840, 1080}, mons, 2, out, U_MD_MAX_SLICES);
		REQUIRE(n == 1);
		CHECK(out[0].monitor_index == 1);
		CHECK(out[0].atlas_x == 0);
		CHECK(out[0].canvas_offset_x == 0);
	}

	SECTION("capacity clamp -> only the first out_capacity slices written")
	{
		// Window covers A, B, and C; capacity 2 -> only the first two written.
		u_md_rect win{1800, 0, 3000, 800};
		u_md_rect mons[] = {A, B, C};
		uint32_t n = u_multi_display_compute_slices(win, mons, 3, out, 2);
		CHECK(n == 2);
		CHECK(out[0].monitor_index == 0);
		CHECK(out[1].monitor_index == 1);
	}
}
