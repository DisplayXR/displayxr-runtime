// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the grid late-weave FIFO queue drain (#2629).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_queue_drain.h"

#include <deque>
#include <utility>
#include <vector>

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t S = 1000ULL * MS;
static constexpr uint64_t P60 = 16666666ULL;
static constexpr uint64_t P90 = 11111111ULL;

static uint64_t
periods(double n, uint64_t p = P60)
{
	return (uint64_t)(n * (double)p);
}

/*!
 * Feed the three post-settle samples a verdict is judged on (their min),
 * starting at @p seq. The first two must not produce a verdict.
 */
static comp_queue_drain_verdict
settle3(comp_queue_drain *d, uint64_t &seq, uint64_t a, uint64_t b, uint64_t c, uint64_t now, uint64_t p = P60)
{
	CHECK(comp_queue_drain_on_residual(d, seq++, a, p, now) == COMP_QD_VERDICT_NONE);
	CHECK(comp_queue_drain_on_residual(d, seq++, b, p, now) == COMP_QD_VERDICT_NONE);
	return comp_queue_drain_on_residual(d, seq++, c, p, now);
}

/*!
 * The grid tier as the vk_native target wires it, against a FIFO model:
 * the residual of a present is (floor + queued slots) periods, a drain lowers
 * the queue by one slot when there is one, and residual samples arrive
 * @ref lag presents late.
 */
struct sim
{
	comp_queue_drain d;
	uint64_t period = P60;
	double floor = 2.0;
	int queued = 0;
	uint64_t now = 10 * S;
	uint64_t seq = 1;
	//! Alternating +/- jitter on every residual sample, ns.
	uint64_t jitter = 0;
	size_t lag = 4;
	std::deque<std::pair<uint64_t, uint64_t>> inflight;
	std::vector<comp_queue_drain_reason> taken;
	std::vector<comp_queue_drain_verdict> verdicts;

	sim()
	{
		comp_queue_drain_reset(&d);
	}

	void
	frame()
	{
		now += period;
		comp_queue_drain_reason r = COMP_QD_REASON_NONE;
		if (comp_queue_drain_take(&d, seq, now, &r)) {
			taken.push_back(r);
			now += period; // the skipped vblank
			if (queued > 0) {
				queued--;
			}
		}
		uint64_t res = periods(floor + queued, period);
		res = (seq & 1) ? res + jitter : res - jitter;
		inflight.emplace_back(seq, res);
		seq++;
		while (inflight.size() > lag) {
			const auto s = inflight.front();
			inflight.pop_front();
			const comp_queue_drain_verdict v =
			    comp_queue_drain_on_residual(&d, s.first, s.second, period, now);
			if (v != COMP_QD_VERDICT_NONE) {
				verdicts.push_back(v);
			}
		}
	}

	void
	run(int n)
	{
		for (int i = 0; i < n; i++) {
			frame();
		}
	}

	void
	run_for(uint64_t ns)
	{
		const uint64_t end = now + ns;
		while (now < end) {
			frame();
		}
	}
};

TEST_CASE("queue drain: 90->60 fill drains one slot per settle, then stops at the floor")
{
	sim s;
	s.floor = 2.8;         // above the 2.5 default threshold: the last probe must fail
	s.queued = 3;          // filled while the tier kept the 11.1 ms cadence
	comp_queue_drain_on_period_change(&s.d, P90, P60, s.now);
	CHECK(s.d.pending);
	CHECK(s.d.pending_reason == COMP_QD_REASON_RATE_DROP);

	s.run(300);
	REQUIRE(s.taken.size() == 4);
	CHECK(s.taken[0] == COMP_QD_REASON_RATE_DROP);
	CHECK(s.taken[1] == COMP_QD_REASON_RESIDUAL);
	CHECK(s.taken[2] == COMP_QD_REASON_RESIDUAL);
	CHECK(s.taken[3] == COMP_QD_REASON_RESIDUAL);
	REQUIRE(s.verdicts.size() == 4);
	// No 60 Hz sample yet when the rate drop was taken: judged leniently,
	// not against the stale 90 Hz value (which would have "failed" it).
	CHECK(s.verdicts[0] == COMP_QD_VERDICT_LENIENT);
	CHECK(s.verdicts[1] == COMP_QD_VERDICT_DRAINED);
	CHECK(s.verdicts[2] == COMP_QD_VERDICT_DRAINED);
	CHECK(s.verdicts[3] == COMP_QD_VERDICT_NOT_QUEUE);
	CHECK(s.queued == 0);
	CHECK(s.d.floor_periods == Catch::Approx(2.8).margin(0.01));
	CHECK(s.d.state == COMP_QD_BACKOFF);

	// Past the backoff and well beyond: the floor holds, no further drains.
	s.run_for(30 * S);
	CHECK(s.taken.size() == 4);
	CHECK(s.d.state == COMP_QD_IDLE);
}

TEST_CASE("queue drain: a rate drop with a 60 Hz sample in hand is judged against it")
{
	sim s;
	s.floor = 2.0;
	s.queued = 2;
	comp_queue_drain_on_period_change(&s.d, P90, P60, s.now);
	// One post-change sample arrives before the take.
	comp_queue_drain_on_residual(&s.d, 0, periods(4.0), P60, s.now);
	s.run(200);
	REQUIRE(s.verdicts.size() >= 1);
	CHECK(s.verdicts[0] == COMP_QD_VERDICT_DRAINED);
	CHECK(s.queued == 0);
	// 2.0 periods is under the default threshold: nothing probes the floor.
	CHECK(s.taken.size() == 2);
	CHECK(s.d.floor_periods == 0.0);
}

TEST_CASE("queue drain: samples from before settle_seq are ignored")
{
	comp_queue_drain d;
	comp_queue_drain_reset(&d);
	uint64_t now = 10 * S;
	for (uint64_t i = 1; i <= 3; i++) {
		comp_queue_drain_on_residual(&d, 90 + i, periods(5.0), P60, now);
	}
	REQUIRE(d.pending);
	comp_queue_drain_reason r = COMP_QD_REASON_NONE;
	REQUIRE(comp_queue_drain_take(&d, 100, now, &r));
	CHECK(r == COMP_QD_REASON_RESIDUAL);
	CHECK(d.settle_seq == 100);

	// Older presents, still at the queued residual: no verdict, no floor.
	CHECK(comp_queue_drain_on_residual(&d, 97, periods(5.0), P60, now) == COMP_QD_VERDICT_NONE);
	CHECK(comp_queue_drain_on_residual(&d, 99, periods(5.0), P60, now) == COMP_QD_VERDICT_NONE);
	CHECK(d.state == COMP_QD_WAIT_RESULT);
	CHECK(d.floor_periods == 0.0);

	uint64_t seq = 100;
	CHECK(settle3(&d, seq, periods(4.0), periods(4.0), periods(4.0), now) == COMP_QD_VERDICT_DRAINED);
	CHECK(d.state == COMP_QD_IDLE);
}

TEST_CASE("queue drain: a high-floor device is probed once, then never again while steady")
{
	sim s;
	s.floor = 3.2;
	s.queued = 0;
	s.run_for(3 * S);
	REQUIRE(s.taken.size() == 1);
	CHECK(s.taken[0] == COMP_QD_REASON_RESIDUAL);
	REQUIRE(s.verdicts.size() == 1);
	CHECK(s.verdicts[0] == COMP_QD_VERDICT_NOT_QUEUE);
	CHECK(s.d.floor_periods == Catch::Approx(3.2).margin(0.01));
	CHECK(s.d.applied_backoff_ns == COMP_QD_BACKOFF_BASE_NS);

	// Well past the 5 s backoff expiry: still exactly one probe.
	s.run_for(120 * S);
	CHECK(s.taken.size() == 1);
	CHECK(s.d.state == COMP_QD_IDLE);
}

TEST_CASE("queue drain: a period change forgets the floor; a shorter one arms nothing")
{
	sim s;
	s.floor = 3.2;
	s.run_for(3 * S);
	REQUIRE(s.d.floor_periods > 0.0);
	REQUIRE(s.d.state == COMP_QD_BACKOFF);

	SECTION("shorter period: floor forgotten, backoff cleared, nothing armed")
	{
		comp_queue_drain_on_period_change(&s.d, P60, P90, s.now);
		CHECK(s.d.floor_periods == 0.0);
		CHECK(s.d.state == COMP_QD_IDLE);
		CHECK_FALSE(s.d.pending);
		CHECK_FALSE(comp_queue_drain_take(&s.d, s.seq, s.now, nullptr));
	}
	SECTION("longer period: floor forgotten and a rate-drop drain armed")
	{
		comp_queue_drain_on_period_change(&s.d, P90, P60, s.now);
		CHECK(s.d.floor_periods == 0.0);
		CHECK(s.d.pending);
		comp_queue_drain_reason r = COMP_QD_REASON_NONE;
		CHECK(comp_queue_drain_take(&s.d, s.seq, s.now, &r));
		CHECK(r == COMP_QD_REASON_RATE_DROP);
	}
	SECTION("a change under 20 % longer arms nothing")
	{
		comp_queue_drain_on_period_change(&s.d, P60, (uint64_t)(P60 * 1.1), s.now);
		CHECK_FALSE(s.d.pending);
	}
}

TEST_CASE("queue drain: acquire blocking arms a drain; with a floor known it needs the residual too")
{
	comp_queue_drain d;
	comp_queue_drain_reset(&d);
	uint64_t now = 10 * S;
	// A residual in hand, under the default threshold.
	comp_queue_drain_on_residual(&d, 1, periods(2.0), P60, now);

	SECTION("three blocking acquires in a row arm it")
	{
		comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		CHECK_FALSE(d.pending);
		comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		CHECK(d.pending);
		comp_queue_drain_reason r = COMP_QD_REASON_NONE;
		CHECK(comp_queue_drain_take(&d, 2, now, &r));
		CHECK(r == COMP_QD_REASON_ACQUIRE_BLOCK);
	}
	SECTION("a short acquire resets the run")
	{
		comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		comp_queue_drain_on_acquire_block(&d, periods(0.1), P60, now);
		comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		CHECK_FALSE(d.pending);
	}
	SECTION("ignored while waiting for a verdict")
	{
		for (int i = 0; i < 3; i++) {
			comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		}
		REQUIRE(comp_queue_drain_take(&d, 2, now, nullptr));
		for (int i = 0; i < 5; i++) {
			comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		}
		CHECK_FALSE(d.pending);
	}
	SECTION("floor known: a GPU-bound app at its floor is left alone")
	{
		for (int i = 0; i < 3; i++) {
			comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		}
		REQUIRE(comp_queue_drain_take(&d, 2, now, nullptr));
		// Residual did not move: floor 2.0 learned, backoff.
		uint64_t seq = 2;
		REQUIRE(settle3(&d, seq, periods(2.0), periods(2.0), periods(2.0), now) == COMP_QD_VERDICT_NOT_QUEUE);
		REQUIRE(d.floor_periods == Catch::Approx(2.0).margin(0.01));
		now += COMP_QD_BACKOFF_BASE_NS + 1;
		comp_queue_drain_on_residual(&d, seq++, periods(2.0), P60, now);
		for (int i = 0; i < 10; i++) {
			comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		}
		CHECK(d.state == COMP_QD_IDLE);
		CHECK_FALSE(d.pending);

		// Residual now above floor + 0.75: the block counts again.
		comp_queue_drain_on_residual(&d, seq++, periods(3.0), P60, now);
		for (int i = 0; i < 3; i++) {
			comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
		}
		CHECK(d.pending);
		CHECK(d.pending_reason == COMP_QD_REASON_ACQUIRE_BLOCK);
	}
}

TEST_CASE("queue drain: backoff doubles from 5 s to a 60 s cap; a success resets it")
{
	comp_queue_drain d;
	comp_queue_drain_reset(&d);
	uint64_t now = 10 * S;
	uint64_t seq = 1;
	double level = 3.0;
	const uint64_t expect_s[] = {5, 10, 20, 40, 60, 60};
	for (uint64_t want : expect_s) {
		// Each probe sits above the previously learned floor + margin.
		for (int i = 0; i < 3; i++) {
			comp_queue_drain_on_residual(&d, seq++, periods(level), P60, now);
		}
		REQUIRE(comp_queue_drain_take(&d, seq, now, nullptr));
		REQUIRE(settle3(&d, seq, periods(level), periods(level), periods(level), now) ==
		        COMP_QD_VERDICT_NOT_QUEUE);
		CHECK(d.applied_backoff_ns == want * S);
		// Not before expiry.
		CHECK_FALSE(comp_queue_drain_take(&d, seq, now + want * S - 1, nullptr));
		now += want * S + 1;
		level += 1.0;
	}

	// A drain that works resets the doubling.
	for (int i = 0; i < 3; i++) {
		comp_queue_drain_on_residual(&d, seq++, periods(level), P60, now);
	}
	REQUIRE(comp_queue_drain_take(&d, seq, now, nullptr));
	REQUIRE(settle3(&d, seq, periods(level - 1.0), periods(level - 1.0), periods(level - 1.0), now) ==
	        COMP_QD_VERDICT_DRAINED);
	CHECK(d.backoff_ns == COMP_QD_BACKOFF_BASE_NS);
}

TEST_CASE("queue drain: a drain with no verdict sample for 2 s falls back to idle")
{
	comp_queue_drain d;
	comp_queue_drain_reset(&d);
	uint64_t now = 10 * S;
	for (uint64_t i = 1; i <= 3; i++) {
		comp_queue_drain_on_residual(&d, i, periods(5.0), P60, now);
	}
	REQUIRE(comp_queue_drain_take(&d, 4, now, nullptr));
	CHECK(d.state == COMP_QD_WAIT_RESULT);

	// 1.9 s: still waiting.
	CHECK_FALSE(comp_queue_drain_take(&d, 5, now + 1900 * MS, nullptr));
	CHECK(d.state == COMP_QD_WAIT_RESULT);
	// Two of the three verdict samples, then silence: still no verdict.
	CHECK(comp_queue_drain_on_residual(&d, 4, periods(5.0), P60, now) == COMP_QD_VERDICT_NONE);
	CHECK(comp_queue_drain_on_residual(&d, 5, periods(5.0), P60, now) == COMP_QD_VERDICT_NONE);
	// 2.1 s after the take, any entry point: back to idle, no verdict, no floor.
	comp_queue_drain_on_acquire_block(&d, 0, P60, now + 2100 * MS);
	CHECK(d.state == COMP_QD_IDLE);
	CHECK(d.timeouts == 1);
	CHECK(d.floor_periods == 0.0);
	CHECK(d.backoff_ns == COMP_QD_BACKOFF_BASE_NS);
}

TEST_CASE("queue drain: at most 8 successful drains in a row without a sample at/below threshold")
{
	comp_queue_drain d;
	comp_queue_drain_reset(&d);
	uint64_t now = 10 * S;
	uint64_t seq = 1;
	// A mis-correlated feed: every drain "works", the residual never lands.
	int drained = 0;
	comp_queue_drain_verdict last = COMP_QD_VERDICT_NONE;
	for (int trial = 0; trial < 20 && d.state != COMP_QD_BACKOFF; trial++) {
		for (int i = 0; i < 3; i++) {
			comp_queue_drain_on_residual(&d, seq++, periods(5.0), P60, now);
		}
		REQUIRE(comp_queue_drain_take(&d, seq, now, nullptr));
		last = settle3(&d, seq, periods(4.0), periods(4.0), periods(4.0), now);
		if (last == COMP_QD_VERDICT_DRAINED) {
			drained++;
		}
		now += 100 * MS;
	}
	CHECK(drained == COMP_QD_MAX_SUCCESS_RUN - 1);
	CHECK(last == COMP_QD_VERDICT_CAPPED);
	CHECK(d.state == COMP_QD_BACKOFF);
	CHECK(d.drains_taken == COMP_QD_MAX_SUCCESS_RUN);

	SECTION("a sample at/below threshold between drains resets the run")
	{
		comp_queue_drain d2;
		comp_queue_drain_reset(&d2);
		seq = 1;
		for (int trial = 0; trial < 20; trial++) {
			comp_queue_drain_on_residual(&d2, seq++, periods(2.0), P60, now);
			for (int i = 0; i < 3; i++) {
				comp_queue_drain_on_residual(&d2, seq++, periods(5.0), P60, now);
			}
			REQUIRE(comp_queue_drain_take(&d2, seq, now, nullptr));
			CHECK(settle3(&d2, seq, periods(4.0), periods(4.0), periods(4.0), now) ==
			      COMP_QD_VERDICT_DRAINED);
		}
		CHECK(d2.state == COMP_QD_IDLE);
	}
}

TEST_CASE("queue drain: only a rate drop is honoured without a residual; reset clears all")
{
	comp_queue_drain d;
	comp_queue_drain_reset(&d);
	uint64_t now = 10 * S;
	for (int i = 0; i < 3; i++) {
		comp_queue_drain_on_acquire_block(&d, periods(0.5), P60, now);
	}
	REQUIRE(d.pending);
	CHECK_FALSE(comp_queue_drain_take(&d, 1, now, nullptr));
	CHECK_FALSE(d.pending);

	comp_queue_drain_on_period_change(&d, P90, P60, now);
	REQUIRE(comp_queue_drain_take(&d, 1, now, nullptr));
	uint64_t seq = 1;
	CHECK(settle3(&d, seq, periods(3.0), periods(3.0), periods(3.0), now) == COMP_QD_VERDICT_LENIENT);
	CHECK(d.floor_periods == 0.0);

	comp_queue_drain_reset(&d);
	CHECK(d.state == COMP_QD_IDLE);
	CHECK_FALSE(d.pending);
	CHECK(d.settle_seq == 0);
	CHECK(d.backoff_ns == COMP_QD_BACKOFF_BASE_NS);
}

// Measured: the residual jitters ~6-7 ms within a steady state, about the
// half-period success bar itself. Verdicts compare minima of 3 for that.
static constexpr uint64_t J = 3 * MS;

TEST_CASE("queue drain: a real one-slot drain under +/-3 ms jitter is judged DRAINED")
{
	comp_queue_drain d;
	comp_queue_drain_reset(&d);
	const uint64_t now = 10 * S;
	uint64_t seq = 1;
	comp_queue_drain_on_residual(&d, seq++, periods(4.0) + J, P60, now);
	comp_queue_drain_on_residual(&d, seq++, periods(4.0) - J, P60, now);
	comp_queue_drain_on_residual(&d, seq++, periods(4.0) + J, P60, now);
	REQUIRE(d.pending);
	REQUIRE(comp_queue_drain_take(&d, seq, now, nullptr));
	CHECK(d.pre_drain_residual_ns == periods(4.0) - J);
	CHECK(settle3(&d, seq, periods(3.0) - J, periods(3.0) + J, periods(3.0) - J, now) == COMP_QD_VERDICT_DRAINED);
	CHECK(d.floor_periods == 0.0);

	SECTION("the sim agrees, jitter on every sample")
	{
		sim s;
		s.jitter = J;
		s.floor = 2.0;
		s.queued = 2;
		s.run_for(5 * S);
		CHECK(s.queued == 0);
		for (auto v : s.verdicts) {
			CHECK(v == COMP_QD_VERDICT_DRAINED);
		}
		CHECK(s.d.floor_periods == 0.0);
	}
}

TEST_CASE("queue drain: a no-op drain under the same jitter is judged NOT_QUEUE")
{
	comp_queue_drain d;
	comp_queue_drain_reset(&d);
	const uint64_t now = 10 * S;
	uint64_t seq = 1;
	comp_queue_drain_on_residual(&d, seq++, periods(3.0) + J, P60, now);
	comp_queue_drain_on_residual(&d, seq++, periods(3.0) - J, P60, now);
	comp_queue_drain_on_residual(&d, seq++, periods(3.0) + J, P60, now);
	REQUIRE(comp_queue_drain_take(&d, seq, now, nullptr));
	CHECK(settle3(&d, seq, periods(3.0) + J, periods(3.0) - J, periods(3.0) + J, now) == COMP_QD_VERDICT_NOT_QUEUE);
	CHECK(d.floor_periods == Catch::Approx((double)(periods(3.0) - J) / P60).margin(0.001));
	CHECK(d.state == COMP_QD_BACKOFF);
}

TEST_CASE("queue drain: one slow first post-settle sample does not fail a real drain")
{
	comp_queue_drain d;
	comp_queue_drain_reset(&d);
	const uint64_t now = 10 * S;
	uint64_t seq = 1;
	for (int i = 0; i < 3; i++) {
		comp_queue_drain_on_residual(&d, seq++, periods(4.0), P60, now);
	}
	REQUIRE(comp_queue_drain_take(&d, seq, now, nullptr));
	// +12 ms on the first: alone it shows a 4.7 ms drop, under the 8.3 ms bar.
	CHECK(settle3(&d, seq, periods(3.0) + 12 * MS, periods(3.0), periods(3.0), now) == COMP_QD_VERDICT_DRAINED);
	CHECK(d.floor_periods == 0.0);
}

TEST_CASE("queue drain: after a drain the baseline holds post-settle samples only")
{
	comp_queue_drain d;
	comp_queue_drain_reset(&d);
	const uint64_t now = 10 * S;
	uint64_t seq = 1;
	for (int i = 0; i < 3; i++) {
		comp_queue_drain_on_residual(&d, seq++, periods(5.0), P60, now);
	}
	REQUIRE(comp_queue_drain_take(&d, seq, now, nullptr));
	CHECK(d.recent_n == 0);
	REQUIRE(settle3(&d, seq, periods(4.0), periods(4.0), periods(4.0), now) == COMP_QD_VERDICT_DRAINED);
	// Three more over threshold arm the next drain; its baseline is 4.0, not 5.0.
	for (int i = 0; i < 3; i++) {
		comp_queue_drain_on_residual(&d, seq++, periods(4.0), P60, now);
	}
	REQUIRE(comp_queue_drain_take(&d, seq, now, nullptr));
	CHECK(d.pre_drain_residual_ns == periods(4.0));
}
