// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the grid tier's one-frame-per-vblank aim (#2629).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_grid_rate_cap.h"

#include <vector>

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t P60 = 16666666ULL;
static constexpr uint64_t P90 = 11111111ULL;
static constexpr uint64_t ANCHOR = 1000 * MS;
//! Most cases below are mode-independent or THROUGHPUT's late rule; the AIM
//! late rule has its own cases.
static constexpr bool THR = true;
static constexpr bool AIM = false;

static comp_vblank_grid
grid(uint64_t period)
{
	comp_vblank_grid g = {};
	REQUIRE(comp_vblank_grid_set_period(&g, period));
	REQUIRE(comp_vblank_grid_observe(&g, ANCHOR));
	return g;
}

static uint64_t
vb(int n, uint64_t p = P60)
{
	return ANCHOR + (uint64_t)n * p;
}

/*!
 * A FIFO display over the vblanks the presents returned before: each vblank
 * shows at most one frame, oldest first. Per frame, how many whole periods
 * it waited in the queue past the vblank its present returned before.
 */
static std::vector<int>
fifo_waits(const std::vector<uint64_t> &made, uint64_t p)
{
	std::vector<int> waits;
	uint64_t shown = 0;
	for (uint64_t m : made) {
		const uint64_t d = (shown != 0 && shown + p > m) ? shown + p : m;
		waits.push_back((int)((d - m + p / 2) / p));
		shown = d;
	}
	return waits;
}

static int
total(const std::vector<int> &v, size_t from = 0)
{
	int t = 0;
	for (size_t i = from; i < v.size(); i++) {
		t += v[i];
	}
	return t;
}

struct run_result
{
	std::vector<uint64_t> made; //!< vblank each present returned before
	std::vector<comp_grid_rate_cap_kind> kinds;
	uint64_t elapsed_ns;
};

/*!
 * One producer through the cap: between weaves it spends frame_of(i) of its
 * own, the weave costs cost_of(i) on a budget @p budget_ns, and the weave's
 * sleep blocks it (the compositor lock).
 */
template <typename F, typename C>
static run_result
run_mode(bool thr, uint64_t period, uint64_t budget_ns, int frames, F frame_of, C cost_of)
{
	comp_vblank_grid g = grid(period);
	run_result r;
	uint64_t now = vb(1, period) + 1 * MS;
	const uint64_t t0 = now;
	uint64_t shown = 0;
	for (int i = 0; i < frames; i++) {
		uint64_t sleep_ns = 0;
		comp_grid_rate_cap_kind k = COMP_GRID_CAP_NONE;
		REQUIRE(comp_grid_rate_cap_target(&g, now, budget_ns, shown, thr, &sleep_ns, &k) != 0);
		r.kinds.push_back(k);
		now += sleep_ns + cost_of(i); // weave, then the present returns
		shown = comp_grid_rate_cap_on_present(&g, shown, now);
		r.made.push_back(comp_vblank_grid_next_vblank_after(&g, now));
		comp_vblank_grid_observe(&g, r.made.back()); // the display-timing feed
		now += frame_of(i);
	}
	r.elapsed_ns = now - t0;
	return r;
}

template <typename F, typename C>
static run_result
run(uint64_t period, uint64_t budget_ns, int frames, F frame_of, C cost_of)
{
	return run_mode(THR, period, budget_ns, frames, frame_of, cost_of);
}

//! The pre-cap grid tier on the same producer (v2.66.15): elapsed ns.
static uint64_t
pre_cap_elapsed(uint64_t period, uint64_t budget_ns, int frames, uint64_t frame_ns, uint64_t cost_ns)
{
	comp_vblank_grid g = grid(period);
	uint64_t now = vb(1, period) + 1 * MS;
	const uint64_t t0 = now;
	for (int i = 0; i < frames; i++) {
		const uint64_t next = comp_vblank_grid_next_vblank_after(&g, now);
		if (next > now + budget_ns) {
			uint64_t sl = next - budget_ns - now;
			now += sl > period ? period : sl;
		}
		now += cost_ns;
		comp_vblank_grid_observe(&g, comp_vblank_grid_next_vblank_after(&g, now));
		now += frame_ns;
	}
	return now - t0;
}

TEST_CASE("grid rate cap: the three aims")
{
	const comp_vblank_grid g = grid(P60);
	uint64_t sleep_ns = 0;
	comp_grid_rate_cap_kind k = COMP_GRID_CAP_NONE;
	const uint64_t now = vb(1) + 2 * MS;

	SECTION("reachable: the usual late-weave sleep")
	{
		CHECK(comp_grid_rate_cap_target(&g, now, 5 * MS, 0, THR, &sleep_ns, &k) == vb(2));
		CHECK(k == COMP_GRID_CAP_AIMED);
		CHECK(sleep_ns == vb(2) - 5 * MS - now);
	}
	SECTION("not reachable, not taken: weave NOW")
	{
		const uint64_t late = vb(2) - 3 * MS;
		CHECK(comp_grid_rate_cap_target(&g, late, 5 * MS, vb(1), THR, &sleep_ns, &k) == vb(3));
		CHECK(k == COMP_GRID_CAP_NOW);
		CHECK(sleep_ns == 0);
	}
	SECTION("not reachable, AIM mode: LATE, sleep toward the vblank after")
	{
		const uint64_t late = vb(2) - 3 * MS;
		CHECK(comp_grid_rate_cap_target(&g, late, 5 * MS, vb(1), AIM, &sleep_ns, &k) == vb(3));
		CHECK(k == COMP_GRID_CAP_LATE);
		CHECK(sleep_ns == vb(3) - 5 * MS - late);
	}
	SECTION("next vblank already shows the previous frame: pushed")
	{
		CHECK(comp_grid_rate_cap_target(&g, now, 5 * MS, vb(2), THR, &sleep_ns, &k) == vb(3));
		CHECK(k == COMP_GRID_CAP_PUSHED);
		CHECK(sleep_ns == vb(3) - 5 * MS - now); // 26 ms: a push may sleep up to two periods
	}
	SECTION("untrusted grid: no aim")
	{
		comp_vblank_grid u = {};
		CHECK(comp_grid_rate_cap_target(&u, now, 5 * MS, 0, THR, &sleep_ns, &k) == 0);
		CHECK(k == COMP_GRID_CAP_NONE);
		CHECK(sleep_ns == 0);
	}
}

TEST_CASE("grid rate cap: throughput — a late-arriving app is not halved")
{
	// Device round 2, 90 Hz model viewer: app frame 7 ms + weave 5 ms on a
	// 6 ms budget, so each frame arrives with the next vblank out of reach.
	// Round 1 slept toward the vblank after and the app's next frame missed
	// too: one weave per two vblanks.
	const run_result r = run(
	    P90, 6 * MS, 180, [](int) { return 7 * MS; }, [](int) { return 5 * MS; });
	const double per_s = 180.0 / ((double)r.elapsed_ns / 1e9);
	// The pre-cap rule (sleep only toward a reachable next vblank) on the
	// same producer: the cap must cost it nothing.
	const double pre_cap = 180.0 / ((double)pre_cap_elapsed(P90, 6 * MS, 180, 7 * MS, 5 * MS) / 1e9);
	CHECK(per_s >= pre_cap * 0.98);
	CHECK(per_s > 75.0);
	CHECK(total(fifo_waits(r.made, P90)) == 0);
	for (auto k : r.kinds) {
		CHECK(k != COMP_GRID_CAP_PUSHED);
	}
}

TEST_CASE("grid rate cap: an app that fits a period stays at the panel rate")
{
	const run_result r = run(
	    P90, 4 * MS, 180, [](int) { return 6 * MS; }, [](int) { return 3 * MS; });
	const double per_s = 180.0 / ((double)r.elapsed_ns / 1e9);
	CHECK(per_s == Catch::Approx(90.0).margin(1.0));
	CHECK(total(fifo_waits(r.made, P90)) == 0);
}

TEST_CASE("grid rate cap: a producer faster than the panel is paced, whatever the budget")
{
	SECTION("budget under a period")
	{
		const run_result r = run(
		    P60, 5 * MS, 120, [](int) { return 1 * MS; }, [](int) { return 4 * MS; });
		const double per_s = 120.0 / ((double)r.elapsed_ns / 1e9);
		CHECK(per_s == Catch::Approx(60.0).margin(1.0));
		CHECK(total(fifo_waits(r.made, P60), 10) == 0);
	}
	SECTION("budget above a period (the inflated-budget case from the device)")
	{
		const run_result r = run(
		    P60, 18 * MS, 120, [](int) { return 1 * MS; }, [](int) { return 15 * MS; });
		const double per_s = 120.0 / ((double)r.elapsed_ns / 1e9);
		CHECK(per_s == Catch::Approx(60.0).margin(1.0));
		// Never more than one frame waiting: no backlog builds.
		const std::vector<int> w = fifo_waits(r.made, P60);
		for (int x : w) {
			CHECK(x <= 1);
		}
	}
}

TEST_CASE("grid rate cap: an overrun does not leave a queued frame behind (two producers)")
{
	// The media player case: app frames and repaints fill every vblank (each
	// 4 ms of weave on a 5 ms budget, 1 ms apart); frame 40 runs 19 ms.
	const run_result r = run(
	    P60, 5 * MS, 120, [](int) { return 1 * MS; }, [](int i) { return i == 40 ? 19 * MS : 4 * MS; });
	const std::vector<int> w = fifo_waits(r.made, P60);
	CHECK(total(w, 0) <= 1);
	CHECK(total(w, 45) == 0);
}

TEST_CASE("grid rate cap: on_present follows the FIFO")
{
	const comp_vblank_grid g = grid(P60);
	// Made its vblank: shown there.
	CHECK(comp_grid_rate_cap_on_present(&g, vb(4), vb(5) - 2 * MS) == vb(5));
	// Returned after its target: shown at the vblank after the return.
	CHECK(comp_grid_rate_cap_on_present(&g, vb(4), vb(5) + 3 * MS) == vb(6));
	// Returned EARLY, into the previous frame's vblank: shown one after it.
	CHECK(comp_grid_rate_cap_on_present(&g, vb(5), vb(5) - 2 * MS) == vb(6));
	// No previous frame: the first vblank after the return.
	CHECK(comp_grid_rate_cap_on_present(&g, 0, vb(5) - 2 * MS) == vb(5));
}

TEST_CASE("grid rate cap: two weaves inside one period get consecutive vblanks")
{
	comp_vblank_grid g = grid(P90);
	uint64_t s2 = 0;
	comp_grid_rate_cap_kind k = COMP_GRID_CAP_NONE;
	// An app weave that started at once (NOW) returned its present early, at
	// vb(5) + 2 ms: shown at vb(6).
	const uint64_t shown = comp_grid_rate_cap_on_present(&g, 0, vb(5, P90) + 2 * MS);
	REQUIRE(shown == vb(6, P90));
	// A repaint 0.5 ms later would land on vb(6) too: pushed to vb(7).
	const uint64_t now = vb(5, P90) + 2 * MS + MS / 2;
	CHECK(comp_grid_rate_cap_target(&g, now, 3 * MS, shown, THR, &s2, &k) == vb(7, P90));
	CHECK(k == COMP_GRID_CAP_PUSHED);
	CHECK(s2 == vb(7, P90) - 3 * MS - now);

	SECTION("a producer already slower than the panel is never pushed")
	{
		// 60 Hz repaint cadence on a 90 Hz panel: the next weave comes after
		// the previous frame's vblank.
		const uint64_t later = vb(6, P90) + 3 * MS;
		uint64_t s3 = 0;
		comp_grid_rate_cap_target(&g, later, 3 * MS, shown, THR, &s3, &k);
		CHECK(k != COMP_GRID_CAP_PUSHED);
	}
}

TEST_CASE("grid rate cap: a stale shown vblank is ignored")
{
	const comp_vblank_grid g = grid(P60);
	uint64_t sleep_ns = 0;
	comp_grid_rate_cap_kind k = COMP_GRID_CAP_NONE;
	const uint64_t now = vb(10) + 1 * MS;
	const uint64_t natural = comp_grid_rate_cap_target(&g, now, 5 * MS, 0, THR, &sleep_ns, nullptr);
	CHECK(comp_grid_rate_cap_target(&g, now, 5 * MS, vb(5), THR, &sleep_ns, &k) == natural);
	CHECK(k == COMP_GRID_CAP_AIMED);
	// Shown at a vblank already past: no push either.
	CHECK(comp_grid_rate_cap_target(&g, now, 5 * MS, vb(10), THR, &sleep_ns, &k) == natural);
	CHECK(k == COMP_GRID_CAP_AIMED);
}

TEST_CASE("grid rate cap: after a period change the caller's reset gives the natural aim")
{
	comp_vblank_grid g = grid(P90);
	uint64_t sleep_ns = 0;
	const uint64_t now = vb(3, P90) + 1 * MS;
	REQUIRE(comp_grid_rate_cap_target(&g, now, 3 * MS, 0, THR, &sleep_ns, nullptr) == vb(4, P90));
	// The panel drops to 60 Hz; the target resets its shown vblank to 0.
	REQUIRE(comp_vblank_grid_set_period(&g, P60));
	comp_grid_rate_cap_kind k = COMP_GRID_CAP_NONE;
	CHECK(comp_grid_rate_cap_target(&g, now, 3 * MS, 0, THR, &sleep_ns, &k) == vb(3, P60));
	CHECK(k == COMP_GRID_CAP_AIMED);
	CHECK(sleep_ns == vb(3, P60) - 3 * MS - now);
}

TEST_CASE("grid rate cap: AIM halves an app that overruns the period; THROUGHPUT does not")
{
	// The device trade (model viewer at 90 Hz): 7 ms of app work + 5 ms of
	// weave on an 11.1 ms panel.
	auto fr = [](int) { return 7 * MS; };
	auto co = [](int) { return 5 * MS; };
	const run_result aim = run_mode(AIM, P90, 6 * MS, 180, fr, co);
	const run_result thr = run_mode(THR, P90, 6 * MS, 180, fr, co);
	const double aim_s = 180.0 / ((double)aim.elapsed_ns / 1e9);
	const double thr_s = 180.0 / ((double)thr.elapsed_ns / 1e9);
	CHECK(aim_s < 50.0);
	CHECK(thr_s > aim_s * 1.4);
	CHECK(total(fifo_waits(aim.made, P90)) == 0);
}

TEST_CASE("grid rate cap: AIM keeps an app that fits the period at the panel rate, aimed")
{
	const run_result r = run_mode(
	    AIM, P60, 4 * MS, 120, [](int) { return 6 * MS; }, [](int) { return 3 * MS; });
	const double per_s = 120.0 / ((double)r.elapsed_ns / 1e9);
	CHECK(per_s == Catch::Approx(60.0).margin(1.0));
	int now_ = 0;
	for (auto k : r.kinds) {
		now_ += k == COMP_GRID_CAP_NOW ? 1 : 0;
	}
	CHECK(now_ == 0);
	CHECK(total(fifo_waits(r.made, P60)) == 0);
}
