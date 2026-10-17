// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The trunk's eye-pair ring sampled at a target time (ADR-054 §4.1 /
 *         N6 Part 1, #2144).
 *
 * Pins the sampling rules every eye consumer now relies on when a display
 * processor cannot answer a target time itself: exact hits, per-eye linear
 * interpolation, bounded extrapolation on the last velocity, the stale answer
 * (last known, is_tracking=false, valid kept), a tracking-lost edge that is
 * never interpolated away, and a view-count change that takes the newer
 * sample. Plus the ring mechanics: order, replacement, wrap, clear.
 */

#include "catch_amalgamated.hpp"

#include "util/u_eye_history.h"

namespace {

constexpr int64_t kMs = 1000ll * 1000ll;

//! A stereo pair centred at x, both eyes at (x ± 0.032, y, z).
xrt_eye_positions
pair(float x, float y, float z, bool tracking = true, int64_t ts = 0)
{
	xrt_eye_positions e = {};
	e.count = 2;
	e.eyes[0] = {x - 0.032f, y, z};
	e.eyes[1] = {x + 0.032f, y, z};
	e.valid = true;
	e.is_tracking = tracking;
	e.timestamp_ns = ts;
	return e;
}

//! A 2x2 quad (the sim_display layout: the pair 32 mm below and above).
xrt_eye_positions
quad(float x, float y, float z)
{
	xrt_eye_positions e = pair(x, y, z);
	e.count = 4;
	e.eyes[0] = {x - 0.032f, y - 0.032f, z};
	e.eyes[1] = {x + 0.032f, y - 0.032f, z};
	e.eyes[2] = {x - 0.032f, y + 0.032f, z};
	e.eyes[3] = {x + 0.032f, y + 0.032f, z};
	return e;
}

struct Ring
{
	u_eye_history h;
	Ring()
	{
		u_eye_history_init(&h);
	}
	~Ring()
	{
		u_eye_history_destroy(&h);
	}
};

} // namespace

TEST_CASE("u_eye_history: empty ring reports EMPTY and leaves out untouched")
{
	Ring r;
	xrt_eye_positions out = pair(9.f, 9.f, 9.f);
	CHECK(u_eye_history_get(&r.h, 1000 * kMs, &out) == U_EYE_HISTORY_RESULT_EMPTY);
	CHECK(out.eyes[0].x == Catch::Approx(9.f - 0.032f));
	CHECK(u_eye_history_get_size(&r.h) == 0);
	CHECK_FALSE(u_eye_history_get_latest(&r.h, nullptr, nullptr));
}

TEST_CASE("u_eye_history: only valid, time-ordered samples are kept")
{
	Ring r;
	xrt_eye_positions invalid = pair(0.f, 0.1f, 0.6f);
	invalid.valid = false;
	CHECK_FALSE(u_eye_history_push(&r.h, &invalid, 100 * kMs));
	CHECK(u_eye_history_get_size(&r.h) == 0);

	xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
	CHECK(u_eye_history_push(&r.h, &a, 100 * kMs));
	// Older than the latest: rejected.
	CHECK_FALSE(u_eye_history_push(&r.h, &a, 90 * kMs));
	CHECK(u_eye_history_get_size(&r.h) == 1);
	// Same instant: replaces.
	xrt_eye_positions b = pair(0.05f, 0.1f, 0.6f);
	CHECK(u_eye_history_push(&r.h, &b, 100 * kMs));
	CHECK(u_eye_history_get_size(&r.h) == 1);
	int64_t ts = 0;
	xrt_eye_positions latest = {};
	REQUIRE(u_eye_history_get_latest(&r.h, &ts, &latest));
	CHECK(ts == 100 * kMs);
	CHECK(latest.eyes[0].x == Catch::Approx(0.05f - 0.032f));
}

TEST_CASE("u_eye_history: exact hit returns the sample as is")
{
	Ring r;
	xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
	xrt_eye_positions b = pair(0.1f, 0.1f, 0.6f);
	u_eye_history_push(&r.h, &a, 100 * kMs);
	u_eye_history_push(&r.h, &b, 116 * kMs);

	xrt_eye_positions out = {};
	CHECK(u_eye_history_get(&r.h, 100 * kMs, &out) == U_EYE_HISTORY_RESULT_EXACT);
	CHECK(out.eyes[1].x == Catch::Approx(0.032f));
	CHECK(out.timestamp_ns == 100 * kMs);
	CHECK(out.valid);
	CHECK(out.is_tracking);

	CHECK(u_eye_history_get(&r.h, 116 * kMs, &out) == U_EYE_HISTORY_RESULT_EXACT);
	CHECK(out.eyes[1].x == Catch::Approx(0.132f));
	CHECK(out.timestamp_ns == 116 * kMs);
}

TEST_CASE("u_eye_history: between two samples the positions lerp per eye")
{
	Ring r;
	xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
	xrt_eye_positions b = pair(0.1f, 0.2f, 0.5f);
	u_eye_history_push(&r.h, &a, 100 * kMs);
	u_eye_history_push(&r.h, &b, 120 * kMs);

	xrt_eye_positions out = {};
	// A quarter of the way.
	CHECK(u_eye_history_get(&r.h, 105 * kMs, &out) == U_EYE_HISTORY_RESULT_INTERPOLATED);
	CHECK(out.count == 2);
	CHECK(out.eyes[0].x == Catch::Approx(0.025f - 0.032f));
	CHECK(out.eyes[0].y == Catch::Approx(0.125f));
	CHECK(out.eyes[0].z == Catch::Approx(0.575f));
	CHECK(out.eyes[1].x == Catch::Approx(0.025f + 0.032f));
	CHECK(out.timestamp_ns == 105 * kMs);
	CHECK(out.valid);
	CHECK(out.is_tracking);
	// Halfway.
	CHECK(u_eye_history_get(&r.h, 110 * kMs, &out) == U_EYE_HISTORY_RESULT_INTERPOLATED);
	CHECK(out.eyes[1].x == Catch::Approx(0.05f + 0.032f));
	CHECK(out.eyes[1].y == Catch::Approx(0.15f));
}

TEST_CASE("u_eye_history: a tracking-lost edge is never interpolated away")
{
	Ring r;
	xrt_eye_positions a = pair(0.f, 0.1f, 0.6f, /*tracking=*/true);
	xrt_eye_positions b = pair(0.1f, 0.1f, 0.6f, /*tracking=*/false);
	u_eye_history_push(&r.h, &a, 100 * kMs);
	u_eye_history_push(&r.h, &b, 120 * kMs);

	xrt_eye_positions out = {};
	CHECK(u_eye_history_get(&r.h, 101 * kMs, &out) == U_EYE_HISTORY_RESULT_INTERPOLATED);
	CHECK_FALSE(out.is_tracking); // the untracked side wins, even 1 ms in
	CHECK(out.valid);             // positions still valid (last known rule)
	CHECK(out.eyes[0].x == Catch::Approx(0.005f - 0.032f));

	// The other way round too.
	Ring r2;
	u_eye_history_push(&r2.h, &b, 100 * kMs);
	u_eye_history_push(&r2.h, &a, 120 * kMs);
	CHECK(u_eye_history_get(&r2.h, 119 * kMs, &out) == U_EYE_HISTORY_RESULT_INTERPOLATED);
	CHECK_FALSE(out.is_tracking);
}

TEST_CASE("u_eye_history: a view-count change between neighbours takes the newer sample")
{
	Ring r;
	xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
	xrt_eye_positions b = quad(0.1f, 0.1f, 0.6f);
	u_eye_history_push(&r.h, &a, 100 * kMs);
	u_eye_history_push(&r.h, &b, 120 * kMs);

	xrt_eye_positions out = {};
	CHECK(u_eye_history_get(&r.h, 110 * kMs, &out) == U_EYE_HISTORY_RESULT_INTERPOLATED);
	CHECK(out.count == 4);
	CHECK(out.eyes[0].x == Catch::Approx(0.1f - 0.032f)); // the quad, unlerped
	CHECK(out.eyes[2].y == Catch::Approx(0.1f + 0.032f));
	CHECK(out.timestamp_ns == 110 * kMs);
	CHECK(out.is_tracking);
}

TEST_CASE("u_eye_history: past the latest the last velocity carries the pair forward")
{
	Ring r;
	// 0.1 m in 20 ms along x, -0.02 m along z.
	xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
	xrt_eye_positions b = pair(0.1f, 0.1f, 0.58f);
	u_eye_history_push(&r.h, &a, 100 * kMs);
	u_eye_history_push(&r.h, &b, 120 * kMs);

	xrt_eye_positions out = {};
	CHECK(u_eye_history_get(&r.h, 130 * kMs, &out) == U_EYE_HISTORY_RESULT_EXTRAPOLATED);
	CHECK(out.eyes[0].x == Catch::Approx(0.15f - 0.032f));
	CHECK(out.eyes[1].x == Catch::Approx(0.15f + 0.032f));
	CHECK(out.eyes[0].z == Catch::Approx(0.57f));
	CHECK(out.eyes[0].y == Catch::Approx(0.1f));
	CHECK(out.timestamp_ns == 130 * kMs);
	CHECK(out.valid);
	CHECK(out.is_tracking);
}

TEST_CASE("u_eye_history: extrapolation is capped at U_EYE_HISTORY_MAX_EXTRAPOLATE_NS and holds there")
{
	Ring r;
	xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
	xrt_eye_positions b = pair(0.1f, 0.1f, 0.6f); // 0.1 m / 20 ms = 5 m/s
	u_eye_history_push(&r.h, &a, 100 * kMs);
	u_eye_history_push(&r.h, &b, 120 * kMs);

	xrt_eye_positions out = {};
	// Exactly at the cap: +50 ms -> +0.25 m.
	CHECK(u_eye_history_get(&r.h, 170 * kMs, &out) == U_EYE_HISTORY_RESULT_EXTRAPOLATED);
	CHECK(out.eyes[0].x == Catch::Approx(0.35f - 0.032f));
	CHECK(out.timestamp_ns == 170 * kMs);
	// Way past it (but under the max age): the SAME capped prediction, and the
	// timestamp says how far the answer really reaches.
	CHECK(u_eye_history_get(&r.h, 400 * kMs, &out) == U_EYE_HISTORY_RESULT_EXTRAPOLATED);
	CHECK(out.eyes[0].x == Catch::Approx(0.35f - 0.032f));
	CHECK(out.timestamp_ns == 170 * kMs);
	CHECK(out.is_tracking);
}

TEST_CASE("u_eye_history: no usable velocity means the latest sample")
{
	xrt_eye_positions out = {};

	SECTION("single sample")
	{
		Ring r;
		xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
		u_eye_history_push(&r.h, &a, 100 * kMs);
		CHECK(u_eye_history_get(&r.h, 130 * kMs, &out) == U_EYE_HISTORY_RESULT_LATEST);
		CHECK(out.eyes[0].x == Catch::Approx(-0.032f));
		CHECK(out.timestamp_ns == 100 * kMs);
		CHECK(out.is_tracking);
	}
	SECTION("previous sample untracked (a frozen-to-live jump is not a velocity)")
	{
		Ring r;
		xrt_eye_positions a = pair(0.f, 0.1f, 0.6f, /*tracking=*/false);
		xrt_eye_positions b = pair(0.1f, 0.1f, 0.6f, /*tracking=*/true);
		u_eye_history_push(&r.h, &a, 100 * kMs);
		u_eye_history_push(&r.h, &b, 120 * kMs);
		CHECK(u_eye_history_get(&r.h, 130 * kMs, &out) == U_EYE_HISTORY_RESULT_LATEST);
		CHECK(out.eyes[0].x == Catch::Approx(0.1f - 0.032f));
		CHECK(out.is_tracking);
	}
	SECTION("count changed on the last two samples")
	{
		Ring r;
		xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
		xrt_eye_positions b = quad(0.1f, 0.1f, 0.6f);
		u_eye_history_push(&r.h, &a, 100 * kMs);
		u_eye_history_push(&r.h, &b, 120 * kMs);
		CHECK(u_eye_history_get(&r.h, 130 * kMs, &out) == U_EYE_HISTORY_RESULT_LATEST);
		CHECK(out.count == 4);
	}
}

TEST_CASE("u_eye_history: a stale ring answers last-known with is_tracking=false and valid kept")
{
	Ring r;
	xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
	xrt_eye_positions b = pair(0.1f, 0.1f, 0.6f);
	u_eye_history_push(&r.h, &a, 100 * kMs);
	u_eye_history_push(&r.h, &b, 120 * kMs);

	xrt_eye_positions out = {};
	// Just inside the max age: still an (capped) extrapolation.
	CHECK(u_eye_history_get(&r.h, 120 * kMs + U_EYE_HISTORY_MAX_AGE_NS, &out) == U_EYE_HISTORY_RESULT_EXTRAPOLATED);
	// One ns past it: stale.
	CHECK(u_eye_history_get(&r.h, 120 * kMs + U_EYE_HISTORY_MAX_AGE_NS + 1, &out) == U_EYE_HISTORY_RESULT_STALE);
	CHECK(out.valid);
	CHECK_FALSE(out.is_tracking);
	CHECK(out.count == 2);
	CHECK(out.eyes[0].x == Catch::Approx(0.1f - 0.032f)); // last known, not extrapolated
	CHECK(out.timestamp_ns == 120 * kMs);                 // and it says how old it is
}

TEST_CASE("u_eye_history: a target before the oldest sample returns the oldest")
{
	Ring r;
	xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
	xrt_eye_positions b = pair(0.1f, 0.1f, 0.6f);
	u_eye_history_push(&r.h, &a, 100 * kMs);
	u_eye_history_push(&r.h, &b, 120 * kMs);

	xrt_eye_positions out = {};
	CHECK(u_eye_history_get(&r.h, 50 * kMs, &out) == U_EYE_HISTORY_RESULT_BEFORE_OLDEST);
	CHECK(out.eyes[0].x == Catch::Approx(-0.032f));
	CHECK(out.timestamp_ns == 100 * kMs);
	CHECK(out.valid);
}

TEST_CASE("u_eye_history: the ring wraps, keeping the newest U_EYE_HISTORY_SIZE samples")
{
	Ring r;
	for (int i = 0; i < 3 * U_EYE_HISTORY_SIZE; i++) {
		xrt_eye_positions e = pair(0.001f * (float)i, 0.1f, 0.6f);
		CHECK(u_eye_history_push(&r.h, &e, (int64_t)i * kMs));
	}
	CHECK(u_eye_history_get_size(&r.h) == U_EYE_HISTORY_SIZE);
	const int last = 3 * U_EYE_HISTORY_SIZE - 1;
	const int oldest = last - (U_EYE_HISTORY_SIZE - 1);

	xrt_eye_positions out = {};
	CHECK(u_eye_history_get(&r.h, (int64_t)last * kMs, &out) == U_EYE_HISTORY_RESULT_EXACT);
	CHECK(out.eyes[0].x == Catch::Approx(0.001f * (float)last - 0.032f));
	CHECK(u_eye_history_get(&r.h, (int64_t)oldest * kMs, &out) == U_EYE_HISTORY_RESULT_EXACT);
	CHECK(out.eyes[0].x == Catch::Approx(0.001f * (float)oldest - 0.032f));
	// Dropped samples are gone: before the oldest.
	CHECK(u_eye_history_get(&r.h, (int64_t)(oldest - 1) * kMs, &out) == U_EYE_HISTORY_RESULT_BEFORE_OLDEST);
	// And interpolation still lands in the right pair after the wrap.
	CHECK(u_eye_history_get(&r.h, (int64_t)oldest * kMs + kMs / 2, &out) == U_EYE_HISTORY_RESULT_INTERPOLATED);
	CHECK(out.eyes[0].x == Catch::Approx(0.001f * ((float)oldest + 0.5f) - 0.032f));
}

TEST_CASE("u_eye_history: clear empties the ring")
{
	Ring r;
	xrt_eye_positions a = pair(0.f, 0.1f, 0.6f);
	u_eye_history_push(&r.h, &a, 100 * kMs);
	u_eye_history_clear(&r.h);
	CHECK(u_eye_history_get_size(&r.h) == 0);
	xrt_eye_positions out = {};
	CHECK(u_eye_history_get(&r.h, 100 * kMs, &out) == U_EYE_HISTORY_RESULT_EMPTY);
	// Older timestamps are accepted again after a clear.
	CHECK(u_eye_history_push(&r.h, &a, 10 * kMs));
}

TEST_CASE("u_eye_history: result names")
{
	CHECK(std::string(u_eye_history_result_str(U_EYE_HISTORY_RESULT_STALE)) == "stale");
	CHECK(std::string(u_eye_history_result_str(U_EYE_HISTORY_RESULT_EXTRAPOLATED)) == "extrapolated");
}

TEST_CASE("u_eye_history: the clamped read never extrapolates past the newest sample")
{
	// The compositors read the ring only when a DP's target-time slot fails,
	// and its samples are already vendor-predicted: predicting them again
	// would double the horizon. So past the newest sample the clamped read
	// holds that sample (LATEST), where u_eye_history_get extrapolates.
	Ring r;
	const xrt_eye_positions a = pair(0.00f, 0.1f, 0.6f);
	const xrt_eye_positions b = pair(0.01f, 0.1f, 0.6f);
	REQUIRE(u_eye_history_push(&r.h, &a, 100 * kMs));
	REQUIRE(u_eye_history_push(&r.h, &b, 110 * kMs));

	xrt_eye_positions out = {};
	CHECK(u_eye_history_get_clamped(&r.h, 130 * kMs, &out) == U_EYE_HISTORY_RESULT_LATEST);
	CHECK(out.eyes[0].x == Catch::Approx(0.01f - 0.032f));
	CHECK(out.timestamp_ns == 110 * kMs);
	CHECK(out.is_tracking);
	CHECK(out.valid);

	// The extrapolating read moves on with the velocity (0.01 m / 10 ms).
	xrt_eye_positions ext = {};
	CHECK(u_eye_history_get(&r.h, 130 * kMs, &ext) == U_EYE_HISTORY_RESULT_EXTRAPOLATED);
	CHECK(ext.eyes[0].x == Catch::Approx(0.03f - 0.032f).margin(1e-5f));

	// Between samples, interpolation is unchanged.
	CHECK(u_eye_history_get_clamped(&r.h, 105 * kMs, &out) == U_EYE_HISTORY_RESULT_INTERPOLATED);
	CHECK(out.eyes[0].x == Catch::Approx(0.005f - 0.032f).margin(1e-5f));

	// Stale is still stale: last known, not tracking.
	CHECK(u_eye_history_get_clamped(&r.h, 110 * kMs + U_EYE_HISTORY_MAX_AGE_NS + 1, &out) ==
	      U_EYE_HISTORY_RESULT_STALE);
	CHECK_FALSE(out.is_tracking);
	CHECK(out.valid);

	// Empty is empty.
	Ring e;
	CHECK(u_eye_history_get_clamped(&e.h, 1, &out) == U_EYE_HISTORY_RESULT_EMPTY);
}
