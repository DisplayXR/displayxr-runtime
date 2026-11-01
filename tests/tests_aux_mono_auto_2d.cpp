// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Auto hardware 2D on a flat stereo submission: the classifier and the
 *         hysteresis (util/u_mono_auto_2d.h).
 *
 * The rule under test: same swapchain + array index + image rect for every
 * view = a flat frame; 0.5 s of continuous flat frames enters 2D; the first
 * frame that is not flat leaves at once; losing eligibility (an explicit app
 * display-mode choice, the kill switch) leaves at once and never re-enters.
 */

#include "catch_amalgamated.hpp"

#include "util/u_mono_auto_2d.h"

namespace {

constexpr uint64_t MS = 1000ull * 1000ull;
constexpr uint64_t FRAME = 16 * MS; // ~60 Hz

u_mono_view_ref
view(uint64_t sc, uint32_t idx, int32_t x, int32_t y, int32_t w, int32_t h)
{
	u_mono_view_ref v{};
	v.swapchain = sc;
	v.array_index = idx;
	v.x = x;
	v.y = y;
	v.w = w;
	v.h = h;
	return v;
}

} // namespace

TEST_CASE("u_mono_views_identical: same swapchain, index and rect", "[mono_auto_2d]")
{
	const u_mono_view_ref same[2] = {view(7, 0, 0, 0, 960, 1080), view(7, 0, 0, 0, 960, 1080)};
	CHECK(u_mono_views_identical(same, 2));

	// The ordinary stereo shapes are never flat.
	const u_mono_view_ref sbs[2] = {view(7, 0, 0, 0, 960, 1080), view(7, 0, 960, 0, 960, 1080)};
	CHECK_FALSE(u_mono_views_identical(sbs, 2));
	const u_mono_view_ref layered[2] = {view(7, 0, 0, 0, 960, 1080), view(7, 1, 0, 0, 960, 1080)};
	CHECK_FALSE(u_mono_views_identical(layered, 2));
	const u_mono_view_ref two_sc[2] = {view(7, 0, 0, 0, 960, 1080), view(8, 0, 0, 0, 960, 1080)};
	CHECK_FALSE(u_mono_views_identical(two_sc, 2));
	const u_mono_view_ref sizes[2] = {view(7, 0, 0, 0, 960, 1080), view(7, 0, 0, 0, 960, 1079)};
	CHECK_FALSE(u_mono_views_identical(sizes, 2));

	// Multiview: all four must agree.
	const u_mono_view_ref four[4] = {view(3, 0, 0, 0, 64, 64), view(3, 0, 0, 0, 64, 64), view(3, 0, 0, 0, 64, 64),
	                                 view(3, 0, 0, 0, 64, 64)};
	CHECK(u_mono_views_identical(four, 4));
	u_mono_view_ref four_odd[4] = {four[0], four[1], four[2], four[3]};
	four_odd[3].y = 1;
	CHECK_FALSE(u_mono_views_identical(four_odd, 4));

	// One view duplicates nothing.
	CHECK_FALSE(u_mono_views_identical(same, 1));
	CHECK_FALSE(u_mono_views_identical(nullptr, 2));
}

TEST_CASE("u_mono_auto_2d: enters after the hysteresis, leaves on the first stereo frame", "[mono_auto_2d]")
{
	u_mono_auto_2d s;
	u_mono_auto_2d_init(&s);
	const uint64_t enter = U_MONO_AUTO_2D_ENTER_NS;
	uint64_t t = 1000 * MS;

	// Stereo frames: nothing.
	for (int i = 0; i < 10; i++, t += FRAME) {
		CHECK(u_mono_auto_2d_step(&s, true, false, t, enter) == U_MONO_AUTO_2D_NONE);
	}

	// Flat frames: nothing until 0.5 s have passed since the first one.
	const uint64_t start = t;
	int enters = 0;
	uint64_t entered_at = 0;
	for (; t < start + 2 * enter; t += FRAME) {
		const auto a = u_mono_auto_2d_step(&s, true, true, t, enter);
		if (a == U_MONO_AUTO_2D_ENTER) {
			enters++;
			entered_at = t;
		} else {
			CHECK(a == U_MONO_AUTO_2D_NONE);
		}
	}
	CHECK(enters == 1);
	CHECK(entered_at >= start + enter);
	CHECK(entered_at < start + enter + FRAME);
	CHECK(s.active);

	// One stereo frame leaves immediately, once.
	CHECK(u_mono_auto_2d_step(&s, true, false, t, enter) == U_MONO_AUTO_2D_LEAVE);
	t += FRAME;
	CHECK_FALSE(s.active);
	CHECK(u_mono_auto_2d_step(&s, true, false, t, enter) == U_MONO_AUTO_2D_NONE);
}

TEST_CASE("u_mono_auto_2d: a stereo frame inside the window restarts the clock", "[mono_auto_2d]")
{
	u_mono_auto_2d s;
	u_mono_auto_2d_init(&s);
	const uint64_t enter = U_MONO_AUTO_2D_ENTER_NS;
	uint64_t t = 0;

	// 0.4 s flat, one stereo frame, then flat again: the second run must be a
	// full 0.5 s on its own.
	for (; t < 400 * MS; t += FRAME) {
		CHECK(u_mono_auto_2d_step(&s, true, true, t, enter) == U_MONO_AUTO_2D_NONE);
	}
	CHECK(u_mono_auto_2d_step(&s, true, false, t, enter) == U_MONO_AUTO_2D_NONE);
	t += FRAME;
	const uint64_t restart = t;
	for (; t < restart + enter; t += FRAME) {
		CHECK(u_mono_auto_2d_step(&s, true, true, t, enter) == U_MONO_AUTO_2D_NONE);
	}
	CHECK(u_mono_auto_2d_step(&s, true, true, t, enter) == U_MONO_AUTO_2D_ENTER);
}

TEST_CASE("u_mono_auto_2d: slow frames wait the same wall-clock time", "[mono_auto_2d]")
{
	u_mono_auto_2d s;
	u_mono_auto_2d_init(&s);
	const uint64_t enter = U_MONO_AUTO_2D_ENTER_NS;
	// A 4 fps loading screen: frames at 0, 250, 500 ms.
	CHECK(u_mono_auto_2d_step(&s, true, true, 0, enter) == U_MONO_AUTO_2D_NONE);
	CHECK(u_mono_auto_2d_step(&s, true, true, 250 * MS, enter) == U_MONO_AUTO_2D_NONE);
	CHECK(u_mono_auto_2d_step(&s, true, true, 500 * MS, enter) == U_MONO_AUTO_2D_ENTER);
}

TEST_CASE("u_mono_auto_2d: not eligible never enters, and losing eligibility leaves", "[mono_auto_2d]")
{
	u_mono_auto_2d s;
	u_mono_auto_2d_init(&s);
	const uint64_t enter = U_MONO_AUTO_2D_ENTER_NS;
	uint64_t t = 0;

	// An app that chose its display mode explicitly (or the kill switch is
	// off): flat forever, never 2D.
	for (; t < 5 * enter; t += FRAME) {
		CHECK(u_mono_auto_2d_step(&s, false, true, t, enter) == U_MONO_AUTO_2D_NONE);
	}
	CHECK_FALSE(s.active);

	// Eligible: engages after the window.
	const uint64_t start = t;
	enum u_mono_auto_2d_action a = U_MONO_AUTO_2D_NONE;
	for (; a != U_MONO_AUTO_2D_ENTER && t < start + 2 * enter; t += FRAME) {
		a = u_mono_auto_2d_step(&s, true, true, t, enter);
	}
	REQUIRE(a == U_MONO_AUTO_2D_ENTER);

	// Eligibility lost mid-flat (e.g. the app called xrRequestDisplayModeDXR):
	// leave at once even though the frames are still flat.
	CHECK(u_mono_auto_2d_step(&s, false, true, t, enter) == U_MONO_AUTO_2D_LEAVE);
	CHECK(u_mono_auto_2d_step(&s, false, true, t + FRAME, enter) == U_MONO_AUTO_2D_NONE);
}

TEST_CASE("u_mono_auto_2d_release: reports whether the caller must restore", "[mono_auto_2d]")
{
	u_mono_auto_2d s;
	u_mono_auto_2d_init(&s);
	CHECK_FALSE(u_mono_auto_2d_release(&s));

	uint64_t t = 0;
	enum u_mono_auto_2d_action a = U_MONO_AUTO_2D_NONE;
	for (; a != U_MONO_AUTO_2D_ENTER; t += FRAME) {
		a = u_mono_auto_2d_step(&s, true, true, t, U_MONO_AUTO_2D_ENTER_NS);
	}
	CHECK(u_mono_auto_2d_release(&s));
	CHECK_FALSE(s.active);
	// Released: the next stereo frame has nothing to leave.
	CHECK(u_mono_auto_2d_step(&s, true, false, t, U_MONO_AUTO_2D_ENTER_NS) == U_MONO_AUTO_2D_NONE);
	CHECK_FALSE(u_mono_auto_2d_release(&s));
}
