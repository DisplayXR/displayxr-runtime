// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the present_wait depth governor (#2119): the
 *         commit-to-glass lag floor (comp_late_weave_depth.h) on top of the
 *         shared slip machine (comp_feedback_pace.h).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_feedback_pace.h"
#include "util/comp_late_weave_depth.h"

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t S = 1000ULL * MS;
static constexpr uint64_t P60 = 16666666ULL;

//! The governor as the vk_native target wires it.
struct gov
{
	comp_fb_pace pace;
	comp_lw_lag lag;
	uint64_t now = 1 * S;
	uint64_t last_frame = 0;
	int lag_changes = 0;

	gov()
	{
		comp_fb_pace_init(&pace, 1, true);
		pace.ever_ok = true;
		comp_lw_lag_init(&lag);
	}

	int
	depth() const
	{
		return comp_fb_pace_effective(&pace);
	}

	//! One paced frame: interval @p dt since the last one, raw commit-to-glass
	//! @p lag_ns measured with @p queued presents of ours ahead of it.
	void
	frame(uint64_t dt, uint64_t lag_ns, uint32_t queued = 0)
	{
		now += dt;
		if (comp_lw_lag_on_sample(&lag, comp_lw_lag_intrinsic_ns(lag_ns, queued, P60), P60, now)) {
			lag_changes++;
			comp_fb_pace_set_floor(&pace, lag.depth, now);
		}
		if (last_frame != 0) {
			comp_fb_pace_on_wait(&pace, !comp_lw_lag_is_slip(now - last_frame, P60), 0, now);
		}
		last_frame = now;
	}

	void
	run(int n, uint64_t dt, uint64_t lag_ns)
	{
		for (int i = 0; i < n; i++) {
			frame(dt, lag_ns);
		}
	}
};

TEST_CASE("lw depth: base is 1 before anything is measured")
{
	gov g;
	CHECK(g.depth() == 1);
	CHECK(comp_fb_pace_wait_target(&g.pace, 10) == 10);
}

TEST_CASE("lw depth: a composited window (2-period commit-to-glass lag) paces at depth 2")
{
	gov g;
	// Depth 1 on a composited desktop: the loop runs at half rate, lag ~1.9 periods.
	g.run(COMP_LW_LAG_MIN_SAMPLES, 2 * P60, P60 * 19 / 10);
	CHECK(g.lag.depth == 2);
	CHECK(g.depth() == 2);
	CHECK(comp_fb_pace_wait_target(&g.pace, 10) == 9);
	// Now at rate; stays at 2 (the floor) and the slip machine never probes under it.
	g.run(60 * 120, P60, P60 * 19 / 10);
	CHECK(g.depth() == 2);
}

TEST_CASE("lw depth: a direct-scanout target (sub-period lag) stays at depth 1")
{
	gov g;
	g.run(60 * 60, P60, P60 * 8 / 10);
	CHECK(g.lag.depth == 1);
	CHECK(g.depth() == 1);
}

TEST_CASE("lw depth: a saturated 25 ms app at 60 Hz escalates to 2, then probes back to 1")
{
	gov g;
	// 25 ms average frame on a 60 Hz FIFO: alternating 1- and 2-vblank intervals,
	// half of them slips. One-period lag: the floor stays 1, saturation drives it.
	for (int i = 0; i <= COMP_FB_PACE_WINDOW; i++) { // first frame has no interval
		g.frame((i & 1) ? 2 * P60 : P60, P60 * 8 / 10);
	}
	CHECK(g.lag.depth == 1);
	CHECK(g.depth() == 2);
	// Saturation over: clean windows. No probe before the dwell...
	g.run(COMP_FB_PACE_WINDOW * COMP_FB_PACE_CLEAN_WINDOWS, P60, P60 * 8 / 10);
	CHECK(g.depth() == 2);
	// ...and one level down after it.
	g.run(60 * 40, P60, P60 * 8 / 10);
	CHECK(g.depth() == 1);
}

TEST_CASE("lw depth: slip escalation is capped one level above the lag floor")
{
	gov g;
	g.run(COMP_LW_LAG_MIN_SAMPLES, 2 * P60, 2 * P60);
	REQUIRE(g.depth() == 2);
	// Still slipping at the floor: one banked level, never more.
	g.run(COMP_FB_PACE_WINDOW * 4, 2 * P60, 2 * P60);
	CHECK(g.depth() == 3);
}

TEST_CASE("lw depth: a lag flipping 1 -> 2 -> 1 periods rises fast and never flaps faster than the dwell")
{
	gov g;
	// mutter's dynamic buffering: the lag alternates between ~1 and ~2 periods
	// in 5 s spells (the measured 59, 34, 97, 60, ... presents/s witness).
	int max_depth_seen = 1;
	uint64_t last_lower = 0;
	uint64_t min_gap_between_lowerings = UINT64_MAX;
	int prev = g.depth();
	for (int spell = 0; spell < 40; spell++) {
		const uint64_t lag = (spell & 1) ? P60 * 19 / 10 : P60 * 9 / 10;
		for (int i = 0; i < 5 * 60; i++) {
			g.frame(P60, lag);
			const int d = g.depth();
			if (d < prev) {
				if (last_lower != 0 && g.now - last_lower < min_gap_between_lowerings) {
					min_gap_between_lowerings = g.now - last_lower;
				}
				last_lower = g.now;
			}
			max_depth_seen = d > max_depth_seen ? d : max_depth_seen;
			prev = d;
		}
	}
	// It went to 2 on the first 2-period spell...
	CHECK(max_depth_seen == 2);
	// ...and 5 s spells at 1 period never outlast the 30 s dwell: it holds 2.
	CHECK(g.depth() == 2);
	CHECK(last_lower == 0);
	CHECK(g.lag_changes == 1);

	// A long calm stretch at 1 period lowers it once, after the dwell.
	const uint64_t calm_start = g.now;
	g.run(60 * 45, P60, P60 * 9 / 10);
	CHECK(g.depth() == 1);
	CHECK(g.now - calm_start >= COMP_LW_LAG_DWELL_NS);
	(void)min_gap_between_lowerings;
}

TEST_CASE("lw depth: a re-raise soon after a lowering doubles the lag dwell")
{
	comp_lw_lag l;
	comp_lw_lag_init(&l);
	uint64_t now = S;
	auto feed = [&](int n, uint64_t lag) {
		bool changed = false;
		for (int i = 0; i < n; i++) {
			now += P60;
			changed |= comp_lw_lag_on_sample(&l, lag, P60, now);
		}
		return changed;
	};
	feed(60, 2 * P60);
	REQUIRE(l.depth == 2);
	feed(60 * 40, P60 / 2); // lowered after the 30 s dwell
	REQUIRE(l.depth == 1);
	CHECK(l.dwell_ns == COMP_LW_LAG_DWELL_NS);
	feed(60, 2 * P60); // straight back up: the probe failed
	CHECK(l.depth == 2);
	CHECK(l.dwell_ns == 2 * COMP_LW_LAG_DWELL_NS);
	// The next lowering now needs the doubled dwell.
	feed(60 * 40, P60 / 2);
	CHECK(l.depth == 2);
	feed(60 * 30, P60 / 2);
	CHECK(l.depth == 1);
}

TEST_CASE("lw depth: the raise threshold is floor + 0.5 periods, stalls are ignored")
{
	comp_lw_lag l;
	comp_lw_lag_init(&l);
	uint64_t now = S;
	for (int i = 0; i < 60; i++) {
		now += P60;
		comp_lw_lag_on_sample(&l, P60 * 14 / 10, P60, now);
	}
	CHECK(l.depth == 1); // 1.4 periods rounds to 1
	for (int i = 0; i < 60; i++) {
		now += P60;
		comp_lw_lag_on_sample(&l, P60 * 16 / 10, P60, now);
	}
	CHECK(l.depth == 2);                                        // 1.6 rounds to 2
	CHECK_FALSE(comp_lw_lag_on_sample(&l, 300 * MS, P60, now)); // a stall, not a lag
}

TEST_CASE("lw depth: slip test is 1.55 periods")
{
	CHECK_FALSE(comp_lw_lag_is_slip(P60, P60));
	CHECK_FALSE(comp_lw_lag_is_slip(25 * MS, P60));
	CHECK(comp_lw_lag_is_slip(2 * P60, P60));
	CHECK_FALSE(comp_lw_lag_is_slip(2 * P60, 0));
}

TEST_CASE("fb pace: a floor of 0 leaves the feedback-pacing behaviour unchanged")
{
	comp_fb_pace p;
	comp_fb_pace_init(&p, 1, true);
	CHECK(comp_fb_pace_effective(&p) == 1);
	CHECK_FALSE(comp_fb_pace_set_floor(&p, 0, S));
	CHECK(comp_fb_pace_set_floor(&p, 2, S));
	CHECK(comp_fb_pace_effective(&p) == 2);
	CHECK(comp_fb_pace_set_floor(&p, 1, 2 * S)); // a lowering is a probe
	CHECK(p.last_probe_ns == 2 * S);
}

TEST_CASE("lw depth: the intrinsic lag is the same at every depth")
{
	// A 2-period intrinsic lag, measured at depth 1 (sample 2 periods, nothing
	// queued), depth 2 (3 periods, 1 queued) and depth 3 (4 periods, 2 queued).
	for (uint32_t q = 0; q < 3; q++) {
		comp_lw_lag l;
		comp_lw_lag_init(&l);
		uint64_t now = S;
		for (int i = 0; i < 60; i++) {
			now += P60;
			comp_lw_lag_on_sample(&l, comp_lw_lag_intrinsic_ns((2 + q) * P60, q, P60), P60, now);
		}
		CHECK(l.depth == 2);
	}
	CHECK(comp_lw_lag_intrinsic_ns(P60, 2, P60) == 1); // fully explained by our queue
}

TEST_CASE("lw depth: the floor never feeds on our own depth (no runaway)")
{
	// The first CI build's runaway: every level of depth adds a period of FIFO
	// wait to the raw sample. Model exactly that, with the measured 1.76-period
	// intrinsic lag, and feed the governor's own depth back in as the queue.
	gov g;
	const uint64_t intrinsic = P60 * 176 / 100;
	int max_floor = 0;
	int max_depth = 0;
	for (int i = 0; i < 60 * 120; i++) {
		const uint32_t q = (uint32_t)(g.depth() - 1);
		g.frame(P60, intrinsic + q * P60, q);
		max_floor = g.lag.depth > max_floor ? g.lag.depth : max_floor;
		max_depth = g.depth() > max_depth ? g.depth() : max_depth;
	}
	CHECK(max_floor == 2);
	CHECK(max_depth == 2);
	CHECK(g.lag_changes == 1);
	// The intrinsic EMA reads the compositor, not the queue.
	CHECK(g.lag.ema_ns == Catch::Approx((double)intrinsic).epsilon(0.01));
}
