// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the windowed #206 residual statistics (#2068).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_residual_window.h"

#include <memory>

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t P60 = 16666666ULL;

TEST_CASE("residual window: empty window reports n = 0")
{
	auto w = std::make_unique<comp_residual_window>();
	*w = {};
	const comp_residual_stats st = comp_residual_window_take(w.get(), P60);
	CHECK(st.n == 0);
}

TEST_CASE("residual window: mean, p50, p95, max and the >1.5-period tail")
{
	auto w = std::make_unique<comp_residual_window>();
	*w = {};
	// 90 warm samples at 15 ms, 10 queued ones at 45 ms (2.7 periods).
	for (int i = 0; i < 90; i++) {
		comp_residual_window_add(w.get(), 15 * MS);
	}
	for (int i = 0; i < 10; i++) {
		comp_residual_window_add(w.get(), 45 * MS);
	}
	const comp_residual_stats st = comp_residual_window_take(w.get(), P60);
	CHECK(st.n == 100);
	CHECK(st.mean_ns == Catch::Approx(18.0 * MS));
	CHECK(st.p50_ns == 15 * MS);
	CHECK(st.p95_ns == 45 * MS);
	CHECK(st.max_ns == 45 * MS);
	CHECK(st.frac_over_1p5 == Catch::Approx(0.10));
	CHECK_FALSE(st.truncated);

	SECTION("take resets the window")
	{
		CHECK(comp_residual_window_take(w.get(), P60).n == 0);
	}
}

TEST_CASE("residual window: a single early transient no longer is the reading")
{
	// The panel trap: first sample 1.81 periods, then a warm 0.9.
	auto w = std::make_unique<comp_residual_window>();
	*w = {};
	comp_residual_window_add(w.get(), (uint64_t)(1.81 * P60));
	for (int i = 0; i < 299; i++) {
		comp_residual_window_add(w.get(), (uint64_t)(0.9 * P60));
	}
	const comp_residual_stats st = comp_residual_window_take(w.get(), P60);
	CHECK((double)st.p50_ns / P60 == Catch::Approx(0.9).margin(0.01));
	CHECK((double)st.max_ns / P60 == Catch::Approx(1.81).margin(0.01));
	CHECK(st.frac_over_1p5 == Catch::Approx(1.0 / 300.0));
}

TEST_CASE("residual window: past the cap, n/mean/max stay exact and truncation is flagged")
{
	auto w = std::make_unique<comp_residual_window>();
	*w = {};
	for (int i = 0; i < COMP_RESIDUAL_WINDOW_CAP + 10; i++) {
		comp_residual_window_add(w.get(), 10 * MS);
	}
	comp_residual_window_add(w.get(), 30 * MS);
	const comp_residual_stats st = comp_residual_window_take(w.get(), P60);
	CHECK(st.n == COMP_RESIDUAL_WINDOW_CAP + 11);
	CHECK(st.max_ns == 30 * MS);
	CHECK(st.truncated);
}
