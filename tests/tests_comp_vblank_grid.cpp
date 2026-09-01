// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the vblank grid (Android late-weave / repaint pacing).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_vblank_grid.h"

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t P60 = 16666666ULL; // 60.00 Hz
static constexpr uint64_t P120 = 8333333ULL; // 120.00 Hz

TEST_CASE("vblank grid: refuses to answer before it is anchored")
{
	struct comp_vblank_grid g = {};
	// The whole failure mode this module exists to prevent is a confident
	// answer from a grid that has no business giving one.
	CHECK_FALSE(comp_vblank_grid_trusted(&g, 1000 * MS));
	CHECK(comp_vblank_grid_next_vblank_after(&g, 1000 * MS) == 0);
	CHECK(comp_vblank_grid_forward_horizon(&g, 1000 * MS) == 0);

	SECTION("a period alone is not enough — it cannot place a vblank")
	{
		REQUIRE(comp_vblank_grid_set_period(&g, P60));
		CHECK_FALSE(comp_vblank_grid_trusted(&g, 1000 * MS));
		CHECK(comp_vblank_grid_next_vblank_after(&g, 1000 * MS) == 0);
	}

	SECTION("an anchor alone is not enough — it cannot step")
	{
		REQUIRE(comp_vblank_grid_observe(&g, 1000 * MS));
		CHECK_FALSE(comp_vblank_grid_trusted(&g, 1000 * MS));
		CHECK(comp_vblank_grid_next_vblank_after(&g, 1000 * MS) == 0);
	}
}

TEST_CASE("vblank grid: implausible periods are refused, not clamped")
{
	struct comp_vblank_grid g = {};
	CHECK_FALSE(comp_vblank_grid_set_period(&g, 0));
	CHECK_FALSE(comp_vblank_grid_set_period(&g, 1000));          // 1 MHz
	CHECK_FALSE(comp_vblank_grid_set_period(&g, 100 * MS));      // 10 Hz
	CHECK(g.period_ns == 0);
	// A clamp here would manufacture a plausible-looking grid from a garbage
	// driver read, which is exactly the class of bug that is invisible later.
	CHECK(comp_vblank_grid_set_period(&g, P60));
	CHECK(g.period_ns == P60);
}

TEST_CASE("vblank grid: projects the next vblank on the grid")
{
	struct comp_vblank_grid g = {};
	REQUIRE(comp_vblank_grid_set_period(&g, P60));
	REQUIRE(comp_vblank_grid_observe(&g, 1000 * MS));

	// Strictly after: exactly on a vblank must return the NEXT one, or a
	// scheduler that wakes precisely on time would target the vblank it is
	// already at and busy-spin.
	CHECK(comp_vblank_grid_next_vblank_after(&g, 1000 * MS) == 1000 * MS + P60);

	// Mid-interval.
	CHECK(comp_vblank_grid_next_vblank_after(&g, 1000 * MS + 5 * MS) == 1000 * MS + P60);

	// Many periods later, with no new observation: still on the grid.
	const uint64_t later = 1000 * MS + 10 * P60 + 1;
	CHECK(comp_vblank_grid_next_vblank_after(&g, later) == 1000 * MS + 11 * P60);
}

TEST_CASE("vblank grid: forward horizon is the time to the next scanout")
{
	struct comp_vblank_grid g = {};
	REQUIRE(comp_vblank_grid_set_period(&g, P60));
	REQUIRE(comp_vblank_grid_observe(&g, 1000 * MS));

	CHECK(comp_vblank_grid_forward_horizon(&g, 1000 * MS) == P60);
	CHECK(comp_vblank_grid_forward_horizon(&g, 1000 * MS + 6 * MS) == P60 - 6 * MS);

	// Never exceeds one period: the horizon is to the NEXT scanout, not to
	// some multiple of it.
	for (uint64_t off = 0; off < P60; off += MS) {
		const uint64_t h = comp_vblank_grid_forward_horizon(&g, 1000 * MS + off);
		CHECK(h > 0);
		CHECK(h <= P60);
	}
}

TEST_CASE("vblank grid: a stale anchor stops being trusted")
{
	struct comp_vblank_grid g = {};
	REQUIRE(comp_vblank_grid_set_period(&g, P60));
	REQUIRE(comp_vblank_grid_observe(&g, 1000 * MS));

	CHECK(comp_vblank_grid_trusted(&g, 1000 * MS + 499 * MS));
	CHECK_FALSE(comp_vblank_grid_trusted(&g, 1000 * MS + 501 * MS));
	// And it must answer 0 rather than project from a dead anchor — a feed
	// that stopped (paused app, lost surface) must not leave a grid behind
	// that still looks authoritative.
	CHECK(comp_vblank_grid_next_vblank_after(&g, 1000 * MS + 501 * MS) == 0);
	CHECK(comp_vblank_grid_forward_horizon(&g, 1000 * MS + 501 * MS) == 0);
}

TEST_CASE("vblank grid: observations never rewind the anchor")
{
	struct comp_vblank_grid g = {};
	REQUIRE(comp_vblank_grid_set_period(&g, P60));
	REQUIRE(comp_vblank_grid_observe(&g, 1000 * MS));

	// Presentation timing arrives out of order on some drivers. A late
	// delivery of an OLDER present must not drag the schedule backwards.
	CHECK_FALSE(comp_vblank_grid_observe(&g, 990 * MS));
	CHECK(g.anchor_ns == 1000 * MS);
	CHECK_FALSE(comp_vblank_grid_observe(&g, 1000 * MS)); // duplicate
	CHECK(g.anchor_ns == 1000 * MS);
	CHECK(comp_vblank_grid_observe(&g, 1000 * MS + P60));
	CHECK(g.anchor_ns == 1000 * MS + P60);
}

TEST_CASE("vblank grid: survives a platform refresh-rate switch")
{
	// The NP02J switches the panel across [60, 90, 120, 144] under the
	// runtime. The grid must follow the new period and keep its anchor,
	// which is still a real vblank.
	struct comp_vblank_grid g = {};
	REQUIRE(comp_vblank_grid_set_period(&g, P60));
	REQUIRE(comp_vblank_grid_observe(&g, 1000 * MS));
	CHECK(comp_vblank_grid_next_vblank_after(&g, 1000 * MS) == 1000 * MS + P60);

	REQUIRE(comp_vblank_grid_set_period(&g, P120));
	CHECK(g.period_changes == 1);
	CHECK(comp_vblank_grid_trusted(&g, 1000 * MS));
	CHECK(comp_vblank_grid_next_vblank_after(&g, 1000 * MS) == 1000 * MS + P120);

	// Re-publishing the same period is not a change.
	REQUIRE(comp_vblank_grid_set_period(&g, P120));
	CHECK(g.period_changes == 1);
}

TEST_CASE("vblank grid: driver jitter is not a mode change")
{
	// Measured on device: the driver returns a slightly different
	// refreshDuration on each read of the SAME mode, so a bare != counted
	// every poll as a switch and churned the grid twice a second at a
	// rock-steady 59.86 Hz.
	struct comp_vblank_grid g = {};
	REQUIRE(comp_vblank_grid_set_period(&g, 16706999));
	REQUIRE(comp_vblank_grid_observe(&g, 1000 * MS));
	const uint64_t established = g.period_ns;

	for (uint64_t jitter = 1; jitter < 5000; jitter += 977) {
		REQUIRE(comp_vblank_grid_set_period(&g, 16706999 + jitter));
		REQUIRE(comp_vblank_grid_set_period(&g, 16706999 - jitter));
	}
	CHECK(g.period_changes == 0);
	CHECK(g.period_ns == established); // and it keeps the first value, not the last

	// A real switch still registers. The narrowest adjacent pair this panel
	// exposes is 120 -> 144 Hz, a 20% step — far outside the 1% band.
	REQUIRE(comp_vblank_grid_set_period(&g, P120));
	CHECK(g.period_changes == 1);
	CHECK(g.period_ns == P120);
	REQUIRE(comp_vblank_grid_set_period(&g, 6944444)); // 144 Hz
	CHECK(g.period_changes == 2);
}

TEST_CASE("vblank grid: phase reset keeps the period")
{
	struct comp_vblank_grid g = {};
	REQUIRE(comp_vblank_grid_set_period(&g, P60));
	REQUIRE(comp_vblank_grid_observe(&g, 1000 * MS));

	// A swapchain recreate invalidates the phase but not the panel's period.
	comp_vblank_grid_reset_phase(&g);
	CHECK(g.period_ns == P60);
	CHECK_FALSE(comp_vblank_grid_trusted(&g, 1000 * MS));
	REQUIRE(comp_vblank_grid_observe(&g, 2000 * MS));
	CHECK(comp_vblank_grid_trusted(&g, 2000 * MS));
	CHECK(comp_vblank_grid_next_vblank_after(&g, 2000 * MS) == 2000 * MS + P60);
}

TEST_CASE("vblank grid: an anchor slightly in the future is usable")
{
	// A present observed at the instant it lands can read marginally ahead of
	// the caller's clock sample; that must not read as untrusted.
	struct comp_vblank_grid g = {};
	REQUIRE(comp_vblank_grid_set_period(&g, P60));
	REQUIRE(comp_vblank_grid_observe(&g, 1000 * MS));
	CHECK(comp_vblank_grid_trusted(&g, 1000 * MS - 1 * MS));
	CHECK(comp_vblank_grid_next_vblank_after(&g, 1000 * MS - 1 * MS) == 1000 * MS);
}

TEST_CASE("vblank grid: NULL is handled everywhere")
{
	CHECK_FALSE(comp_vblank_grid_set_period(NULL, P60));
	CHECK_FALSE(comp_vblank_grid_observe(NULL, 1000 * MS));
	CHECK_FALSE(comp_vblank_grid_trusted(NULL, 1000 * MS));
	CHECK(comp_vblank_grid_next_vblank_after(NULL, 1000 * MS) == 0);
	CHECK(comp_vblank_grid_forward_horizon(NULL, 1000 * MS) == 0);
	comp_vblank_grid_reset_phase(NULL); // must not crash
}
