// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for feedback pacing's wait / slip logic (#2068).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_feedback_pace.h"

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t S = 1000ULL * MS;
static constexpr uint64_t P60 = 16666666ULL;

//! Run one window of waits with @p slips of them timing out.
static comp_fb_pace_event
window(comp_fb_pace &p, int slips, uint64_t &now)
{
	comp_fb_pace_event last = COMP_FB_PACE_NONE;
	for (int i = 0; i < COMP_FB_PACE_WINDOW; i++) {
		now += P60;
		const bool ok = i >= slips;
		const comp_fb_pace_event e = comp_fb_pace_on_wait(&p, ok, ok ? 4 * MS : 25 * MS, now);
		if (e != COMP_FB_PACE_NONE) {
			last = e;
		}
	}
	return last;
}

TEST_CASE("fb pace: latency 1 waits on the newest commit, deeper waits further back")
{
	comp_fb_pace p;
	comp_fb_pace_init(&p, 1, true);
	CHECK(comp_fb_pace_wait_target(&p, 0) == 0); // nothing committed yet
	CHECK(comp_fb_pace_wait_target(&p, 10) == 10);
	comp_fb_pace_init(&p, 2, true);
	CHECK(comp_fb_pace_wait_target(&p, 10) == 9);
	CHECK(comp_fb_pace_wait_target(&p, 1) == 0);
}

TEST_CASE("fb pace: the knob is clamped to 1..4")
{
	comp_fb_pace p;
	comp_fb_pace_init(&p, 0, true);
	CHECK(p.depth == 1);
	comp_fb_pace_init(&p, 9, true);
	CHECK(p.depth == COMP_FB_PACE_MAX_DEPTH);
}

TEST_CASE("fb pace: timeout is 1.5 periods, 25 ms before the period is known")
{
	CHECK(comp_fb_pace_timeout_ns(P60) == P60 + P60 / 2);
	CHECK(comp_fb_pace_timeout_ns(0) == 25 * MS);
}

TEST_CASE("fb pace: feedback that never arrives turns pacing off after three slips")
{
	comp_fb_pace p;
	comp_fb_pace_init(&p, 1, true);
	uint64_t now = 10 * S;
	CHECK(comp_fb_pace_on_wait(&p, false, 25 * MS, now) == COMP_FB_PACE_NONE);
	CHECK(comp_fb_pace_on_wait(&p, false, 25 * MS, now) == COMP_FB_PACE_NONE);
	CHECK(comp_fb_pace_on_wait(&p, false, 25 * MS, now) == COMP_FB_PACE_OFF);
	CHECK(p.off);
	CHECK(comp_fb_pace_wait_target(&p, 10) == 0);

	SECTION("once a wait has succeeded, slips in a row are occlusion, not absence")
	{
		comp_fb_pace q;
		comp_fb_pace_init(&q, 1, true);
		comp_fb_pace_on_wait(&q, true, 4 * MS, now);
		for (int i = 0; i < 10; i++) {
			CHECK(comp_fb_pace_on_wait(&q, false, 25 * MS, now) != COMP_FB_PACE_OFF);
		}
		CHECK_FALSE(q.off);
	}
}

TEST_CASE("fb pace: an ~8% slip window escalates once, capped at base + 1")
{
	comp_fb_pace p;
	comp_fb_pace_init(&p, 1, true);
	uint64_t now = 10 * S;
	window(p, 0, now); // establish ever_ok
	CHECK(window(p, COMP_FB_PACE_SLIP_LIMIT - 1, now) == COMP_FB_PACE_NONE);
	CHECK(p.depth == 1);
	CHECK(window(p, COMP_FB_PACE_SLIP_LIMIT, now) == COMP_FB_PACE_DEEPER);
	CHECK(p.depth == 2);
	CHECK(window(p, 40, now) == COMP_FB_PACE_AT_CAP); // the cap
	CHECK(p.depth == 2);
}

TEST_CASE("fb pace: return probe needs three clean windows and the dwell")
{
	comp_fb_pace p;
	comp_fb_pace_init(&p, 1, true);
	uint64_t now = 10 * S;
	window(p, 0, now);
	REQUIRE(window(p, 20, now) == COMP_FB_PACE_DEEPER);
	// Three clean windows are ~6 s: inside the 30 s dwell, no probe yet.
	for (int i = 0; i < 3; i++) {
		CHECK(window(p, 0, now) == COMP_FB_PACE_NONE);
	}
	CHECK(p.depth == 2);
	now += 30 * S;
	CHECK(window(p, 0, now) == COMP_FB_PACE_SHALLOWER);
	CHECK(p.depth == 1);

	SECTION("a probe that fails soon after doubles the dwell")
	{
		CHECK(window(p, 20, now) == COMP_FB_PACE_DEEPER);
		CHECK(p.dwell_ns == 2 * COMP_FB_PACE_DWELL_NS);
	}
}

TEST_CASE("fb pace: DXR_LATE_WEAVE_AUTOBACKOFF=0 holds the latency")
{
	comp_fb_pace p;
	comp_fb_pace_init(&p, 1, false);
	uint64_t now = 10 * S;
	window(p, 0, now);
	CHECK(window(p, 60, now) == COMP_FB_PACE_NONE);
	CHECK(p.depth == 1);
}

TEST_CASE("fb pace: trace totals are taken and reset")
{
	comp_fb_pace p;
	comp_fb_pace_init(&p, 1, true);
	comp_fb_pace_on_wait(&p, true, 4 * MS, 1 * S);
	comp_fb_pace_on_wait(&p, false, 25 * MS, 1 * S);
	uint64_t w = 0, s = 0, sum = 0;
	comp_fb_pace_take_stats(&p, &w, &s, &sum);
	CHECK(w == 2);
	CHECK(s == 1);
	CHECK(sum == 29 * MS);
	comp_fb_pace_take_stats(&p, &w, &s, &sum);
	CHECK(w == 0);
}
