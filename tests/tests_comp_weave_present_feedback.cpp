// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the present-owner #206 residual from caller present
 *         feedback (XR_DXR_weave spec v21).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_weave_present_feedback.h"

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t P90 = 11111111ULL;
static constexpr uint64_t P60 = 16666666ULL;
static constexpr uint64_t T0 = 1000ULL * MS; // nothing here starts at time 0

namespace {

struct Sim
{
	comp_weave_pf pf = {};
	uint64_t fence = 0;
	uint64_t now = T0;

	//! One submit: weave starts now, returns the new fenceValue.
	uint64_t
	submit(uint64_t period)
	{
		now += period;
		fence++;
		comp_weave_pf_record(&pf, fence, now);
		return fence;
	}

	//! Report frame @p f (woven at @p start) as presented @p residual later.
	comp_weave_pf_verdict
	report(uint64_t f, uint64_t start, uint64_t residual)
	{
		return comp_weave_pf_report(&pf, f, (int64_t)(start + residual), now);
	}

	//! Submit + report the same frame at once (the residual is what matters).
	comp_weave_pf_verdict
	frame(uint64_t period, uint64_t residual)
	{
		const uint64_t f = submit(period);
		return report(f, now, residual);
	}
};

} // namespace

TEST_CASE("weave present feedback: unknown until a report is accepted")
{
	Sim s;
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 0);
	s.submit(P90);
	s.submit(P90);
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 0);
	CHECK(comp_weave_pf_predict(nullptr, s.now) == 0);
}

TEST_CASE("weave present feedback: steady state predicts the residual")
{
	Sim s;
	for (int i = 0; i < 200; i++) {
		// +-0.2 ms of jitter around 33 ms.
		const uint64_t r = 33 * MS + (uint64_t)((i * 7) % 5) * (MS / 10) - 2 * (MS / 10);
		REQUIRE(s.frame(P90, r) == COMP_WEAVE_PF_ACCEPTED);
	}
	const uint64_t p = comp_weave_pf_predict(&s.pf, s.now);
	CHECK(p >= 33 * MS - MS / 5);
	CHECK(p <= 33 * MS + MS / 5);
	CHECK(s.pf.accepted_total == 200);
	CHECK(s.pf.rejected_total == 0);
}

TEST_CASE("weave present feedback: the first accepted report is the prediction")
{
	Sim s;
	REQUIRE(s.frame(P90, 33 * MS) == COMP_WEAVE_PF_ACCEPTED);
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 33 * MS);
}

TEST_CASE("weave present feedback: a step change is followed within the window")
{
	Sim s;
	for (int i = 0; i < 50; i++) {
		s.frame(P90, 33 * MS);
	}
	REQUIRE(comp_weave_pf_predict(&s.pf, s.now) == 33 * MS);

	// 90 Hz -> 60 Hz: the residual steps to 61 ms.
	int frames_to_follow = 0;
	for (int i = 1; i <= COMP_WEAVE_PF_MEDIAN; i++) {
		s.frame(P60, 61 * MS);
		if (frames_to_follow == 0 && comp_weave_pf_predict(&s.pf, s.now) == 61 * MS) {
			frames_to_follow = i;
		}
	}
	CHECK(frames_to_follow > 1); // one sample alone must not move it
	CHECK(frames_to_follow <= COMP_WEAVE_PF_MEDIAN);
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 61 * MS);

	// ... and back.
	for (int i = 0; i < COMP_WEAVE_PF_MEDIAN; i++) {
		s.frame(P90, 33 * MS);
	}
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 33 * MS);
}

TEST_CASE("weave present feedback: an isolated late frame does not move the median")
{
	Sim s;
	for (int i = 0; i < 20; i++) {
		s.frame(P90, 33 * MS);
	}
	REQUIRE(s.frame(P90, 77 * MS) == COMP_WEAVE_PF_ACCEPTED); // plausible, just late
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 33 * MS);
}

TEST_CASE("weave present feedback: implausible residuals are rejected")
{
	Sim s;
	for (int i = 0; i < 20; i++) {
		s.frame(P90, 33 * MS);
	}
	const uint64_t accepted = s.pf.accepted_total;

	// Presented before / exactly when the weave started.
	uint64_t f = s.submit(P90);
	CHECK(comp_weave_pf_report(&s.pf, f, (int64_t)(s.now - MS), s.now) == COMP_WEAVE_PF_REJECTED_RANGE);
	f = s.submit(P90);
	CHECK(comp_weave_pf_report(&s.pf, f, (int64_t)s.now, s.now) == COMP_WEAVE_PF_REJECTED_RANGE);
	// Non-positive timestamps.
	f = s.submit(P90);
	CHECK(comp_weave_pf_report(&s.pf, f, 0, s.now) == COMP_WEAVE_PF_REJECTED_RANGE);
	f = s.submit(P90);
	CHECK(comp_weave_pf_report(&s.pf, f, -5, s.now) == COMP_WEAVE_PF_REJECTED_RANGE);
	// Above the ceiling; exactly on it is still a measurement.
	CHECK(s.frame(P90, COMP_WEAVE_PF_MAX_RESIDUAL_NS + 1) == COMP_WEAVE_PF_REJECTED_RANGE);
	CHECK(s.frame(P90, COMP_WEAVE_PF_MAX_RESIDUAL_NS) == COMP_WEAVE_PF_ACCEPTED);

	CHECK(s.pf.accepted_total == accepted + 1);
	CHECK(s.pf.rejected_total == 5);
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 33 * MS);
}

TEST_CASE("weave present feedback: unknown fenceValues are rejected")
{
	Sim s;
	// Nothing recorded at all.
	CHECK(comp_weave_pf_report(&s.pf, 7, (int64_t)(s.now + 33 * MS), s.now) == COMP_WEAVE_PF_REJECTED_UNKNOWN);
	// 0 is never a returned fenceValue.
	CHECK(comp_weave_pf_report(&s.pf, 0, (int64_t)(s.now + 33 * MS), s.now) == COMP_WEAVE_PF_REJECTED_UNKNOWN);

	const uint64_t f = s.submit(P90);
	const uint64_t start = s.now;
	// A value from the future, and one that shares f's slot but is not f.
	CHECK(s.report(f + 1, start, 33 * MS) == COMP_WEAVE_PF_REJECTED_UNKNOWN);
	CHECK(s.report(f + COMP_WEAVE_PF_RING, start, 33 * MS) == COMP_WEAVE_PF_REJECTED_UNKNOWN);
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 0);

	// The real one is still there; a repeat of it is not.
	CHECK(s.report(f, start, 33 * MS) == COMP_WEAVE_PF_ACCEPTED);
	CHECK(s.report(f, start, 33 * MS) == COMP_WEAVE_PF_REJECTED_UNKNOWN);
	CHECK(s.pf.accepted_total == 1);
	CHECK(s.pf.rejected_total == 5);

	// A NULL filter and unrecordable inputs are harmless.
	CHECK(comp_weave_pf_report(nullptr, f, 1, s.now) == COMP_WEAVE_PF_REJECTED_UNKNOWN);
	comp_weave_pf_record(nullptr, 1, 1);
	comp_weave_pf_record(&s.pf, 0, s.now);
	comp_weave_pf_record(&s.pf, 99, 0);
	CHECK(s.report(99, s.now, 33 * MS) == COMP_WEAVE_PF_REJECTED_UNKNOWN);
}

TEST_CASE("weave present feedback: the ring wraps")
{
	Sim s;
	// A frame whose report never came, then enough submits to overwrite it.
	const uint64_t lost = s.submit(P90);
	const uint64_t lost_start = s.now;
	for (int i = 0; i < COMP_WEAVE_PF_RING - 1; i++) {
		s.submit(P90);
	}
	// Still in the ring: exactly RING submits are remembered.
	const uint64_t oldest_kept = lost;
	CHECK(s.report(oldest_kept, lost_start, 33 * MS) == COMP_WEAVE_PF_ACCEPTED);

	const uint64_t lost2 = s.submit(P90);
	const uint64_t lost2_start = s.now;
	for (int i = 0; i < COMP_WEAVE_PF_RING; i++) {
		s.submit(P90);
	}
	CHECK(s.report(lost2, lost2_start, 33 * MS) == COMP_WEAVE_PF_REJECTED_UNKNOWN);

	// Many wraps, each frame reported 3 submits late: all resolve.
	uint64_t starts[4] = {0, 0, 0, 0};
	uint64_t fences[4] = {0, 0, 0, 0};
	uint64_t ok = 0;
	for (int i = 0; i < 1000; i++) {
		const uint64_t f = s.submit(P90);
		if (i >= 3) {
			const int k = (i - 3) % 4;
			ok += s.report(fences[k], starts[k], 33 * MS) == COMP_WEAVE_PF_ACCEPTED ? 1 : 0;
		}
		fences[i % 4] = f;
		starts[i % 4] = s.now;
	}
	CHECK(ok == 997);
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 33 * MS);
}

TEST_CASE("weave present feedback: stale feedback reads unknown, then restarts")
{
	Sim s;
	for (int i = 0; i < 20; i++) {
		s.frame(P90, 33 * MS);
	}
	const uint64_t last = s.now;
	CHECK(comp_weave_pf_predict(&s.pf, last + COMP_WEAVE_PF_STALE_NS) == 33 * MS);
	CHECK(comp_weave_pf_predict(&s.pf, last + COMP_WEAVE_PF_STALE_NS + 1) == 0);

	// Submits continue without reports: still unknown.
	for (int i = 0; i < 120; i++) {
		s.submit(P60);
	}
	REQUIRE(s.now - last > COMP_WEAVE_PF_STALE_NS);
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 0);

	// Reports resume at another rate: the old window does not outvote them.
	REQUIRE(s.frame(P60, 61 * MS) == COMP_WEAVE_PF_ACCEPTED);
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 61 * MS);
}

TEST_CASE("weave present feedback: reports arriving out of order")
{
	Sim s;
	uint64_t fences[8];
	uint64_t starts[8];
	for (int i = 0; i < 8; i++) {
		fences[i] = s.submit(P90);
		starts[i] = s.now;
	}
	// Each frame has its own residual; delivered newest-first, in two batches.
	static const int order[8] = {3, 1, 2, 0, 7, 5, 6, 4};
	for (int i = 0; i < 8; i++) {
		const int k = order[i];
		const uint64_t r = (30 + (uint64_t)k) * MS; // 30..37 ms
		REQUIRE(s.report(fences[k], starts[k], r) == COMP_WEAVE_PF_ACCEPTED);
	}
	// Each resolved against ITS OWN weave start: median of 30..37 (upper middle).
	CHECK(comp_weave_pf_predict(&s.pf, s.now) == 34 * MS);
	CHECK(s.pf.accepted_total == 8);
	CHECK(s.pf.rejected_total == 0);
}

TEST_CASE("weave present feedback: the summary window")
{
	Sim s;
	comp_weave_pf_stats st = comp_weave_pf_take_stats(&s.pf, s.now);
	CHECK(st.accepted == 0);
	CHECK(st.min_ns == 0);
	CHECK(st.median_ns == 0);

	s.frame(P90, 32 * MS);
	s.frame(P90, 33 * MS);
	s.frame(P90, 35 * MS);
	s.frame(P90, 600 * MS); // rejected
	st = comp_weave_pf_take_stats(&s.pf, s.now);
	CHECK(st.accepted == 3);
	CHECK(st.rejected == 1);
	CHECK(st.min_ns == 32 * MS);
	CHECK(st.max_ns == 35 * MS);
	CHECK(st.median_ns == 33 * MS);

	// Taking clears the window, not the filter.
	st = comp_weave_pf_take_stats(&s.pf, s.now);
	CHECK(st.accepted == 0);
	CHECK(st.rejected == 0);
	CHECK(st.median_ns == 33 * MS);
	CHECK(comp_weave_pf_take_stats(nullptr, s.now).accepted == 0);
}
