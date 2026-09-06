// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for comp_rear_budget — the per-session rear-depth-budget
 *         runner shared by the native compositors (XR_DXR_depth_budget).
 *
 * `tests_aux_rear_budget` covers the policy and `tests_aux_bg_neutrality` the
 * measurement; what is left — and what this covers — is the *runner*: the
 * cadence rules that sit between them and that each backend used to own a copy
 * of. Three of them are load-bearing and none is visible from either pure
 * layer:
 *
 * - a DP that declines is a source that does not exist, not a neutral one,
 * - a preview whose generation stops advancing is NOT stale (capture sources
 *   deliver only on change, so a quiet desktop is the best case, and
 *   re-analysing it is pure waste),
 * - the policy is ticked EVERY frame while the DP is polled at most every
 *   66 ms — so the ramp must keep moving on frames that asked the DP nothing.
 *
 * A synthetic BGRA preview stands in for the display processor, so this runs
 * with no GPU, no window and no plug-in.
 */

#include "util/comp_rear_budget.h"

#include "catch_amalgamated.hpp"

#include <cstring>
#include <vector>

namespace {

constexpr uint64_t MS = 1000000ULL;

//! A DP-shaped background preview backed by a buffer the caller owns.
struct FakePreview
{
	std::vector<uint8_t> bytes;
	xrt_dp_background_preview pv{};

	FakePreview(uint32_t w, uint32_t h, uint32_t generation, bool busy)
	{
		bytes.resize((size_t)w * h * 4u);
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x++) {
				// Busy = 1-px vertical stripes: every horizontal
				// difference sample crosses the edge threshold, which is
				// exactly the cue the rear budget exists to avoid fighting.
				// Neutral = one flat colour, which carries none.
				const uint8_t v = busy ? ((x & 1u) ? 255 : 0) : 128;
				uint8_t *p = &bytes[((size_t)y * w + x) * 4u];
				p[0] = v; // B
				p[1] = v; // G
				p[2] = v; // R
				p[3] = 255;
			}
		}
		xrt_dp_background_preview_init(&pv);
		pv.generation = generation;
		pv.width = w;
		pv.height = h;
		pv.stride_bytes = w * 4u;
		pv.bgra = bytes.data();
	}
};

//! An armed, transparent, opted-in session.
struct Runner
{
	comp_rear_budget b{};

	Runner()
	{
		comp_rear_budget_init(&b, "test");
		comp_rear_budget_set_requested(&b, true);
		comp_rear_budget_arm(&b, /*transparent=*/true);
	}
	~Runner() { comp_rear_budget_fini(&b); }

	Runner(const Runner &) = delete;
	Runner &operator=(const Runner &) = delete;
};

/*!
 * One frame, exactly as a compositor drives it: ask whether this frame is due
 * to poll, call the (fake) DP only if it is, hand over what came back.
 *
 * @param pv NULL models a DP with no source — either no slot at all or a slot
 *           that declined this frame.
 * @return whether this frame actually polled.
 */
bool
step(Runner &r, const xrt_dp_background_preview *pv, uint64_t now_ns)
{
	const bool polled = comp_rear_budget_should_poll(&r.b, now_ns);
	comp_rear_budget_tick(&r.b, polled ? pv : nullptr, polled, /*transparent=*/true, now_ns);
	return polled;
}

u_rear_budget_out
read(Runner &r)
{
	u_rear_budget_out out{};
	REQUIRE(comp_rear_budget_get(&r.b, &out));
	return out;
}

//! Drive @p ms of frames at 10 ms each, ending on a tick at exactly `start+ms`.
uint64_t
run_for(Runner &r, const xrt_dp_background_preview *pv, uint64_t start_ns, uint64_t ms)
{
	for (uint64_t t = start_ns; t <= start_ns + ms * MS; t += 10 * MS) {
		step(r, pv, t);
	}
	return start_ns + ms * MS;
}

} // namespace


TEST_CASE("comp_rear_budget: a session that never opted in has no policy to read")
{
	comp_rear_budget b{};
	comp_rear_budget_init(&b, "test");
	comp_rear_budget_arm(&b, /*transparent=*/true);

	// False, NOT "unrestricted": the caller must apply the extension's own
	// zero-default rule, which differs by transparency. Answering here would
	// make this runner a second, quieter source of truth for that rule.
	u_rear_budget_out out{};
	REQUIRE_FALSE(comp_rear_budget_get(&b, &out));

	comp_rear_budget_fini(&b);
}

TEST_CASE("comp_rear_budget: an opaque session answers unrestricted without a tick")
{
	comp_rear_budget b{};
	comp_rear_budget_init(&b, "test");
	comp_rear_budget_set_requested(&b, true);
	comp_rear_budget_arm(&b, /*transparent=*/false);

	// The render thread never ticks for an opaque session, so a value that
	// waited for one would never arrive.
	u_rear_budget_out out{};
	REQUIRE(comp_rear_budget_get(&b, &out));
	CHECK(out.state == U_REAR_BUDGET_UNRESTRICTED_OPAQUE);
	CHECK(out.far_offset_vh > U_REAR_BUDGET_UNRESTRICTED_VH - 0.5f);

	comp_rear_budget_fini(&b);
}

TEST_CASE("comp_rear_budget: no preview clips, for seconds, without ever opening")
{
	Runner r;

	// Before the first tick the runner already answers — the conservative
	// answer, so the app's first frames are unchanged rather than briefly and
	// wrongly open.
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);

	run_for(r, nullptr, 0, 3000);

	const u_rear_budget_out out = read(r);
	CHECK(out.state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);
	CHECK(out.far_offset_vh < 0.001f);
	// "No answer" must never be reported as a measurement.
	CHECK(out.cue_energy < 0.001f);
}

TEST_CASE("comp_rear_budget: an invalid preview is no source at all")
{
	Runner r;

	// A 1-px-wide buffer cannot hold a single horizontal difference sample, so
	// there is nothing to measure. It must read as "no source", never as
	// "neutral" — the whole point of the dwell is that opening is earned.
	FakePreview too_narrow(1, 8, 1, /*busy=*/false);
	run_for(r, &too_narrow.pv, 0, 2000);
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);

	// Same for a source that positively flags its own preview stale.
	Runner r2;
	FakePreview flagged(64, 32, 1, /*busy=*/false);
	flagged.pv.flags |= XRT_DP_BG_PREVIEW_STALE;
	run_for(r2, &flagged.pv, 0, 2000);
	CHECK(read(r2).state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);
}

TEST_CASE("comp_rear_budget: a neutral background opens, and a frozen generation keeps it open")
{
	Runner r;
	FakePreview neutral(64, 32, /*generation=*/7, /*busy=*/false);

	// The dwell (400 ms) then the ramp (300 ms).
	uint64_t t = run_for(r, &neutral.pv, 0, 800);
	u_rear_budget_out out = read(r);
	CHECK(out.state == U_REAR_BUDGET_OPEN);
	CHECK(out.far_offset_vh > U_REAR_BUDGET_UNRESTRICTED_VH - 0.5f);

	// The generation never advances from here — a capture source delivers only
	// when the desktop CHANGES, so this is a quiet desktop, i.e. the best case.
	// It must not decay into staleness.
	run_for(r, &neutral.pv, t + 10 * MS, 5000);
	out = read(r);
	CHECK(out.state == U_REAR_BUDGET_OPEN);
	CHECK(out.far_offset_vh > U_REAR_BUDGET_UNRESTRICTED_VH - 0.5f);
}

TEST_CASE("comp_rear_budget: a busy background closes it, fast")
{
	Runner r;
	FakePreview neutral(64, 32, 1, /*busy=*/false);
	uint64_t t = run_for(r, &neutral.pv, 0, 800);
	REQUIRE(read(r).state == U_REAR_BUDGET_OPEN);

	// A new generation, because a capture that CHANGED is exactly what makes
	// the runner re-measure. Closing is deliberately much quicker than
	// opening: a visible occlusion conflict is worse than a missing rear.
	FakePreview busy(64, 32, 2, /*busy=*/true);
	t = run_for(r, &busy.pv, t + 10 * MS, 400);

	const u_rear_budget_out out = read(r);
	CHECK(out.state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
	CHECK(out.far_offset_vh < 0.001f);
	CHECK(out.cue_energy > 0.0f);
}

TEST_CASE("comp_rear_budget: a busy background never opens in the first place")
{
	Runner r;
	FakePreview busy(64, 32, 1, /*busy=*/true);
	run_for(r, &busy.pv, 0, 3000);
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
}

TEST_CASE("comp_rear_budget: the ramp advances on frames that polled nothing")
{
	Runner r;
	FakePreview neutral(64, 32, 1, /*busy=*/false);

	// Sit out the dwell so the next frames are mid-ramp.
	uint64_t t = run_for(r, &neutral.pv, 0, 410) + 10 * MS;

	// Land on a polling frame, so the following 50 ms provably contains none
	// (the DP throttle is 66 ms).
	while (!step(r, &neutral.pv, t)) {
		t += 10 * MS;
	}
	const float before = read(r).far_offset_vh;
	REQUIRE(before < U_REAR_BUDGET_UNRESTRICTED_VH); // still mid-ramp

	for (int i = 1; i <= 5; i++) {
		CHECK_FALSE(step(r, &neutral.pv, t + (uint64_t)i * 10 * MS));
	}
	const float after = read(r).far_offset_vh;

	// If the runner only advanced the policy on polling frames the clip plane
	// would step ~15 times a second instead of sliding, which is the artefact
	// the ramp exists to avoid.
	CHECK(after > before);
}

TEST_CASE("comp_rear_budget: an unarmed runner does no work")
{
	comp_rear_budget b{};
	comp_rear_budget_init(&b, "test");
	comp_rear_budget_set_requested(&b, false);
	comp_rear_budget_arm(&b, /*transparent=*/true);

	CHECK_FALSE(comp_rear_budget_is_running(&b));
	// An app that never asked must not pay for the poll — this is the gate the
	// extension's "an app that never asks costs nothing" promise rests on.
	CHECK_FALSE(comp_rear_budget_should_poll(&b, 0));
	CHECK_FALSE(comp_rear_budget_should_poll(&b, 10ULL * 1000 * MS));

	comp_rear_budget_fini(&b);
}
