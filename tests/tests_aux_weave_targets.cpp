// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pin the XR_DXR_weave v21 weave-target id table (u_weave_targets.h,
 *         browser-pvt#288): several present-owner windows on one session.
 */

#include "catch_amalgamated.hpp"

#include "util/u_weave_targets.h"

TEST_CASE("weave targets: id 0 is the default window and never takes a slot")
{
	uint64_t ids[4] = {};
	bool created = true;
	REQUIRE(u_weave_target_claim(ids, 4, 0, &created) == U_WEAVE_TARGET_NO_SLOT);
	REQUIRE_FALSE(created);
	REQUIRE(u_weave_target_find(ids, 4, 0) == U_WEAVE_TARGET_NO_SLOT);
	REQUIRE(u_weave_target_release(ids, 4, 0) == U_WEAVE_TARGET_NO_SLOT);
	REQUIRE(u_weave_target_count(ids, 4) == 0);
}

TEST_CASE("weave targets: a claim is stable — the same id keeps its slot and is created once")
{
	uint64_t ids[4] = {};
	bool created = false;
	const int a = u_weave_target_claim(ids, 4, 0x1234, &created);
	REQUIRE(a == 0);
	REQUIRE(created);
	REQUIRE(u_weave_target_claim(ids, 4, 0x1234, &created) == a);
	REQUIRE_FALSE(created);
	REQUIRE(u_weave_target_find(ids, 4, 0x1234) == a);
	// Ids are opaque 64-bit values (a browser uses its window's widget id).
	const int b = u_weave_target_claim(ids, 4, 0xffffffff00000001ull, &created);
	REQUIRE(b == 1);
	REQUIRE(created);
	REQUIRE(u_weave_target_count(ids, 4) == 2);
}

TEST_CASE("weave targets: a full table refuses a new id but still answers the ones it holds")
{
	uint64_t ids[3] = {};
	for (uint64_t id = 1; id <= 3; id++) {
		REQUIRE(u_weave_target_claim(ids, 3, id, nullptr) != U_WEAVE_TARGET_NO_SLOT);
	}
	bool created = true;
	REQUIRE(u_weave_target_claim(ids, 3, 4, &created) == U_WEAVE_TARGET_NO_SLOT);
	REQUIRE_FALSE(created);
	REQUIRE(u_weave_target_claim(ids, 3, 2, &created) == 1);
	REQUIRE_FALSE(created);
	REQUIRE(u_weave_target_count(ids, 3) == 3);
}

TEST_CASE("weave targets: release frees the slot for the next window, lowest slot first")
{
	uint64_t ids[3] = {};
	REQUIRE(u_weave_target_claim(ids, 3, 10, nullptr) == 0);
	REQUIRE(u_weave_target_claim(ids, 3, 20, nullptr) == 1);
	REQUIRE(u_weave_target_claim(ids, 3, 30, nullptr) == 2);

	// The second window closes: its slot is reported so its state is torn down.
	REQUIRE(u_weave_target_release(ids, 3, 20) == 1);
	REQUIRE(u_weave_target_find(ids, 3, 20) == U_WEAVE_TARGET_NO_SLOT);
	REQUIRE(u_weave_target_release(ids, 3, 20) == U_WEAVE_TARGET_NO_SLOT); // a repeat is a no-op
	REQUIRE(u_weave_target_count(ids, 3) == 2);

	// A new window takes the freed slot; the survivors keep theirs.
	bool created = false;
	REQUIRE(u_weave_target_claim(ids, 3, 40, &created) == 1);
	REQUIRE(created);
	REQUIRE(u_weave_target_find(ids, 3, 10) == 0);
	REQUIRE(u_weave_target_find(ids, 3, 30) == 2);

	// A released id that comes back is a NEW target (fresh state), not the old one.
	REQUIRE(u_weave_target_release(ids, 3, 10) == 0);
	REQUIRE(u_weave_target_claim(ids, 3, 10, &created) == 0);
	REQUIRE(created);
}
