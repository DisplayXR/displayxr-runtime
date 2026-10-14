// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the self-correcting grid late-weave deadline (#2068).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_late_weave_adapt.h"

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t P60 = 16666666ULL;
static constexpr uint64_t MARGIN = 1500ULL * 1000ULL;
static constexpr uint64_t WEAVE = 3 * MS;
static constexpr uint64_t T0 = 1000ULL * 1000ULL * MS;

/*!
 * A simulated composited desktop: a commit made at least @ref latch_ns before
 * a vblank is composed for it and scanned out @ref pipeline vblanks later;
 * anything later waits one more. Measured GNOME shape: pipeline 2.
 */
struct desktop
{
	uint64_t latch_ns;
	uint32_t pipeline;

	//! Glass time of a commit at @p commit_ns.
	uint64_t
	glass(uint64_t commit_ns) const
	{
		const uint64_t k = (commit_ns - T0) / P60 + 1; // first vblank after the commit
		uint64_t v = T0 + k * P60;
		if (v - commit_ns < latch_ns) {
			v += P60; // missed this one's latch
		}
		return v + (uint64_t)(pipeline - 1) * P60;
	}
};

//! Commit @p gap before vblank number @p frame.
static uint64_t
commit_at(uint64_t frame, uint64_t gap)
{
	return T0 + (frame + 1) * P60 - gap;
}

TEST_CASE("lw lead: vblanks and gap follow from commit and glass alone")
{
	uint64_t gap = 0;
	CHECK(comp_lw_lead_vblanks(T0 + P60 - 3 * MS, T0 + P60, P60, &gap) == 1);
	CHECK(gap == 3 * MS);
	CHECK(comp_lw_lead_vblanks(T0 + P60 - 3 * MS, T0 + 3 * P60, P60, &gap) == 3);
	CHECK(gap == 3 * MS);
	CHECK(comp_lw_lead_vblanks(T0, T0, P60, &gap) == 0); // cannot place
	CHECK(comp_lw_lead_vblanks(T0, T0 + P60, 0, &gap) == 0);
}

TEST_CASE("lw lead: does not aim until it has watched the desktop")
{
	comp_lw_lead l = {};
	desktop d = {5 * MS, 2};
	for (uint64_t f = 0; f < COMP_LW_LEAD_PRIME_SAMPLES - 1; f++) {
		const uint64_t c = commit_at(f, 10 * MS);
		comp_lw_lead_observe(&l, c, d.glass(c), false, P60, WEAVE, MARGIN);
	}
	CHECK_FALSE(comp_lw_lead_primed(&l));
	const uint64_t c = commit_at(100, 10 * MS);
	comp_lw_lead_observe(&l, c, d.glass(c), false, P60, WEAVE, MARGIN);
	CHECK(comp_lw_lead_primed(&l));
	CHECK(l.base_vblanks == 2);
}

TEST_CASE("lw lead: starts from the smallest gap known to hit, not from a guess")
{
	comp_lw_lead l = {};
	desktop d = {5 * MS, 2};
	// Un-aimed commits spread across the period: some before the latch, some after.
	for (uint64_t f = 0; f < 60; f++) {
		const uint64_t gap = 1 * MS + (f % 15) * MS; // 1..15 ms before the vblank
		const uint64_t c = commit_at(f, gap);
		comp_lw_lead_observe(&l, c, d.glass(c), false, P60, WEAVE, MARGIN);
	}
	REQUIRE(comp_lw_lead_primed(&l));
	CHECK(l.base_vblanks == 2);
	CHECK(l.min_hit_gap_ns == 5 * MS); // the latch
	comp_lw_lead_init(&l, MARGIN);
	CHECK(l.lead_ns == 5 * MS - MARGIN);
}

TEST_CASE("lw lead: the compositor's pipeline delay is not a miss (panel regression)")
{
	// Panel log: "lead 12.17 ms, aimed hits 0, misses 4 [OFF]" — every frame
	// behind a two-vblank compositor counted as late. With the gate committing
	// margin + lead before the aimed vblank, a desktop whose latch is inside
	// that window must give hits, every frame.
	comp_lw_lead l = {};
	desktop d = {3 * MS, 2};
	for (uint64_t f = 0; f < 40; f++) {
		const uint64_t c = commit_at(f, 8 * MS);
		comp_lw_lead_observe(&l, c, d.glass(c), false, P60, WEAVE, MARGIN);
	}
	comp_lw_lead_init(&l, MARGIN);
	REQUIRE(l.initialised);
	for (uint64_t f = 40; f < 400; f++) {
		const uint64_t c = commit_at(f, MARGIN + l.lead_ns);
		comp_lw_lead_observe(&l, c, d.glass(c), true, P60, WEAVE, MARGIN);
	}
	CHECK_FALSE(l.off);
	CHECK(l.misses == 0);
	CHECK(l.hits == 360);
}

TEST_CASE("lw lead: probes down to the latch and holds there")
{
	comp_lw_lead l = {};
	desktop d = {4 * MS, 2};
	// The app commits early in every un-aimed frame: the known-good gap is big.
	for (uint64_t f = 0; f < 40; f++) {
		const uint64_t c = commit_at(f, 12 * MS);
		comp_lw_lead_observe(&l, c, d.glass(c), false, P60, WEAVE, MARGIN);
	}
	comp_lw_lead_init(&l, MARGIN);
	CHECK(l.lead_ns == 12 * MS - MARGIN);
	for (uint64_t f = 40; f < 40 + 60 * 120; f++) { // two minutes at 60 Hz
		const uint64_t c = commit_at(f, MARGIN + l.lead_ns);
		comp_lw_lead_observe(&l, c, d.glass(c), true, P60, WEAVE, MARGIN);
	}
	CHECK_FALSE(l.off);
	// Converged near the latch (4 ms gap = 2.5 ms lead), within a step.
	CHECK(l.lead_ns + MARGIN + COMP_LW_LEAD_STEP_DOWN_NS >= 4 * MS);
	CHECK(l.lead_ns + MARGIN < 4 * MS + COMP_LW_LEAD_STEP_UP_NS + COMP_LW_LEAD_STEP_DOWN_NS);
	// Each probe below the latch costs one frame: well under one a second.
	CHECK(l.misses < 60);
}

TEST_CASE("lw lead: a latch that leaves no slack turns the tier off, once")
{
	comp_lw_lead l = {};
	l.base_vblanks = 2;
	l.have_hit_gap = true;
	l.min_hit_gap_ns = 3 * MS;
	l.observed = COMP_LW_LEAD_PRIME_SAMPLES;
	comp_lw_lead_init(&l, MARGIN);
	const uint64_t weave = 9 * MS; // slack: 16.67 - 9 - 1.5 = 6.2 ms
	const uint64_t c = commit_at(0, 2 * MS);
	enum comp_lw_adapt_event e = COMP_LW_ADAPT_NONE;
	int raises = 0;
	while ((e = comp_lw_lead_observe(&l, c, c + 3 * P60 - 2 * MS + P60, true, P60, weave, MARGIN)) ==
	       COMP_LW_ADAPT_RAISED) {
		raises++;
	}
	CHECK(e == COMP_LW_ADAPT_DISABLED);
	CHECK(raises == 2);
	CHECK(l.off);
	CHECK(comp_lw_lead_observe(&l, c, c + 4 * P60, true, P60, weave, MARGIN) == COMP_LW_ADAPT_NONE);
}

TEST_CASE("lw lead: a shorter pipeline than any seen resets the floor and its hit gaps")
{
	comp_lw_lead l = {};
	desktop slow = {3 * MS, 3};
	for (uint64_t f = 0; f < 10; f++) {
		const uint64_t c = commit_at(f, 6 * MS);
		comp_lw_lead_observe(&l, c, slow.glass(c), false, P60, WEAVE, MARGIN);
	}
	CHECK(l.base_vblanks == 3);
	desktop fast = {3 * MS, 2}; // e.g. the window went direct-scanout
	const uint64_t c = commit_at(20, 9 * MS);
	comp_lw_lead_observe(&l, c, fast.glass(c), false, P60, WEAVE, MARGIN);
	CHECK(l.base_vblanks == 2);
	CHECK(l.min_hit_gap_ns == 9 * MS);
}
