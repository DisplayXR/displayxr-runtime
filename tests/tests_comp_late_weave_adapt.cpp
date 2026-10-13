// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the self-correcting late-weave deadlines (#2068).
 */

#include "catch_amalgamated.hpp"

#include "util/comp_late_weave_adapt.h"

static constexpr uint64_t MS = 1000ULL * 1000ULL;
static constexpr uint64_t P60 = 16666666ULL;
static constexpr uint64_t MARGIN = 1500ULL * 1000ULL;

TEST_CASE("lw lead: starts at a quarter period and rises on a miss")
{
	comp_lw_lead l = {};
	comp_lw_lead_init(&l, P60);
	CHECK(l.lead_ns == P60 / 4);
	CHECK(comp_lw_lead_on_result(&l, true, P60, 3 * MS, MARGIN) == COMP_LW_ADAPT_RAISED);
	CHECK(l.lead_ns == P60 / 4 + COMP_LW_LEAD_STEP_UP_NS);
	CHECK(l.misses == 1);
}

TEST_CASE("lw lead: a miss is landing past half a period after the aimed vblank")
{
	const uint64_t v = 1000 * MS;
	CHECK_FALSE(comp_lw_lead_is_miss(v, v, P60));
	CHECK_FALSE(comp_lw_lead_is_miss(v, v + 2 * MS, P60)); // timestamp jitter
	CHECK(comp_lw_lead_is_miss(v, v + P60, P60));          // the next vblank
}

TEST_CASE("lw lead: a run of hits probes the lead down, never below zero")
{
	comp_lw_lead l = {};
	comp_lw_lead_init(&l, P60);
	const uint64_t start = l.lead_ns;
	for (int i = 0; i < COMP_LW_LEAD_HITS_TO_LOWER - 1; i++) {
		CHECK(comp_lw_lead_on_result(&l, false, P60, 3 * MS, MARGIN) == COMP_LW_ADAPT_NONE);
	}
	CHECK(comp_lw_lead_on_result(&l, false, P60, 3 * MS, MARGIN) == COMP_LW_ADAPT_LOWERED);
	CHECK(l.lead_ns == start - COMP_LW_LEAD_STEP_DOWN_NS);

	l.lead_ns = 100 * 1000; // 0.1 ms
	l.hits_in_row = COMP_LW_LEAD_HITS_TO_LOWER - 1;
	comp_lw_lead_on_result(&l, false, P60, 3 * MS, MARGIN);
	CHECK(l.lead_ns == 0);
}

TEST_CASE("lw lead: a compositor that needs more than the slack turns the tier off, once")
{
	comp_lw_lead l = {};
	comp_lw_lead_init(&l, P60);
	// Weave cost 8 ms: slack is 16.67 - 8 - 1.5 = 7.2 ms; the lead starts at
	// 4.2 ms, so the second miss crosses it.
	CHECK(comp_lw_lead_on_result(&l, true, P60, 8 * MS, MARGIN) == COMP_LW_ADAPT_RAISED);
	CHECK(comp_lw_lead_on_result(&l, true, P60, 8 * MS, MARGIN) == COMP_LW_ADAPT_DISABLED);
	CHECK(l.off);
	CHECK(comp_lw_lead_on_result(&l, true, P60, 8 * MS, MARGIN) == COMP_LW_ADAPT_NONE);

	SECTION("a weave budget already over a period disables on the first miss")
	{
		comp_lw_lead l2 = {};
		CHECK(comp_lw_lead_on_result(&l2, true, P60, 19 * MS, MARGIN) == COMP_LW_ADAPT_DISABLED);
	}
}

TEST_CASE("lw lead: steady state under an occasional miss converges, does not run away")
{
	comp_lw_lead l = {};
	comp_lw_lead_init(&l, P60);
	// A compositor needing 5 ms of lead: frames miss below it, hit above it.
	const uint64_t needed = 5 * MS;
	for (int i = 0; i < 60 * 60; i++) { // a minute at 60 Hz
		comp_lw_lead_on_result(&l, l.lead_ns < needed, P60, 3 * MS, MARGIN);
	}
	CHECK_FALSE(l.off);
	CHECK(l.lead_ns + COMP_LW_LEAD_STEP_DOWN_NS >= needed); // at most one probe below
	CHECK(l.lead_ns < needed + COMP_LW_LEAD_STEP_UP_NS + COMP_LW_LEAD_STEP_DOWN_NS);
	// Probing costs at most one miss per probe: well under one frame a second.
	CHECK(l.misses <= 60);
}
