// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the present_wait depth governor (#2119): the depth
 *         floor (comp_late_weave_depth.h — raised by clean lag samples and
 *         depth-1 starvation, lowered only by a throughput-verified probe) on
 *         top of the shared slip machine (comp_feedback_pace.h).
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
	int at_cap = 0;
	int probes_down = 0;
	int probes_failed = 0;
	int probes_held = 0;
	uint64_t last_probe_down = 0;
	uint64_t min_probe_gap = UINT64_MAX;
	uint64_t last_fail_revert_ns = 0; //!< probe -> revert time of the last failure

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
	 * the acquire wait @p wait_ns.
	 */
	void
	frame(uint64_t dt, uint64_t lag_ns, uint32_t outstanding = 0, uint64_t wait_ns = 0)
	{
		now += dt;
		if (lag_ns != 0 && comp_lw_lag_on_sample(&lag, lag_ns, outstanding, P60, now)) {
			if (lag.probes_failed > (uint32_t)probes_failed) {
				probes_failed = (int)lag.probes_failed;
				last_fail_revert_ns = now - last_probe_down;
			}
			raises++;
			comp_fb_pace_set_floor(&pace, lag.depth, now);
		}
		const uint64_t interval = last_frame != 0 ? now - last_frame : 0;
		const comp_lw_lag_event le = comp_lw_lag_on_frame(&lag, interval, wait_ns, P60, now);
		if (le == COMP_LW_LAG_PROBE_DOWN) {
			probes_down++;
			if (last_probe_down != 0 && now - last_probe_down < min_probe_gap) {
				min_probe_gap = now - last_probe_down;
			}
			last_probe_down = now;
		} else if (le == COMP_LW_LAG_PROBE_FAILED) {
			probes_failed = (int)lag.probes_failed;
			last_fail_revert_ns = now - last_probe_down;
		} else if (le == COMP_LW_LAG_PROBE_HELD) {
			probes_held++;
		}
		if (le != COMP_LW_LAG_NONE) {
			comp_fb_pace_set_floor(&pace, lag.depth, now);
		}
		if (interval != 0) {
			const comp_fb_pace_event e =
			    comp_fb_pace_on_wait(&pace, !comp_lw_lag_is_slip(interval, P60), wait_ns, now);
			if (e == COMP_FB_PACE_AT_CAP) {
				at_cap++;
				if (comp_lw_lag_on_slip_window_at_cap(&lag, P60, now)) {
					raises++;
					comp_fb_pace_set_floor(&pace, lag.depth, now);
				}
			}
		}
		last_frame = now;
	}

	void
	run(int n, uint64_t dt, uint64_t lag_ns, uint32_t outstanding = 0, uint64_t wait_ns = 0)
	{
		for (int i = 0; i < n; i++) {
			frame(dt, lag_ns, outstanding, wait_ns);
		}
	}

	/*!
	 * A composited desktop with a PIPELINED 2-period compositor: at depth 1 the
	 * loop serialises to 2 periods per frame; at depth >= 2 it runs at rate.
	 * Lag samples exist only at depth 1 (at 2 one of ours is always outstanding,
	 * and the raw sample still reads 2 periods). @p with_lag false = no clean
	 * samples at all (e.g. X11 with no wait that blocked).
	 */
	void
	composited(int n, bool with_lag, uint64_t *under_half_ns = nullptr)
	{
		for (int i = 0; i < n; i++) {
			const bool d1 = depth() == 1;
			const uint64_t dt = d1 ? 2 * P60 : P60;
			if (d1 && under_half_ns != nullptr) {
				*under_half_ns += dt;
			}
			frame(dt, with_lag || !d1 ? 2 * P60 : 0, d1 ? 0 : 1, d1 ? P60 + P60 / 4 : P60 / 4);
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
	g.run(COMP_LW_LAG_MIN_SAMPLES, 2 * P60, P60 * 19 / 10);
	CHECK(g.lag.depth == 2);
	CHECK(g.depth() == 2);
	CHECK(comp_fb_pace_wait_target(&g.pace, 10) == 9);
}

TEST_CASE("lw depth: a direct-scanout target (sub-period lag) stays at depth 1")
{
	gov g;
	g.run(60 * 60, P60, P60 * 8 / 10);
	CHECK(g.lag.depth == 1);
	CHECK(g.depth() == 1);
	CHECK(g.probes_down == 0);
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

TEST_CASE("lw depth: round 3 replay — the floor never falls by lag, a failed probe reverts fast")
{
	// Round 3 on a198c95: depth 2 at rate (residual 1.99), raw samples of 2
	// periods with 1 of ours outstanding; the subtracting build read that as
	// 0.95 periods, dropped to 1 and sat at 30 present/s for ~2 minutes.
	gov g;
	g.composited(60 * 5, true); // rises to 2 from the clean depth-1 samples
	REQUIRE(g.depth() == 2);

	// 10 minutes. Lag samples are never allowed to lower it; only probes do,
	// each one must be reverted within ~1 s, the dwell must double, and the
	// app must never spend more than ~2 s under half its pre-probe rate.
	uint64_t under_half = 0;
	int fails_before = g.probes_failed;
	for (int i = 0; i < 600; i++) {
		const int before = g.probes_failed;
		uint64_t spell = 0;
		g.composited(60, true, &spell);
		under_half += spell;
		CHECK(spell <= 2 * S);
		if (g.probes_failed > before) {
			CHECK(g.last_fail_revert_ns <= 1100 * MS);
		}
	}
	CHECK(g.probes_down >= 3);
	CHECK(g.probes_failed - fails_before == g.probes_down);
	CHECK(g.probes_held == 0);
	CHECK(g.depth() == 2);
	// 30 -> 60 -> 120 -> 300 s: probes get rarer, never more often than 30 s.
	CHECK(g.min_probe_gap >= COMP_LW_PROBE_DWELL_NS);
	CHECK(g.lag.dwell_ns >= 4 * COMP_LW_PROBE_DWELL_NS);
	CHECK(under_half <= 2 * S * (uint64_t)g.probes_down);
}

TEST_CASE("lw depth: without lag samples a probe is reverted by throughput alone")
{
	gov g;
	g.composited(60 * 5, true);
	REQUIRE(g.depth() == 2);
	// From here no clean lag sample ever arrives: the revert must come from
	// the throughput check.
	uint64_t under_half = 0;
	g.composited(60 * 40, false, &under_half);
	CHECK(g.probes_down == 1);
	CHECK(g.probes_failed == 1);
	CHECK(g.depth() == 2);
	CHECK(g.lag.dwell_ns == 2 * COMP_LW_PROBE_DWELL_NS);
	CHECK(g.last_fail_revert_ns <= 1100 * MS);
	CHECK(under_half <= 1100 * MS);
}

TEST_CASE("lw depth: a probe that keeps the rate holds the lower depth")
{
	gov g;
	g.run(COMP_LW_LAG_MIN_SAMPLES, 2 * P60, 2 * P60); // transient: floor 2
	REQUIRE(g.depth() == 2);
	// The compositor went to direct scanout: depth 1 runs at rate, lag 0.8.
	g.run(60 * 35, P60, 0);
	CHECK(g.probes_down == 1);
	CHECK(g.probes_held == 1);
	CHECK(g.depth() == 1);
}

TEST_CASE("lw depth: depth-1 starvation (slipping, wait-bound) raises to 2 without a lag sample")
{
	gov g;
	g.run(COMP_FB_PACE_WINDOW + 1, 2 * P60, 0, 0, P60 + P60 / 4);
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

TEST_CASE("lw depth: with a floor of 2, slipping windows stay at 2")
{
	gov g;
	g.run(COMP_LW_LAG_MIN_SAMPLES, 2 * P60, 2 * P60);
	REQUIRE(g.depth() == 2);
	g.run(COMP_FB_PACE_WINDOW * 4, 2 * P60, 2 * P60, 1);
	CHECK(g.depth() == 2);
	CHECK(g.at_cap >= 3);
}

TEST_CASE("lw depth: a lag flipping 1 -> 2 -> 1 periods never flaps faster than the dwell")
{
	// mutter's dynamic buffering: the compositor flips between a 1- and a
	// 2-period pipeline in 5 s spells (measured 59, 34, 97, 60, ... present/s).
	gov g;
	int changes = 0;
	int prev = g.depth();
	for (int spell = 0; spell < 120; spell++) { // 10 minutes
		const bool two = (spell & 1) != 0;
		for (int i = 0; i < 5 * 60; i++) {
			const bool d1 = g.depth() == 1;
			const uint64_t dt = (d1 && two) ? 2 * P60 : P60;
			const uint64_t lag = two ? 2 * P60 : P60 * 9 / 10;
			g.frame(dt, lag, d1 ? 0 : 1, d1 && two ? P60 : P60 / 4);
			if (g.depth() != prev) {
				changes++;
				prev = g.depth();
			}
		}
	}
	CHECK(g.depth() >= 1);
	// It rose on the first 2-period spell; every lowering since is a probe, and
	// probes are at least a dwell apart.
	CHECK(g.min_probe_gap >= COMP_LW_PROBE_DWELL_NS);
	// 10 minutes of 5 s flips: one rise plus at most a probe and its outcome per dwell.
	CHECK(changes <= 1 + 2 * (int)(600 * S / COMP_LW_PROBE_DWELL_NS));
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
		g.frame(slip ? 2 * P60 : P60, raw, outstanding, P60 / 4);
		max_depth = g.depth() > max_depth ? g.depth() : max_depth;
		if (g.lag.samples >= COMP_LW_LAG_MIN_SAMPLES) {
			const double e = g.lag.ema_ns / (double)P60;
			min_ema = e < min_ema ? e : min_ema;
			max_ema = e > max_ema ? e : max_ema;
		}
	}
	CHECK(max_depth == 2);
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
