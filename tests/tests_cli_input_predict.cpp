// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  N3a (ADR-054): the `input list` prediction rule and the id check
 *         behind `input enable|disable|use`.
 *
 * What must hold (input-provider-discovery.md §3 "Selection", N3a):
 *  - no provider, or none enabled → nothing is predicted;
 *  - lowest ProbeOrder among the ENABLED providers wins, first on ties;
 *  - the PreferredPlugin override outranks ProbeOrder when it names a
 *    registered, enabled provider;
 *  - a disabled or unregistered preferred id falls through to ProbeOrder
 *    (`Enabled=0` is the stronger verb; the loader skips it too);
 *  - the id check rejects what would not name a registry subkey.
 */

#include "catch_amalgamated.hpp"

#include "cli_input_predict.h"

#include <cstring>

namespace {

target_input_plugin_desc
desc(const char *id, uint32_t order, bool enabled)
{
	target_input_plugin_desc d{};
	std::snprintf(d.id, sizeof(d.id), "%s", id);
	d.probe_order = order;
	d.enabled = enabled;
	return d;
}

} // namespace

TEST_CASE("cli_input_predict_active: ProbeOrder among enabled providers")
{
	SECTION("empty list predicts nothing")
	{
		CHECK(cli_input_predict_active(nullptr, 0, nullptr) == -1);
	}

	SECTION("lowest ProbeOrder wins, first on ties")
	{
		const target_input_plugin_desc list[] = {
		    desc("sim-input", 200, true),
		    desc("a-hands", 50, true),
		    desc("b-hands", 50, true),
		};
		CHECK(cli_input_predict_active(list, 3, nullptr) == 1);
		CHECK(cli_input_predict_active(list, 3, "") == 1);
	}

	SECTION("a disabled provider is never predicted")
	{
		const target_input_plugin_desc list[] = {
		    desc("a-hands", 50, false),
		    desc("sim-input", 200, true),
		};
		CHECK(cli_input_predict_active(list, 2, nullptr) == 1);
		const target_input_plugin_desc all_off[] = {desc("a-hands", 50, false)};
		CHECK(cli_input_predict_active(all_off, 1, nullptr) == -1);
	}
}

TEST_CASE("cli_input_predict_active: the PreferredPlugin override")
{
	const target_input_plugin_desc list[] = {
	    desc("a-hands", 50, true),
	    desc("sim-input", 200, true),
	    desc("off-hands", 10, false),
	};

	SECTION("preferred outranks ProbeOrder")
	{
		CHECK(cli_input_predict_active(list, 3, "sim-input") == 1);
	}

	SECTION("an unregistered preferred id falls through")
	{
		CHECK(cli_input_predict_active(list, 3, "nope") == 0);
	}

	SECTION("a disabled preferred id falls through — Enabled=0 is the stronger verb")
	{
		CHECK(cli_input_predict_active(list, 3, "off-hands") == 0);
	}
}

TEST_CASE("cli_input_id_valid: what can name a provider row")
{
	CHECK(cli_input_id_valid("sim-input"));
	CHECK(cli_input_id_valid("ultraleap"));
	CHECK_FALSE(cli_input_id_valid(nullptr));
	CHECK_FALSE(cli_input_id_valid(""));
	CHECK_FALSE(cli_input_id_valid("--machine"));
	CHECK_FALSE(cli_input_id_valid("a\\b"));
	CHECK_FALSE(cli_input_id_valid("a/b"));
	char too_long[70];
	std::memset(too_long, 'x', sizeof(too_long) - 1);
	too_long[sizeof(too_long) - 1] = '\0';
	CHECK_FALSE(cli_input_id_valid(too_long));
	char max_ok[64];
	std::memset(max_ok, 'x', 63);
	max_ok[63] = '\0';
	CHECK(cli_input_id_valid(max_ok));
}
