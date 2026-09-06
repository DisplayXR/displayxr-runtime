// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for u_rear_budget — the rear depth budget policy
 *         (XR_DXR_depth_budget).
 *
 * Time is a parameter of the machine, never read inside it, so every dynamic
 * here (dwell, close grace, ramp shape) is exercised exactly rather
 * than approximately. The asymmetry is the point: opening is slow and must be
 * earned, closing is fast, and every "I don't know" answer clips.
 */

#include "util/u_rear_budget.h"

#include "catch_amalgamated.hpp"

#include <cstring>

namespace {

constexpr uint64_t MS = 1000000ULL;

u_rear_budget_tuning
tuning()
{
	u_rear_budget_tuning t{};
	u_rear_budget_tuning_defaults(&t);
	return t;
}

//! A transparent, standalone session whose background reads as @p neutral.
u_rear_budget_in
sample(bool neutral, uint32_t generation)
{
	u_rear_budget_in in{};
	in.transparent = true;
	in.under_workspace = false;
	in.source_available = true;
	in.have_result = true;
	in.generation = generation;
	in.result.neutral = neutral;
	in.result.cue_energy = neutral ? 0.1f : 1.0f;
	in.result.edge_fraction = neutral ? 0.0f : 0.05f;
	in.result.max_column_density = neutral ? 0.0f : 1.0f;
	return in;
}

/*!
 * The same session with an explicit cue energy. `u_bg_neutrality` clamps the
 * energy to [0,1] and reports `neutral` iff it is strictly below 1, so a cue
 * of exactly 1.0 IS the busy sample - the two cannot be set independently.
 */
u_rear_budget_in
sample_cue(float cue, uint32_t generation)
{
	u_rear_budget_in in = sample(cue < 1.0f, generation);
	in.result.cue_energy = cue;
	return in;
}

bool
is_clipped(u_rear_budget_state s)
{
	return s == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND || s == U_REAR_BUDGET_CLIPPED_NO_SOURCE;
}

} // namespace

TEST_CASE("rear_budget: an opaque session is unrestricted immediately")
{
	u_rear_budget b{};
	auto t = tuning();
	u_rear_budget_init(&b, &t, "test", 0);

	u_rear_budget_in in{};
	in.transparent = false;

	u_rear_budget_out out{};
	u_rear_budget_update(&b, &in, 0, &out);

	// No ramp: "this session does not composite over the desktop" is a fact,
	// not a perceptual judgement, so there is nothing to ease into.
	CHECK(out.state == U_REAR_BUDGET_UNRESTRICTED_OPAQUE);
	CHECK(out.far_offset_vh == U_REAR_BUDGET_UNRESTRICTED_VH);
	CHECK(out.cue_energy == 0.0f);
}

TEST_CASE("rear_budget: transparent under a workspace controller is unrestricted")
{
	u_rear_budget b{};
	auto t = tuning();
	u_rear_budget_init(&b, &t, "test", 0);

	u_rear_budget_in in{};
	in.transparent = true;
	in.under_workspace = true;

	u_rear_budget_out out{};
	u_rear_budget_update(&b, &in, 0, &out);

	CHECK(out.state == U_REAR_BUDGET_UNRESTRICTED_WORKSPACE);
	CHECK(out.far_offset_vh == U_REAR_BUDGET_UNRESTRICTED_VH);
}

TEST_CASE("rear_budget: transparent with no background source clips at the ZDP")
{
	u_rear_budget b{};
	auto t = tuning();
	u_rear_budget_init(&b, &t, "test", 0);

	u_rear_budget_in in{};
	in.transparent = true;
	in.source_available = false;

	u_rear_budget_out out{};
	u_rear_budget_update(&b, &in, 0, &out);

	CHECK(out.state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);
	CHECK(out.far_offset_vh == 0.0f);
	CHECK(out.cue_energy == 0.0f);

	// A DP that answers but hands over no analysis is the same situation.
	in.source_available = true;
	in.have_result = false;
	u_rear_budget_update(&b, &in, 10 * MS, &out);
	CHECK(out.state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);
	CHECK(out.far_offset_vh == 0.0f);
}

TEST_CASE("rear_budget: neutral held for less than the dwell stays clipped")
{
	u_rear_budget b{};
	auto t = tuning(); // open_dwell_ms = 400
	u_rear_budget_init(&b, &t, "test", 0);

	u_rear_budget_out out{};
	uint32_t gen = 1;
	for (uint64_t ms = 0; ms < t.open_dwell_ms; ms += 66) {
		auto in = sample(true, gen++);
		u_rear_budget_update(&b, &in, ms * MS, &out);
		INFO("ms = " << ms);
		CHECK(is_clipped(out.state));
		CHECK(out.far_offset_vh == 0.0f);
	}
}

TEST_CASE("rear_budget: neutral held past the dwell ramps open, monotonically")
{
	u_rear_budget b{};
	auto t = tuning(); // dwell 400 ms, ramp_open 300 ms
	u_rear_budget_init(&b, &t, "test", 0);

	u_rear_budget_out out{};
	uint32_t gen = 1;

	// Serve the dwell.
	for (uint64_t ms = 0; ms <= t.open_dwell_ms; ms += 50) {
		auto in = sample(true, gen++);
		u_rear_budget_update(&b, &in, ms * MS, &out);
	}
	REQUIRE(out.state == U_REAR_BUDGET_OPEN);

	// Mid-ramp the budget must be strictly between the two ends — a machine
	// that "ramps" by stepping straight to the target would pass every
	// endpoint assertion and still pop the app's clip plane.
	float prev = out.far_offset_vh;
	bool saw_intermediate = false;
	for (uint64_t ms = t.open_dwell_ms + 20; ms <= t.open_dwell_ms + t.ramp_open_ms; ms += 20) {
		auto in = sample(true, gen++);
		u_rear_budget_update(&b, &in, ms * MS, &out);
		INFO("ms = " << ms << " vh = " << out.far_offset_vh);
		CHECK(out.state == U_REAR_BUDGET_OPEN);
		CHECK(out.far_offset_vh >= prev); // monotonic non-decreasing
		if (out.far_offset_vh > 0.0f && out.far_offset_vh < U_REAR_BUDGET_UNRESTRICTED_VH) {
			saw_intermediate = true;
		}
		prev = out.far_offset_vh;
	}
	CHECK(saw_intermediate);

	// And it does arrive.
	auto in = sample(true, gen++);
	u_rear_budget_update(&b, &in, (t.open_dwell_ms + t.ramp_open_ms + 100) * MS, &out);
	CHECK(out.state == U_REAR_BUDGET_OPEN);
	CHECK(out.far_offset_vh == U_REAR_BUDGET_UNRESTRICTED_VH);
}

TEST_CASE("rear_budget: one busy sample closes after CLOSE_MS and ramps down")
{
	u_rear_budget b{};
	auto t = tuning(); // close 100 ms, ramp_close 150 ms
	u_rear_budget_init(&b, &t, "test", 0);

	u_rear_budget_out out{};
	uint32_t gen = 1;
	uint64_t ms = 0;

	// Get fully open first.
	for (; ms <= t.open_dwell_ms + t.ramp_open_ms; ms += 50) {
		auto in = sample(true, gen++);
		u_rear_budget_update(&b, &in, ms * MS, &out);
	}
	REQUIRE(out.state == U_REAR_BUDGET_OPEN);
	REQUIRE(out.far_offset_vh == U_REAR_BUDGET_UNRESTRICTED_VH);

	// The busy RUN starts here; the grace is measured from this sample.
	const uint64_t busy_at = ms + 10;

	// Inside the close grace the budget holds — a single stray sample must
	// not strobe the clip plane.
	{
		auto in = sample(false, gen++);
		u_rear_budget_update(&b, &in, busy_at * MS, &out);
		CHECK(out.state == U_REAR_BUDGET_OPEN);
		CHECK(out.far_offset_vh == U_REAR_BUDGET_UNRESTRICTED_VH);
	}
	{
		auto in = sample(false, gen++);
		u_rear_budget_update(&b, &in, (busy_at + t.close_ms - 20) * MS, &out);
		CHECK(out.state == U_REAR_BUDGET_OPEN);
	}

	// Past it, the state closes and the value SLIDES down rather than
	// snapping — closing fast is not the same as closing instantly.
	const uint64_t close_at = busy_at + t.close_ms;
	{
		auto in = sample(false, gen++);
		u_rear_budget_update(&b, &in, close_at * MS, &out);
		CHECK(out.state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
		CHECK(out.far_offset_vh > 0.0f);
		CHECK(out.far_offset_vh <= U_REAR_BUDGET_UNRESTRICTED_VH);
	}

	float prev = out.far_offset_vh;
	bool saw_intermediate = false;
	for (uint64_t d = 20; d <= t.ramp_close_ms; d += 20) {
		auto in = sample(false, gen++);
		u_rear_budget_update(&b, &in, (close_at + d) * MS, &out);
		INFO("d = " << d << " vh = " << out.far_offset_vh);
		CHECK(out.far_offset_vh <= prev); // monotonic non-increasing
		if (out.far_offset_vh > 0.0f && out.far_offset_vh < U_REAR_BUDGET_UNRESTRICTED_VH) {
			saw_intermediate = true;
		}
		prev = out.far_offset_vh;
	}
	CHECK(saw_intermediate);

	auto in = sample(false, gen++);
	u_rear_budget_update(&b, &in, (close_at + t.ramp_close_ms + 100) * MS, &out);
	CHECK(out.state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
	CHECK(out.far_offset_vh == 0.0f);
	CHECK(out.cue_energy > 0.0f);
}

TEST_CASE("rear_budget: closing is faster than opening")
{
	// The asymmetry is a design decision, not an accident of the numbers:
	// a visible depth conflict is worse than a missing rear.
	auto t = tuning();
	CHECK(t.close_ms < t.open_dwell_ms);
	CHECK(t.ramp_close_ms < t.ramp_open_ms);
}

TEST_CASE("rear_budget: a preview generation that stops advancing keeps the last verdict")
{
	u_rear_budget b{};
	auto t = tuning();
	u_rear_budget_init(&b, &t, "test", 0);

	u_rear_budget_out out{};
	uint32_t gen = 1;
	uint64_t ms = 0;

	for (; ms <= t.open_dwell_ms + t.ramp_open_ms; ms += 50) {
		auto in = sample(true, gen++);
		u_rear_budget_update(&b, &in, ms * MS, &out);
	}
	REQUIRE(out.state == U_REAR_BUDGET_OPEN);

	// The DP keeps claiming a source and keeps handing back the SAME
	// generation. That is what a QUIET desktop looks like: the capture only
	// delivers on change, so "unchanged" means the last analysis still
	// describes the screen. It must NOT read as a dead source and re-clip.
	const uint32_t frozen = gen;
	auto in = sample(true, frozen);
	for (uint64_t hold = 0; hold <= 30000; hold += 1000) {
		u_rear_budget_update(&b, &in, (ms + hold) * MS, &out);
		CHECK(out.state == U_REAR_BUDGET_OPEN);
		CHECK(out.far_offset_vh == U_REAR_BUDGET_UNRESTRICTED_VH);
	}

	// The source itself withdrawing is what closes it.
	auto gone = sample(true, frozen);
	gone.source_available = false;
	gone.have_result = false;
	u_rear_budget_update(&b, &gone, (ms + 31000) * MS, &out);
	CHECK(out.state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);
	CHECK(out.far_offset_vh == 0.0f);
}

TEST_CASE("rear_budget: the force override pins the budget")
{
	SECTION("clip")
	{
		u_rear_budget b{};
		auto t = tuning();
		t.force = U_REAR_BUDGET_FORCE_CLIP;
		u_rear_budget_init(&b, &t, "test", 0);

		u_rear_budget_out out{};
		uint32_t gen = 1;
		// Even a perfectly neutral background held far past the dwell.
		for (uint64_t ms = 0; ms <= 2000; ms += 50) {
			auto in = sample(true, gen++);
			u_rear_budget_update(&b, &in, ms * MS, &out);
			CHECK(out.state == U_REAR_BUDGET_FORCED);
			CHECK(out.far_offset_vh == 0.0f);
		}
	}

	SECTION("open")
	{
		u_rear_budget b{};
		auto t = tuning();
		t.force = U_REAR_BUDGET_FORCE_OPEN;
		u_rear_budget_init(&b, &t, "test", 0);

		u_rear_budget_out out{};
		uint32_t gen = 1;
		// Even a screen full of text, and even with no source at all.
		for (uint64_t ms = 0; ms <= 2000; ms += 50) {
			auto in = sample(false, gen++);
			in.source_available = (ms % 200) != 0;
			u_rear_budget_update(&b, &in, ms * MS, &out);
			CHECK(out.state == U_REAR_BUDGET_FORCED);
			CHECK(out.far_offset_vh == U_REAR_BUDGET_UNRESTRICTED_VH);
		}
	}
}

TEST_CASE("rear_budget: state names exist for every state")
{
	// The transition log is the only trace of this machine in a shipped
	// build; an unnamed state there is a state nobody can debug.
	const u_rear_budget_state all[] = {
	    U_REAR_BUDGET_UNRESTRICTED_OPAQUE,     U_REAR_BUDGET_UNRESTRICTED_WORKSPACE, U_REAR_BUDGET_OPEN,
	    U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND, U_REAR_BUDGET_CLIPPED_NO_SOURCE,      U_REAR_BUDGET_FORCED,
	};
	for (auto s : all) {
		INFO("state = " << (int)s);
		CHECK(std::strcmp(u_rear_budget_state_str(s), "?") != 0);
	}
}


/*
 * -----------------------------------------------------------------------------
 * The cue dead band (#1365)
 *
 * `neutral` is one threshold (cue < 1.0). A background parked just under it
 * satisfies the open dwell, opens, crosses the line on the next sample, closes
 * after the close grace, and repeats - the panel logged
 * `CLIPPED_NO_SOURCE -> OPEN cue=0.93`, then `CLIPPED_BUSY_BACKGROUND -> OPEN
 * cue=0.97`, then a 400-500 ms flap for seconds. One threshold cannot both
 * admit and reject; open_cue_max splits it in two and leaves a band between
 * them where the current verdict simply holds.
 * -----------------------------------------------------------------------------
 */

TEST_CASE("rear_budget: a cue inside the dead band never opens")
{
	u_rear_budget b{};
	auto t = tuning();
	u_rear_budget_init(&b, &t, "test", 0);

	// 0.95: neutral by the analysis, far above the 0.85 bar. Held for ten
	// times the dwell, it must still be shut - the dwell is reset by every
	// sample in the band, so it can never be served.
	u_rear_budget_out out{};
	for (uint64_t now = 0; now <= 4000 * MS; now += 10 * MS) {
		auto in = sample_cue(0.95f, 1);
		u_rear_budget_update(&b, &in, now, &out);
	}
	CHECK(is_clipped(out.state));
	CHECK(out.far_offset_vh == 0.0f);
	CHECK(out.cue_energy == Catch::Approx(0.95f));
}

TEST_CASE("rear_budget: an OPEN session holds through the dead band")
{
	u_rear_budget b{};
	auto t = tuning();
	u_rear_budget_init(&b, &t, "test", 0);

	// Earn OPEN on a genuinely quiet background, then let the cue drift up
	// into the band. This is the other half of the flap: the state must not
	// come back down for a sample that was never busy.
	u_rear_budget_out out{};
	for (uint64_t now = 0; now <= 800 * MS; now += 10 * MS) {
		auto in = sample_cue(0.5f, 1);
		u_rear_budget_update(&b, &in, now, &out);
	}
	REQUIRE(out.state == U_REAR_BUDGET_OPEN);
	REQUIRE(out.far_offset_vh == U_REAR_BUDGET_UNRESTRICTED_VH);

	for (uint64_t now = 810 * MS; now <= 3000 * MS; now += 10 * MS) {
		auto in = sample_cue(0.95f, 2);
		u_rear_budget_update(&b, &in, now, &out);
	}
	CHECK(out.state == U_REAR_BUDGET_OPEN);
	CHECK(out.far_offset_vh == U_REAR_BUDGET_UNRESTRICTED_VH);
}

TEST_CASE("rear_budget: the dead band does not block closing")
{
	u_rear_budget b{};
	auto t = tuning();
	u_rear_budget_init(&b, &t, "test", 0);

	u_rear_budget_out out{};
	for (uint64_t now = 0; now <= 800 * MS; now += 10 * MS) {
		auto in = sample_cue(0.5f, 1);
		u_rear_budget_update(&b, &in, now, &out);
	}
	REQUIRE(out.state == U_REAR_BUDGET_OPEN);

	// A genuinely busy sample (cue 1.0 == !neutral) still closes on the close
	// grace. The band buys stability, never a slower response to a real cue.
	for (uint64_t now = 810 * MS; now <= 1100 * MS; now += 10 * MS) {
		auto in = sample_cue(1.0f, 2);
		u_rear_budget_update(&b, &in, now, &out);
	}
	CHECK(out.state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
	CHECK(out.far_offset_vh == 0.0f);
}

TEST_CASE("rear_budget: after closing, a dead-band cue does not restart the dwell")
{
	u_rear_budget b{};
	auto t = tuning();
	u_rear_budget_init(&b, &t, "test", 0);

	u_rear_budget_out out{};
	for (uint64_t now = 0; now <= 300 * MS; now += 10 * MS) {
		auto in = sample_cue(1.0f, 1);
		u_rear_budget_update(&b, &in, now, &out);
	}
	REQUIRE(out.state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);

	// The cue relaxes to 0.95 - still in the band - and stays there. Without
	// the band this is the reopen half of the flap; with it the session stays
	// clipped no matter how long it holds.
	for (uint64_t now = 310 * MS; now <= 3000 * MS; now += 10 * MS) {
		auto in = sample_cue(0.95f, 2);
		u_rear_budget_update(&b, &in, now, &out);
	}
	CHECK(out.state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
	CHECK(out.far_offset_vh == 0.0f);

	// And it opens the moment the cue actually drops under the bar and the
	// dwell is served from scratch - the band delays nothing that qualifies.
	for (uint64_t now = 3010 * MS; now <= 3800 * MS; now += 10 * MS) {
		auto in = sample_cue(0.5f, 3);
		u_rear_budget_update(&b, &in, now, &out);
	}
	CHECK(out.state == U_REAR_BUDGET_OPEN);
}

TEST_CASE("rear_budget: open_cue_max is tunable and guarded")
{
	// Raise the bar past the band and the 0.95 background opens again - the
	// A/B has two visibly different arms rather than one arm and a no-op.
	u_rear_budget b{};
	auto t = tuning();
	t.open_cue_max = 0.99f;
	u_rear_budget_init(&b, &t, "test", 0);

	u_rear_budget_out out{};
	for (uint64_t now = 0; now <= 800 * MS; now += 10 * MS) {
		auto in = sample_cue(0.95f, 1);
		u_rear_budget_update(&b, &in, now, &out);
	}
	CHECK(out.state == U_REAR_BUDGET_OPEN);

	// A tuning struct that never set the field (or set it out of range) must
	// not silently mean "open on any neutral sample" - that is the flapping
	// behaviour, and a zero-initialised struct is the easiest way to get it.
	u_rear_budget c{};
	u_rear_budget_tuning zeroed{};
	zeroed.open_dwell_ms = 400;
	zeroed.close_ms = 100;
	zeroed.ramp_open_ms = 300;
	zeroed.ramp_close_ms = 150;
	zeroed.open_cue_max = 0.0f; // never set
	u_rear_budget_init(&c, &zeroed, "test", 0);

	for (uint64_t now = 0; now <= 2000 * MS; now += 10 * MS) {
		auto in = sample_cue(0.95f, 1);
		u_rear_budget_update(&c, &in, now, &out);
	}
	CHECK(is_clipped(out.state));
}
