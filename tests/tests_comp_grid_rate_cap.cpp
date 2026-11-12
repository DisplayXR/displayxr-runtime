// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the grid tier's one-weave-per-vblank target (#2629).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_grid_rate_cap.h"

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t P60 = 16666666ULL;
static constexpr uint64_t P90 = 11111111ULL;
static constexpr uint64_t ANCHOR = 1000 * MS;

static comp_vblank_grid
grid(uint64_t period)
{
	comp_vblank_grid g = {};
	REQUIRE(comp_vblank_grid_set_period(&g, period));
	REQUIRE(comp_vblank_grid_observe(&g, ANCHOR));
	return g;
}

static uint64_t
vb(int n, uint64_t p = P60)
{
	return ANCHOR + (uint64_t)n * p;
}

TEST_CASE("grid rate cap: budget under a period aims the next vblank it can make")
{
	const comp_vblank_grid g = grid(P60);
	uint64_t sleep_ns = 0;
	// 2 ms after vblank 1, 5 ms budget: vblank 2 is reachable.
	const uint64_t now = vb(1) + 2 * MS;
	const uint64_t t = comp_grid_rate_cap_target(&g, now, 5 * MS, 0, &sleep_ns);
	CHECK(t == vb(2));
	CHECK(sleep_ns == vb(2) - 5 * MS - now);

	SECTION("too late for the next one: aims the one after, no sleep past it")
	{
		const uint64_t late = vb(2) - 3 * MS;
		const uint64_t t2 = comp_grid_rate_cap_target(&g, late, 5 * MS, 0, &sleep_ns);
		CHECK(t2 == vb(3));
		CHECK(sleep_ns == vb(3) - 5 * MS - late);
	}
	SECTION("untrusted grid: no aim")
	{
		comp_vblank_grid u = {};
		CHECK(comp_grid_rate_cap_target(&u, now, 5 * MS, 0, &sleep_ns) == 0);
		CHECK(sleep_ns == 0);
	}
}

TEST_CASE("grid rate cap: a budget of a period or more still paces one weave per vblank")
{
	const comp_vblank_grid g = grid(P60);
	// The inflated-budget case from the device: 18 ms budget on 16.7 ms.
	const uint64_t budget = 18 * MS;
	uint64_t now = vb(1) + 1 * MS;
	uint64_t last = 0;
	uint64_t prev = 0;
	for (int i = 0; i < 20; i++) {
		uint64_t sleep_ns = 0;
		const uint64_t t = comp_grid_rate_cap_target(&g, now, budget, last, &sleep_ns);
		REQUIRE(t != 0);
		CHECK(sleep_ns <= 2 * P60);
		if (prev != 0) {
			CHECK(t == prev + P60); // consecutive vblanks, never shared
		}
		prev = t;
		last = t;
		// The weave starts after the sleep and takes the whole budget, then
		// the next frame arrives immediately (a producer faster than the panel).
		now = now + sleep_ns + 1 * MS;
	}
}

TEST_CASE("grid rate cap: two weaves inside one period get consecutive vblanks")
{
	const comp_vblank_grid g = grid(P90);
	uint64_t s1 = 0;
	uint64_t s2 = 0;
	const uint64_t now = vb(5, P90) + 1 * MS;
	const uint64_t t1 = comp_grid_rate_cap_target(&g, now, 3 * MS, 0, &s1);
	CHECK(t1 == vb(6, P90));
	// A repaint right after, same budget: pushed to the next vblank.
	const uint64_t now2 = now + s1 + 2 * MS;
	const uint64_t t2 = comp_grid_rate_cap_target(&g, now2, 3 * MS, t1, &s2);
	CHECK(t2 == vb(7, P90));
	CHECK(s2 == vb(7, P90) - 3 * MS - now2);

	SECTION("a producer already slower than the panel is never pushed")
	{
		// 60 Hz repaint cadence on a 90 Hz panel: the next weave comes 16.7 ms
		// later and its natural target is already past last + 0.75 period.
		const uint64_t later = now + P60;
		uint64_t s3 = 0;
		const uint64_t natural = comp_grid_rate_cap_target(&g, later, 3 * MS, 0, &s3);
		CHECK(comp_grid_rate_cap_target(&g, later, 3 * MS, t1, &s3) == natural);
	}
}

TEST_CASE("grid rate cap: a stale previous target is ignored")
{
	const comp_vblank_grid g = grid(P60);
	uint64_t sleep_ns = 0;
	const uint64_t now = vb(10) + 1 * MS;
	const uint64_t natural = comp_grid_rate_cap_target(&g, now, 5 * MS, 0, &sleep_ns);
	// Last target 5 periods ago: not a neighbour, no push.
	CHECK(comp_grid_rate_cap_target(&g, now, 5 * MS, vb(5), &sleep_ns) == natural);
	// A target in the past that the natural one already clears: no push either.
	CHECK(comp_grid_rate_cap_target(&g, now, 5 * MS, vb(10), &sleep_ns) == natural);
}

TEST_CASE("grid rate cap: after a period change the caller's reset gives the natural aim")
{
	comp_vblank_grid g = grid(P90);
	uint64_t sleep_ns = 0;
	const uint64_t now = vb(3, P90) + 1 * MS;
	const uint64_t t90 = comp_grid_rate_cap_target(&g, now, 3 * MS, 0, &sleep_ns);
	REQUIRE(t90 == vb(4, P90));
	// The panel drops to 60 Hz; the target resets its last target to 0.
	REQUIRE(comp_vblank_grid_set_period(&g, P60));
	const uint64_t t60 = comp_grid_rate_cap_target(&g, now, 3 * MS, 0, &sleep_ns);
	CHECK(t60 == vb(3, P60)); // first 60 Hz vblank after now + budget (37.3 ms)
	CHECK(sleep_ns == vb(3, P60) - 3 * MS - now);
}

TEST_CASE("grid rate cap: the sleep never exceeds two periods")
{
	comp_vblank_grid g = grid(P60);
	uint64_t sleep_ns = 0;
	// A previous target far ahead (a wrong anchor) with a tiny budget.
	const uint64_t now = vb(1) + 1 * MS;
	const uint64_t t = comp_grid_rate_cap_target(&g, now, 1 * MS, vb(4), &sleep_ns);
	CHECK(t == vb(5));
	CHECK(sleep_ns == 2 * P60);
}
