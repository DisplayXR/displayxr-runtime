// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the grid tier's AIM/THROUGHPUT pacing controller and
 *         its weave budget (#2629), alone and wired to the rate cap and the
 *         queue drain the way the vk_native target wires them.
 */

#include "catch_amalgamated.hpp"

#include "util/comp_grid_pace_mode.h"
#include "util/comp_grid_rate_cap.h"
#include "util/comp_queue_drain.h"

#include <deque>
#include <functional>
#include <utility>

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t S = 1000ULL * MS;
static constexpr uint64_t P60 = 16666666ULL;
static constexpr uint64_t P90 = 11111111ULL;

TEST_CASE("weave budget: p90 of the last 32 — one long frame does not move it")
{
	comp_weave_budget b;
	comp_weave_budget_reset(&b);
	for (int i = 0; i < 40; i++) {
		comp_weave_budget_add(&b, 3 * MS + (uint64_t)(i % 4) * 100000ULL);
	}
	const uint64_t steady = b.p90_ns;
	CHECK(steady <= 3 * MS + 300000ULL);
	// One 30 ms frame (a high-water would have jumped to 30 ms).
	comp_weave_budget_add(&b, 30 * MS);
	CHECK(b.p90_ns == steady);
	// Three of them still do not.
	comp_weave_budget_add(&b, 30 * MS);
	comp_weave_budget_add(&b, 30 * MS);
	CHECK(b.p90_ns <= 3 * MS + 300000ULL);
	// A sustained rise does.
	for (int i = 0; i < 16; i++) {
		comp_weave_budget_add(&b, 8 * MS);
	}
	CHECK(b.p90_ns >= 8 * MS);
}

TEST_CASE("pace mode: window verdicts")
{
	comp_grid_pace p;
	comp_grid_pace_reset(&p);
	uint64_t now = 10 * S;

	SECTION("a slow app that is never late stays in AIM")
	{
		// One weave every two vblanks, none late.
		for (int i = 0; i < 300; i++) {
			now += 2 * P60;
			CHECK(comp_grid_pace_on_weave(&p, false, now, P60) == COMP_PACE_EV_NONE);
		}
		CHECK(p.mode == COMP_PACE_AIM);
		CHECK(p.aim_rate == Catch::Approx(0.5).margin(0.05));
	}
	SECTION("late and slow: probe; a gain proves it, no gain rejects it with backoff")
	{
		int ev_probe = 0;
		for (int i = 0; i < 100 && p.mode == COMP_PACE_AIM; i++) {
			now += 2 * P60;
			ev_probe += comp_grid_pace_on_weave(&p, true, now, P60) == COMP_PACE_EV_PROBE ? 1 : 0;
		}
		REQUIRE(ev_probe == 1);
		REQUIRE(p.mode == COMP_PACE_THROUGHPUT);
		REQUIRE(p.probing);

		SECTION("THROUGHPUT is faster: proven, stays")
		{
			comp_grid_pace_event last = COMP_PACE_EV_NONE;
			for (int i = 0; i < 400; i++) {
				now += (P60 * 4) / 3; // 0.75 of the vblanks
				const comp_grid_pace_event e = comp_grid_pace_on_weave(&p, true, now, P60);
				if (e != COMP_PACE_EV_NONE) {
					last = e;
				}
			}
			CHECK(last == COMP_PACE_EV_PROVEN);
			CHECK(p.mode == COMP_PACE_THROUGHPUT);
			CHECK(comp_grid_pace_on_period_change(&p) == COMP_PACE_EV_RESET_AIM);
			CHECK(p.mode == COMP_PACE_AIM);
		}
		SECTION("THROUGHPUT is no faster: rejected, backoff 5 s then 10 s")
		{
			comp_grid_pace_event e = COMP_PACE_EV_NONE;
			for (int i = 0; i < 100 && e == COMP_PACE_EV_NONE; i++) {
				now += 2 * P60;
				e = comp_grid_pace_on_weave(&p, true, now, P60);
			}
			CHECK(e == COMP_PACE_EV_REJECTED);
			CHECK(p.mode == COMP_PACE_AIM);
			const uint64_t until = p.backoff_until_ns;
			CHECK(until == now + COMP_PACE_BACKOFF_BASE_NS);
			// No probe before the backoff expires.
			while (now + 2 * P60 < until) {
				now += 2 * P60;
				REQUIRE(comp_grid_pace_on_weave(&p, true, now, P60) != COMP_PACE_EV_PROBE);
			}
			CHECK(p.backoff_ns == 2 * COMP_PACE_BACKOFF_BASE_NS);
		}
	}
	SECTION("a pause starts a fresh window instead of reading as a slow rate")
	{
		for (int i = 0; i < 30; i++) {
			now += P60;
			comp_grid_pace_on_weave(&p, true, now, P60);
		}
		now += 3 * S;
		for (int i = 0; i < 70; i++) {
			now += P60;
			CHECK(comp_grid_pace_on_weave(&p, true, now, P60) == COMP_PACE_EV_NONE);
		}
		CHECK(p.mode == COMP_PACE_AIM);
	}
}

/*!
 * The whole grid tier on one producer: pace mode, rate cap, queue drain and
 * the p90 budget, wired as comp_vk_native_target.cpp wires them, over a FIFO
 * display. A frame is shown at its booked vblank plus @ref latency (the
 * display pipeline); its #206 residual is shown + latency - weave start.
 */
struct tier
{
	uint64_t period;
	uint64_t latency;
	comp_vblank_grid grid = {};
	comp_grid_pace pace;
	comp_queue_drain qd;
	comp_weave_budget budget;
	uint64_t shown = 0;
	uint64_t now = 0;
	uint64_t seq = 1;
	std::deque<std::pair<uint64_t, uint64_t>> inflight; // (seq, residual)
	size_t lag = 4;

	// Tallies.
	uint64_t weaves = 0;
	uint64_t drains = 0;
	uint64_t mode_changes = 0;
	uint64_t now_weaves = 0;
	uint64_t t0 = 0;
	double res_sum = 0;
	uint64_t res_n = 0;

	tier(uint64_t p, uint64_t lat) : period(p), latency(lat)
	{
		comp_vblank_grid_set_period(&grid, p);
		comp_vblank_grid_observe(&grid, 1000 * MS);
		comp_grid_pace_reset(&pace);
		comp_queue_drain_reset(&qd);
		comp_weave_budget_reset(&budget);
		now = 1000 * MS + p + MS;
		t0 = now;
	}

	//! One weave of cost @p cost_ns, starting no earlier than now.
	void
	weave(uint64_t cost_ns)
	{
		const uint64_t budget_ns = (budget.n ? budget.p90_ns : cost_ns) + 1500 * 1000;
		uint64_t sleep_ns = 0;
		comp_grid_rate_cap_kind k = COMP_GRID_CAP_NONE;
		const bool thr = pace.mode == COMP_PACE_THROUGHPUT;
		REQUIRE(comp_grid_rate_cap_target(&grid, now, budget_ns, shown, thr, &sleep_ns, &k) != 0);
		now += sleep_ns;
		if (comp_queue_drain_take(&qd, seq, now, nullptr)) {
			drains++;
			now += period;
		}
		const uint64_t start = now;
		now += cost_ns;
		comp_weave_budget_add(&budget, cost_ns);
		shown = comp_grid_rate_cap_on_present(&grid, shown, now);
		comp_vblank_grid_observe(&grid, comp_vblank_grid_next_vblank_after(&grid, now));
		weaves++;
		now_weaves += k == COMP_GRID_CAP_NOW ? 1 : 0;
		const bool late = k == COMP_GRID_CAP_LATE || k == COMP_GRID_CAP_NOW;
		const comp_grid_pace_event e = comp_grid_pace_on_weave(&pace, late, now, period);
		if (e == COMP_PACE_EV_PROBE || e == COMP_PACE_EV_REJECTED || e == COMP_PACE_EV_RESET_AIM) {
			mode_changes++;
			comp_queue_drain_on_mode_change(&qd, pace.mode == COMP_PACE_THROUGHPUT);
		}
		const uint64_t residual = shown + latency - start;
		inflight.emplace_back(seq++, residual);
		while (inflight.size() > lag) {
			const auto smp = inflight.front();
			inflight.pop_front();
			comp_queue_drain_on_residual(&qd, smp.first, smp.second, period, now);
			if (now - t0 > 3 * S) { // past the warm-up
				res_sum += (double)smp.second;
				res_n++;
			}
		}
	}

	double
	rate() const
	{
		return (double)weaves / ((double)(now - t0) / 1e9);
	}
	double
	mean_residual_ms() const
	{
		return res_n ? res_sum / (double)res_n / 1e6 : 0.0;
	}
};

TEST_CASE("pace mode: a fast app at 60 Hz stays in AIM at the panel rate, no drains")
{
	tier t(P60, 15 * MS); // floor ~19 ms = 1.15 periods
	while (t.now - t.t0 < 30 * S) {
		t.now += 6 * MS; // app work
		t.weave(3 * MS);
	}
	CHECK(t.pace.mode == COMP_PACE_AIM);
	CHECK(t.mode_changes == 0);
	CHECK(t.rate() == Catch::Approx(60.0).margin(1.5));
	CHECK(t.drains == 0);
	CHECK(t.now_weaves == 0);
	CHECK(t.mean_residual_ms() < 21.0);
}

TEST_CASE("pace mode: render + weave over the period at 90 Hz moves to THROUGHPUT and stays")
{
	tier t(P90, 11 * MS);
	while (t.now - t.t0 < 30 * S) {
		t.now += 7 * MS;
		t.weave(5 * MS);
	}
	CHECK(t.pace.mode == COMP_PACE_THROUGHPUT);
	CHECK_FALSE(t.pace.probing);
	CHECK(t.pace.probes == 1);
	CHECK(t.rate() > 75.0);
	// Early "now" weaves sit ~2 periods with an empty queue: no drain loop.
	CHECK(t.drains <= 1);
}

TEST_CASE("pace mode: a 45 fps content-limited app on a 60 Hz panel does not sit in THROUGHPUT")
{
	tier t(P60, 15 * MS);
	const uint64_t frame = S / 45;
	uint64_t release = t.now;
	uint64_t thr_time = 0;
	uint64_t last = t.now;
	while (t.now - t.t0 < 60 * S) {
		release += frame;
		if (t.now < release) {
			t.now = release; // the content's own clock
		}
		t.now += 3 * MS;
		t.weave(3 * MS);
		if (t.pace.mode == COMP_PACE_THROUGHPUT) {
			thr_time += t.now - last;
		}
		last = t.now;
	}
	CHECK(t.rate() == Catch::Approx(45.0).margin(1.5));
	// Probes back off 5, 10, 20, 40 s: a handful in a minute, each a second.
	CHECK(t.pace.probes <= 5);
	CHECK(thr_time < 6 * S);
	CHECK(t.pace.mode == COMP_PACE_AIM);
}

TEST_CASE("pace mode: the round-3 oscillation — early weaves at ~2 periods do not trigger a drain")
{
	// Round 3 on the device: aimed weaves at 1.15 periods taught the drain a
	// passive floor; THROUGHPUT's early weaves at ~2 periods then read as a
	// backlog over 1.15 + 0.75, the drain skipped a vblank, the loop re-phased
	// for a few frames and slipped back — 26 drains in 25 s.
	auto feed = [](comp_queue_drain *d, bool through_mode) {
		comp_queue_drain_reset(d);
		uint64_t now = 10 * S;
		uint64_t seq = 1;
		// Aimed phase: the floor is learned.
		for (int i = 0; i < 12; i++) {
			comp_queue_drain_on_residual(d, seq++, (uint64_t)(1.15 * P60), P60, now);
		}
		if (through_mode) {
			comp_queue_drain_on_mode_change(d, true);
		}
		// Mostly "now" weaves, ~2 periods with an empty queue.
		int armed = 0;
		for (int i = 0; i < 300; i++) {
			now += P60;
			const double per = (i % 5 == 0) ? 1.2 : 2.05;
			comp_queue_drain_on_residual(d, seq++, (uint64_t)(per * P60), P60, now);
			armed += d->pending ? 1 : 0;
			if (comp_queue_drain_take(d, seq, now, nullptr)) {
				// (the test only counts; a real drain would re-phase)
			}
		}
		return armed;
	};
	comp_queue_drain d;
	CHECK(feed(&d, true) == 0); // THROUGHPUT: absolute threshold only
	CHECK(comp_queue_drain_threshold(&d) == COMP_QD_DEFAULT_THRESHOLD);
	// The same feed with the round-3 rule (AIM floor kept) does arm — the
	// test above is not passing for want of a trigger.
	comp_queue_drain d2;
	CHECK(feed(&d2, false) > 0);
}

TEST_CASE("pace mode: in the full tier, THROUGHPUT's early weaves do not feed a drain loop")
{
	// Like the 90 Hz case, but on a 60 Hz panel with an app that only just
	// misses the period (render 10 ms + weave 8 ms > 16.7 ms).
	tier t(P60, 15 * MS);
	while (t.now - t.t0 < 30 * S) {
		t.now += 10 * MS;
		t.weave(8 * MS);
	}
	CHECK(t.pace.mode == COMP_PACE_THROUGHPUT);
	CHECK(t.drains <= 1);
}
