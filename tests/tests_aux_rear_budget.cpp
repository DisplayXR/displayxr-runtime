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
