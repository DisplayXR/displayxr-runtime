// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for window-system presentation feedback -> vblank grid
 *         (#902, the desktop-Linux late-weave source).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_present_feedback.h"

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t P60 = 16666666ULL;
static constexpr uint64_t P120 = 8333333ULL;
static constexpr uint64_t T0 = 5000ULL * 1000ULL * MS; // ~5000 s of uptime

static comp_present_feedback_sample
presented(uint64_t t, uint64_t refresh, uint64_t seq, bool vsync = true)
{
	comp_present_feedback_sample s = {};
	s.present_ns = t;
	s.refresh_ns = refresh;
	s.seq = seq;
	s.seq_valid = seq != 0;
	s.vsync = vsync;
	s.hw_clock = true;
	return s;
}

static comp_present_feedback_sample
discarded()
{
	comp_present_feedback_sample s = {};
	s.discarded = true;
	return s;
}

TEST_CASE("present feedback: a vsync'd sample with a refresh value makes the grid trusted")
{
	comp_present_feedback f = {};
	comp_vblank_grid g = {};
	const auto s = presented(T0, P60, 1000);
	const auto r = comp_present_feedback_apply(&f, &g, &s, T0 + 2 * MS);
	CHECK(r.anchored);
	CHECK(r.period_changed);
	CHECK_FALSE(r.period_derived);
	CHECK(r.state_changed);
	CHECK(f.state == COMP_PRESENT_FEEDBACK_STATE_FEEDING);
	CHECK(comp_vblank_grid_trusted(&g, T0 + 2 * MS));
	CHECK(comp_vblank_grid_next_vblank_after(&g, T0 + 2 * MS) == T0 + P60);

	SECTION("the same state again is not a state change (one log line, not one per frame)")
	{
		const auto s2 = presented(T0 + P60, P60, 1001);
		const auto r2 = comp_present_feedback_apply(&f, &g, &s2, T0 + P60 + 2 * MS);
		CHECK(r2.anchored);
		CHECK_FALSE(r2.state_changed);
		CHECK_FALSE(r2.period_changed);
	}
}

TEST_CASE("present feedback: a sample that was not vsync'd never touches the grid")
{
	comp_present_feedback f = {};
	comp_vblank_grid g = {};
	const auto s = presented(T0, P60, 1000, /*vsync=*/false);
	const auto r = comp_present_feedback_apply(&f, &g, &s, T0);
	CHECK_FALSE(r.anchored);
	CHECK(g.period_ns == 0);
	CHECK_FALSE(g.have_anchor);
	CHECK(f.not_vsync == 1);
	// One odd sample is not yet a state.
	CHECK(f.state == COMP_PRESENT_FEEDBACK_STATE_WAITING);

	SECTION("a run of them is, reported exactly once")
	{
		int changes = r.state_changed ? 1 : 0;
		for (int i = 1; i < 3 * COMP_PRESENT_FEEDBACK_DEBOUNCE; i++) {
			const auto si = presented(T0 + i * P60, P60, 1000 + i, false);
			changes += comp_present_feedback_apply(&f, &g, &si, T0 + i * P60).state_changed ? 1 : 0;
		}
		CHECK(changes == 1);
		CHECK(f.state == COMP_PRESENT_FEEDBACK_STATE_NOT_VSYNC);
		CHECK_FALSE(g.have_anchor);
	}

	SECTION("a non-vsync'd sample breaks the counter pairing used to derive a period")
	{
		comp_present_feedback f2 = {};
		comp_vblank_grid g2 = {};
		auto a = presented(T0, 0, 100);
		comp_present_feedback_apply(&f2, &g2, &a, T0);
		auto torn = presented(T0 + 5 * MS, 0, 101, false);
		comp_present_feedback_apply(&f2, &g2, &torn, T0 + 5 * MS);
		auto b = presented(T0 + 2 * P60, 0, 102);
		const auto rb = comp_present_feedback_apply(&f2, &g2, &b, T0 + 2 * P60);
		// No period from (a, b) across the torn sample — it was dropped.
		CHECK_FALSE(rb.period_changed);
		CHECK(g2.period_ns == 0);
	}
}

TEST_CASE("present feedback: discards are counted, never anchor, and only a run is a state")
{
	comp_present_feedback f = {};
	comp_vblank_grid g = {};
	auto s = presented(T0, P60, 1000);
	comp_present_feedback_apply(&f, &g, &s, T0);
	REQUIRE(f.state == COMP_PRESENT_FEEDBACK_STATE_FEEDING);
	const uint64_t anchor = g.anchor_ns;

	const auto d = discarded();
	for (int i = 0; i < COMP_PRESENT_FEEDBACK_DEBOUNCE - 1; i++) {
		const auto r = comp_present_feedback_apply(&f, &g, &d, T0);
		CHECK_FALSE(r.anchored);
		CHECK_FALSE(r.state_changed);
	}
	CHECK(f.state == COMP_PRESENT_FEEDBACK_STATE_FEEDING);
	CHECK(comp_present_feedback_apply(&f, &g, &d, T0).state_changed);
	CHECK(f.state == COMP_PRESENT_FEEDBACK_STATE_DISCARDING);
	CHECK(g.anchor_ns == anchor);
	CHECK(f.discarded == COMP_PRESENT_FEEDBACK_DEBOUNCE);

	SECTION("the next presented sample returns the feed to FEEDING, once")
	{
		auto s2 = presented(T0 + 20 * P60, P60, 1020);
		const auto r = comp_present_feedback_apply(&f, &g, &s2, T0 + 20 * P60);
		CHECK(r.anchored);
		CHECK(r.state_changed);
		CHECK(f.state == COMP_PRESENT_FEEDBACK_STATE_FEEDING);
	}
}

TEST_CASE("present feedback: X11-style samples derive the period from the vblank counter")
{
	// X11 Present gives ust/msc and no refresh value.
	comp_present_feedback f = {};
	comp_vblank_grid g = {};
	auto a = presented(T0, 0, 5000);
	auto ra = comp_present_feedback_apply(&f, &g, &a, T0);
	CHECK(ra.anchored);
	CHECK_FALSE(ra.period_changed);
	CHECK_FALSE(comp_vblank_grid_trusted(&g, T0)); // anchor, no period yet

	SECTION("consecutive vblanks")
	{
		auto b = presented(T0 + P60, 0, 5001);
		const auto rb = comp_present_feedback_apply(&f, &g, &b, T0 + P60);
		CHECK(rb.period_changed);
		CHECK(rb.period_derived);
		CHECK(g.period_ns == P60);
		CHECK(comp_vblank_grid_trusted(&g, T0 + P60));
	}

	SECTION("skipped vblanks: the counter keeps the period exact where a time delta would not")
	{
		// The app ran at a third of the panel rate.
		auto b = presented(T0 + 3 * P120, 0, 5003);
		comp_present_feedback_apply(&f, &g, &b, T0 + 3 * P120);
		CHECK(g.period_ns == P120);
	}

	SECTION("a counter that did not advance derives nothing")
	{
		auto b = presented(T0 + P60, 0, 5000);
		CHECK_FALSE(comp_present_feedback_apply(&f, &g, &b, T0 + P60).period_changed);
		CHECK(g.period_ns == 0);
	}
}

TEST_CASE("present feedback: a reported refresh wins over the counter, and a mode switch is reported")
{
	comp_present_feedback f = {};
	comp_vblank_grid g = {};
	auto a = presented(T0, P60, 10);
	comp_present_feedback_apply(&f, &g, &a, T0);
	REQUIRE(g.period_ns == P60);
	// The compositor switched the output to 120 Hz.
	auto b = presented(T0 + P60 + P120, P120, 12);
	const auto r = comp_present_feedback_apply(&f, &g, &b, T0 + P60 + P120);
	CHECK(r.period_changed);
	CHECK_FALSE(r.period_derived);
	CHECK(g.period_ns == P120);
	CHECK(g.period_changes == 1);
}

TEST_CASE("present feedback: an implausible reported refresh is refused and said")
{
	comp_present_feedback f = {};
	comp_vblank_grid g = {};
	auto s = presented(T0, 1000 /* 1 us */, 1);
	const auto r = comp_present_feedback_apply(&f, &g, &s, T0);
	CHECK(r.period_refused);
	CHECK(g.period_ns == 0);
	CHECK(r.anchored); // the timestamp itself is still a real vblank
}

TEST_CASE("present feedback: timestamps in another clock are refused for good")
{
	comp_present_feedback f = {};
	comp_vblank_grid g = {};
	// e.g. CLOCK_REALTIME (~1.8e18 ns) against a monotonic "now" of ~5000 s.
	auto s = presented(1800000000ULL * 1000ULL * MS, P60, 1);
	const auto r = comp_present_feedback_apply(&f, &g, &s, T0);
	CHECK(r.state_changed);
	CHECK(f.state == COMP_PRESENT_FEEDBACK_STATE_TIMEBASE_REFUSED);
	CHECK_FALSE(g.have_anchor);
	CHECK(g.period_ns == 0);

	// Latched: a later sample that happens to look right does not undo it.
	auto s2 = presented(T0, P60, 2);
	const auto r2 = comp_present_feedback_apply(&f, &g, &s2, T0);
	CHECK_FALSE(r2.anchored);
	CHECK_FALSE(r2.state_changed);
	CHECK_FALSE(g.have_anchor);
}

TEST_CASE("present feedback: the grid only moves forward under out-of-order delivery")
{
	comp_present_feedback f = {};
	comp_vblank_grid g = {};
	auto late = presented(T0 + 2 * P60, P60, 102);
	comp_present_feedback_apply(&f, &g, &late, T0 + 2 * P60);
	auto early = presented(T0 + P60, P60, 101);
	const auto r = comp_present_feedback_apply(&f, &g, &early, T0 + 2 * P60);
	CHECK_FALSE(r.anchored);
	CHECK(g.anchor_ns == T0 + 2 * P60);
}

TEST_CASE("present feedback: reset_phase drops the counter pairing, keeps the verdicts")
{
	comp_present_feedback f = {};
	comp_vblank_grid g = {};
	auto a = presented(T0, 0, 100);
	comp_present_feedback_apply(&f, &g, &a, T0);
	comp_present_feedback_reset_phase(&f);
	CHECK(f.timebase_checked);
	// A new swapchain: the next sample cannot pair with the old one.
	auto b = presented(T0 + 7 * MS, 0, 101);
	CHECK_FALSE(comp_present_feedback_apply(&f, &g, &b, T0 + 7 * MS).period_changed);
	CHECK(g.period_ns == 0);
}
