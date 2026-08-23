// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #918 Phase 3 — the output-device split's activation decision.
 * @ingroup tests
 *
 * Phase 3 flipped `DXR_WEAVE_ON_SCANOUT` from an opt-in to a kill switch, which
 * is the kind of change that is invisible until a user reports it. A default is
 * exactly the sort of thing that gets "tidied" back by a later refactor, so it
 * is pinned here rather than left to a hardware session to notice.
 *
 * `comp_split_gate_env_requested()` latches for process lifetime and so can only
 * ever be tested for ONE value; the unlatched
 * @ref comp_split_gate_parse_requested is what these exercise. None of this
 * needs a GPU — the gate deliberately owns no graphics types.
 */

#include "comp_split_gate.h"

#include "catch_amalgamated.hpp"

#include <string.h>

static struct comp_split_luid
luid(uint32_t low, int32_t high)
{
	struct comp_split_luid l = {low, high};
	return l;
}

TEST_CASE("#918 Phase 3: the split is REQUESTED by default")
{
	SECTION("unset means allowed — the whole point of Phase 3")
	{
		CHECK(comp_split_gate_parse_requested(nullptr));
		CHECK(comp_split_gate_parse_requested(""));
	}

	SECTION("the documented false spellings kill it")
	{
		CHECK_FALSE(comp_split_gate_parse_requested("0"));
		CHECK_FALSE(comp_split_gate_parse_requested("f"));
		CHECK_FALSE(comp_split_gate_parse_requested("false"));
		CHECK_FALSE(comp_split_gate_parse_requested("FALSE"));
		CHECK_FALSE(comp_split_gate_parse_requested("n"));
		CHECK_FALSE(comp_split_gate_parse_requested("no"));
		CHECK_FALSE(comp_split_gate_parse_requested("NO"));
		CHECK_FALSE(comp_split_gate_parse_requested("off"));
		CHECK_FALSE(comp_split_gate_parse_requested("OFF"));
	}

	SECTION("=1 still means allowed, so no shipped script or doc changed meaning")
	{
		CHECK(comp_split_gate_parse_requested("1"));
		CHECK(comp_split_gate_parse_requested("true"));
		CHECK(comp_split_gate_parse_requested("yes"));
		CHECK(comp_split_gate_parse_requested("on"));
	}

	SECTION("'on' and 'off' share a leading character, so 'off' is matched whole")
	{
		// The bug this guards: a bare leading-character test reads 'o' and has
		// to pick one, and picking wrong turns the kill switch into a no-op.
		CHECK(comp_split_gate_parse_requested("on"));
		CHECK_FALSE(comp_split_gate_parse_requested("off"));
	}
}

TEST_CASE("#918: the gate always names a short reason when the split is off")
{
	struct comp_split_gate_inputs in = {};
	struct comp_split_gate_result out = {};

	SECTION("killed by env")
	{
		in.requested = false;
		comp_split_gate_evaluate(&in, &out);
		CHECK_FALSE(out.split_active);
		REQUIRE(out.short_reason != nullptr);
		CHECK(strcmp(out.short_reason, COMP_SPLIT_REASON_KILLED_BY_ENV) == 0);
	}

	SECTION("the caller's own eligibility verdict passes straight through")
	{
		in.requested = true;
		in.ineligible_reason = COMP_SPLIT_REASON_NO_HWND;
		comp_split_gate_evaluate(&in, &out);
		CHECK_FALSE(out.split_active);
		REQUIRE(out.short_reason != nullptr);
		CHECK(strcmp(out.short_reason, COMP_SPLIT_REASON_NO_HWND) == 0);
	}

	SECTION("an unresolvable scanout adapter is not silently a no-op")
	{
		in.requested = true;
		in.scanout_resolved = false;
		comp_split_gate_evaluate(&in, &out);
		CHECK_FALSE(out.split_active);
		REQUIRE(out.short_reason != nullptr);
		CHECK(strcmp(out.short_reason, COMP_SPLIT_REASON_SCANOUT_UNRESOLVABLE) == 0);
	}

	SECTION("same adapter is a no-op, and STILL reports a reason")
	{
		// The regression this pins: `reason` is deliberately "" here (there is
		// nothing for a human to read), and before Phase 3 that emptiness was
		// also the log's answer.
		in.requested = true;
		in.scanout_resolved = true;
		in.render_luid = luid(0x1234, 0);
		in.scanout_luid = luid(0x1234, 0);
		comp_split_gate_evaluate(&in, &out);
		CHECK_FALSE(out.split_active);
		CHECK(out.same_adapter);
		REQUIRE(out.reason != nullptr);
		CHECK(strcmp(out.reason, COMP_SPLIT_REASON_HANDLED) == 0);
		REQUIRE(out.short_reason != nullptr);
		CHECK(strcmp(out.short_reason, COMP_SPLIT_REASON_SAME_ADAPTER) == 0);
	}
}

TEST_CASE("#918: differing adapters engage the split, with nothing left to report")
{
	struct comp_split_gate_inputs in = {};
	struct comp_split_gate_result out = {};

	in.requested = true;
	in.scanout_resolved = true;
	in.render_luid = luid(0x1234, 0);
	in.scanout_luid = luid(0x5678, 0);
	comp_split_gate_evaluate(&in, &out);

	CHECK(out.split_active);
	CHECK_FALSE(out.same_adapter);
	CHECK(out.reason == nullptr);
	CHECK(out.short_reason == nullptr);
	// The output half belongs on the SCANOUT adapter — the one sentence of
	// ADR-037 §1 the gate is responsible for.
	CHECK(comp_split_luid_equal(out.out_adapter_luid, in.scanout_luid));
}

TEST_CASE("#918: a NULL input block is the kill switch, not a crash")
{
	struct comp_split_gate_result out = {};
	comp_split_gate_evaluate(nullptr, &out);
	CHECK_FALSE(out.split_active);
	REQUIRE(out.short_reason != nullptr);
	CHECK(strcmp(out.short_reason, COMP_SPLIT_REASON_KILLED_BY_ENV) == 0);
}
