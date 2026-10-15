// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the present_wait depth governor (#2119): the depth
 *         floor (comp_late_weave_depth.h — raised by clean lag samples or
 *         starvation, lowered by slack) on top of the shared slip machine
 *         (comp_feedback_pace.h), and the queue drain after a raise.
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
	int raises = 0;
	int lowers = 0;
	int changes = 0;
	int at_cap = 0;
	uint64_t last_lower = 0;
	uint64_t min_lower_gap = UINT64_MAX;

	gov()
	{
		comp_fb_pace_init(&pace, 1, true);
		pace.ever_ok = true;
		pace.banked = 0; // the present_wait tier: no level above the floor
		comp_lw_lag_init(&lag);
	}

	int
	depth() const
	{
		return comp_fb_pace_effective(&pace);
	}

	/*!
	 * One paced frame: interval @p dt since the last one; a commit-to-glass
	 * sample @p lag_ns (0 = none) committed with @p outstanding of ours ahead;
	 * the present wait @p wait_ns.
	 */
	void
	frame(uint64_t dt, uint64_t lag_ns, uint32_t outstanding, uint64_t wait_ns)
	{
		now += dt;
		const int before = depth();
		if (lag_ns != 0 && comp_lw_lag_on_sample(&lag, lag_ns, outstanding, P60, now)) {
			raises++;
			comp_fb_pace_set_floor(&pace, lag.depth, now);
		}
		const uint64_t interval = last_frame != 0 ? now - last_frame : 0;
		const comp_lw_lag_event le = comp_lw_lag_on_frame(&lag, interval, wait_ns, P60, now);
		if (le == COMP_LW_LAG_LOWERED_SLACK) {
			lowers++;
			if (last_lower != 0 && now - last_lower < min_lower_gap) {
				min_lower_gap = now - last_lower;
			}
			last_lower = now;
		} else if (le == COMP_LW_LAG_RAISED_STARVED) {
			raises++;
		}
		if (le != COMP_LW_LAG_NONE) {
			comp_fb_pace_set_floor(&pace, lag.depth, now);
		}
		if (interval != 0 && comp_fb_pace_on_wait(&pace, !comp_lw_lag_is_slip(interval, P60), wait_ns, now) ==
		                         COMP_FB_PACE_AT_CAP) {
			at_cap++;
		}
		if (depth() != before) {
			changes++;
		}
		last_frame = now;
	}

	void
	run(int n, uint64_t dt, uint64_t lag_ns, uint32_t outstanding, uint64_t wait_ns)
	{
		for (int i = 0; i < n; i++) {
			frame(dt, lag_ns, outstanding, wait_ns);
		}
	}

	/*!
	 * A composited desktop with a pipelined 2-period compositor. At depth 1
	 * the loop serialises to 2 periods per frame and the wait is most of it;
	 * at depth 2 it runs at rate and the wait on N−1 is still ~0.7 period
	 * (round 4's measurement). Lag samples exist only at depth 1.
	 * @p time_at_1 accumulates the time spent at depth 1.
	 */
	void
	composited(int n, uint64_t *time_at_1 = nullptr)
	{
		for (int i = 0; i < n; i++) {
			const bool d1 = depth() == 1;
			const uint64_t dt = d1 ? 2 * P60 : P60;
			if (d1 && time_at_1 != nullptr) {
				*time_at_1 += dt;
			}
			frame(dt, 2 * P60, d1 ? 0 : 1, d1 ? P60 + P60 / 4 : P60 * 7 / 10);
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
	g.run(COMP_LW_LAG_MIN_SAMPLES, 2 * P60, P60 * 19 / 10, 0, P60);
	CHECK(g.lag.depth == 2);
	CHECK(g.depth() == 2);
	CHECK(comp_fb_pace_wait_target(&g.pace, 10) == 9);
}

TEST_CASE("lw depth: a direct-scanout target (sub-period lag) stays at depth 1")
{
	gov g;
	g.run(60 * 60, P60, P60 * 8 / 10, 0, P60 / 5);
	CHECK(g.depth() == 1);
	CHECK(g.changes == 0);
}

TEST_CASE("lw depth: lag samples taken behind our own presents are ignored")
{
	comp_lw_lag l;
	comp_lw_lag_init(&l);
	uint64_t now = S;
	for (int i = 0; i < 60; i++) {
		now += P60;
		CHECK_FALSE(comp_lw_lag_on_sample(&l, 6 * P60, 1 + (i % 3), P60, now));
	}
	CHECK(l.samples == 0);
	CHECK(l.depth == 1);
}

TEST_CASE("lw depth: round 4 replay — on a 2-period compositor depth 2 never lowers")
{
	// Round 4 (21c965e): the throughput probe lowered to 1 every dwell and a
	// revert left an extra frame queued. With slack as the lowering signal the
	// wait on N−1 (~0.7 period every frame at depth 2 here) never says
	// "already on glass", so depth 2 holds for good: only `1 -> 2`.
	gov g;
	uint64_t at1 = 0;
	g.composited(60 * 600, &at1); // 10 minutes
	CHECK(g.depth() == 2);
	CHECK(g.changes == 1);
	CHECK(g.lowers == 0);
	CHECK(at1 < 1 * S); // only the start-up frames before the first raise
}

TEST_CASE("lw depth: on a 1-period target the wait on N−1 is ~0, so it lowers to 1 after the dwell and stays")
{
	gov g;
	g.run(COMP_LW_LAG_MIN_SAMPLES, 2 * P60, 2 * P60, 0, P60); // a transient raised it to 2
	REQUIRE(g.depth() == 2);
	const uint64_t raised_at = g.now;
	// Now a direct-scanout target: at depth 2 the waited-on present is always
	// on glass already; at depth 1 the loop keeps the rate.
	for (int i = 0; i < 60 * 120; i++) {
		const bool d2 = g.depth() == 2;
		g.frame(P60, P60 * 8 / 10, d2 ? 1 : 0, d2 ? 0 : P60 / 5);
	}
	CHECK(g.depth() == 1);
	CHECK(g.lowers == 1);
	CHECK(g.last_lower - raised_at >= COMP_LW_DWELL_NS);
	CHECK(g.last_lower - raised_at <= 2 * COMP_LW_DWELL_NS + S);
}

TEST_CASE("lw depth: a wrong lowering is caught by starvation within ~1 s and the dwell doubles")
{
	// An app throttled elsewhere (waits ~0 at depth 2) on a compositor where
	// depth 1 serialises: slack lowers, starvation raises, and the doubling
	// dwell makes each such lowering rarer. No lag samples at all.
	gov g;
	g.run(COMP_LW_STARVE_WINDOW + 1, 2 * P60, 0, 0, P60 + P60 / 4); // starvation: 1 -> 2
	REQUIRE(g.depth() == 2);
	uint64_t longest_at_1 = 0, spell = 0;
	for (int i = 0; i < 60 * 600; i++) {
		const bool d1 = g.depth() == 1;
		g.frame(d1 ? 2 * P60 : P60, 0, 0, d1 ? P60 + P60 / 4 : 0);
		if (d1) {
			spell += d1 ? 2 * P60 : 0;
			longest_at_1 = spell > longest_at_1 ? spell : longest_at_1;
		} else {
			spell = 0;
		}
	}
	CHECK(g.depth() == 2);
	CHECK(longest_at_1 <= 1100 * MS);
	CHECK(g.lag.lowers_reverted == (uint32_t)g.lowers);
	CHECK(g.lowers <= 7);                           // 5, 10, 20, 40, 80, 160, 300 s
	CHECK(g.lag.dwell_ns >= 32 * COMP_LW_DWELL_NS); // doubled every time
	CHECK(g.min_lower_gap >= 2 * COMP_LW_DWELL_NS);
}

TEST_CASE("lw depth: depth-1 starvation raises to 2 without a lag sample")
{
	gov g;
	g.run(COMP_LW_STARVE_WINDOW + 1, 2 * P60, 0, 0, P60 + P60 / 4);
	CHECK(g.depth() == 2);
	CHECK(g.raises == 1);
}

TEST_CASE("lw depth: a render-bound slipper at depth 1 is not starvation")
{
	gov g;
	// 25 ms frames on a 60 Hz FIFO, wait short: more depth buys nothing.
	for (int i = 0; i <= COMP_FB_PACE_WINDOW * 3; i++) {
		g.frame((i & 1) ? 2 * P60 : P60, P60 * 8 / 10, 0, 2 * MS);
	}
	CHECK(g.depth() == 1);
	CHECK(g.at_cap == 3);
}

TEST_CASE("lw depth: the drain brings our outstanding presents back to depth - 1 within one frame")
{
	CHECK(comp_lw_lag_drain_tag(10, 9, 2) == 0); // 1 outstanding at depth 2: fine
	CHECK(comp_lw_lag_drain_tag(10, 8, 2) == 9); // 2 outstanding: wait for tag 9
	CHECK(comp_lw_lag_drain_tag(10, 7, 1) == 10);
	CHECK(comp_lw_lag_drain_tag(10, 10, 1) == 0);
	CHECK(comp_lw_lag_drain_tag(0, 0, 2) == 0);

	// Round 4's leftover: a revert to depth 2 found 2 of ours outstanding. The
	// next admission drains to depth - 1 = 1, and the queue then stays there.
	uint64_t last = 100, answered = 98;
	const int depth = 2;
	for (int f = 0; f < 10; f++) {
		const uint64_t tag = comp_lw_lag_drain_tag(last, answered, depth);
		if (tag != 0) {
			answered = tag; // the feedback wait returns once tag is answered
		}
		CHECK(last - answered <= (uint64_t)(depth - 1)); // at admission
		last++;                                          // commit
		answered++;                                      // one present reaches glass per period
	}
}

TEST_CASE("lw depth: a lag flipping 1 -> 2 -> 1 periods never flaps faster than the dwell")
{
	// mutter's dynamic buffering flips the compositor between a 1- and a
	// 2-period pipeline in 5 s spells (measured 59, 34, 97, 60, ... present/s).
	gov g;
	for (int spell = 0; spell < 120; spell++) { // 10 minutes
		const bool two = (spell & 1) != 0;
		for (int i = 0; i < 5 * 60; i++) {
			const bool d1 = g.depth() == 1;
			const uint64_t dt = (d1 && two) ? 2 * P60 : P60;
			const uint64_t lag = two ? 2 * P60 : P60 * 9 / 10;
			const uint64_t wait = d1 ? (two ? P60 + P60 / 4 : P60 / 5) : (two ? P60 * 7 / 10 : 0);
			g.frame(dt, lag, d1 ? 0 : 1, wait);
		}
	}
	CHECK(g.min_lower_gap >= COMP_LW_DWELL_NS);
	CHECK(g.changes <= 1 + 2 * (int)(600 * S / COMP_LW_DWELL_NS));
	CHECK(g.lag.dwell_ns >= COMP_LW_DWELL_NS);
}

TEST_CASE("lw depth: the raise threshold is floor + 0.5 periods, stalls are ignored")
{
	comp_lw_lag l;
	comp_lw_lag_init(&l);
	uint64_t now = S;
	for (int i = 0; i < 60; i++) {
		now += P60;
		comp_lw_lag_on_sample(&l, P60 * 14 / 10, 0, P60, now);
	}
	CHECK(l.depth == 1); // 1.4 periods rounds to 1
	for (int i = 0; i < 60; i++) {
		now += P60;
		comp_lw_lag_on_sample(&l, P60 * 16 / 10, 0, P60, now);
	}
	CHECK(l.depth == 2);                                           // 1.6 rounds to 2
	CHECK_FALSE(comp_lw_lag_on_sample(&l, 300 * MS, 0, P60, now)); // a stall, not a lag
}

TEST_CASE("lw depth: one sample moves the EMA by at most 0.05 period")
{
	comp_lw_lag l;
	comp_lw_lag_init(&l);
	uint64_t now = S;
	for (int i = 0; i < 60; i++) {
		now += P60;
		comp_lw_lag_on_sample(&l, 2 * P60, 0, P60, now);
	}
	const double before = l.ema_ns;
	comp_lw_lag_on_sample(&l, 150 * MS, 0, P60, now); // a 9-period outlier
	CHECK(l.ema_ns - before <= 0.05 * (double)P60 + 1.0);
}

TEST_CASE("lw depth: replay of the second CI build's log stays at depth <= 2")
{
	// 2.44.2+4: intrinsic lag ~1.72 periods; raw samples carried 0..3 of our
	// presents outstanding; 58 fps with bursty slip windows from GPU jitter;
	// a few multi-period stalls. That build went 1 -> 2 -> 3 -> 2 -> 3 -> 4.
	gov g;
	const uint64_t intrinsic = P60 * 172 / 100;
	uint32_t rng = 12345;
	auto next = [&]() {
		rng = rng * 1103515245u + 12345u;
		return (rng >> 16) & 0x7fff;
	};
	int max_depth = 0;
	double min_ema = 1e30, max_ema = 0;
	for (int i = 0; i < 60 * 150; i++) {
		const uint32_t outstanding = next() % 4;
		uint64_t raw = intrinsic + outstanding * P60 + (uint64_t)(next() % 3000) * 1000;
		if (next() % 200 == 0) {
			raw += 4 * P60;
		}
		const bool burst = (i / COMP_FB_PACE_WINDOW) % 3 == 1;
		const bool slip = burst ? (i % 10 == 0) : (next() % 30 == 0);
		g.frame(slip ? 2 * P60 : P60, raw, outstanding, P60 / 2);
		max_depth = g.depth() > max_depth ? g.depth() : max_depth;
		if (g.lag.samples >= COMP_LW_LAG_MIN_SAMPLES) {
			const double e = g.lag.ema_ns / (double)P60;
			min_ema = e < min_ema ? e : min_ema;
			max_ema = e > max_ema ? e : max_ema;
		}
	}
	CHECK(max_depth == 2);
	CHECK(g.depth() == 2);
	CHECK(min_ema >= 1.5);
	CHECK(max_ema <= 2.5);
	CHECK(g.at_cap > 0);
}

TEST_CASE("lw depth: slip test is 1.55 periods")
{
	CHECK_FALSE(comp_lw_lag_is_slip(P60, P60));
	CHECK_FALSE(comp_lw_lag_is_slip(25 * MS, P60));
	CHECK(comp_lw_lag_is_slip(2 * P60, P60));
	CHECK_FALSE(comp_lw_lag_is_slip(2 * P60, 0));
}

TEST_CASE("lw depth: the feedback-pacing default still banks one level")
{
	comp_fb_pace p;
	comp_fb_pace_init(&p, 1, true);
	CHECK(p.banked == 1);
	uint64_t now = 10 * S;
	for (int i = 0; i < COMP_FB_PACE_WINDOW; i++) {
		now += P60;
		comp_fb_pace_on_wait(&p, i == 0, 0, now); // one success, then slips
	}
	CHECK(comp_fb_pace_effective(&p) == 2);
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
