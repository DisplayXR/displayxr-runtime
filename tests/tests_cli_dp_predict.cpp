// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  N3b (ADR-054): the `dp list` prediction rule and the id check
 *         behind `dp enable|disable|use`.
 *
 * What must hold (plugin-discovery.md §2 / §2.1 / §3.1, N3b):
 *  - no plug-in, or none enabled → nothing is predicted;
 *  - lowest ProbeOrder among the ENABLED plug-ins wins, first on ties;
 *  - the PreferredPlugin override outranks ProbeOrder when it names a
 *    registered, enabled plug-in;
 *  - a disabled or unregistered preferred id falls through to ProbeOrder
 *    (`Enabled=0` is the stronger verb; the loader skips it with one WARN);
 *  - `dp enable|disable` finds a registered id whether enabled or not and
 *    refuses an unregistered one (no orphan per-user row);
 *  - the id check rejects what would not name a registry subkey.
 */

#include "catch_amalgamated.hpp"

#include "cli_dp_predict.h"

#include <cstring>

namespace {

target_plugin_desc
desc(const char *id, uint32_t order, bool enabled)
{
	target_plugin_desc d{};
	std::snprintf(d.id, sizeof(d.id), "%s", id);
	d.probe_order = order;
	d.enabled = enabled;
	return d;
}

} // namespace

TEST_CASE("cli_dp_predict_active: ProbeOrder among enabled plug-ins")
{
	SECTION("empty list predicts nothing")
	{
		CHECK(cli_dp_predict_active(nullptr, 0, nullptr) == -1);
	}

	SECTION("lowest ProbeOrder wins, first on ties")
	{
		const target_plugin_desc list[] = {
		    desc("sim-display", 200, true),
		    desc("acme-panel", 50, true),
		    desc("brand-panel", 50, true),
		};
		CHECK(cli_dp_predict_active(list, 3, nullptr) == 1);
		CHECK(cli_dp_predict_active(list, 3, "") == 1);
	}

	SECTION("a disabled plug-in is never predicted — the fallback takes over")
	{
		// The acceptance case of #2049: `dp disable leia-sr` on a box with
		// the vendor plug-in + sim-display registered puts `*` on sim-display.
		const target_plugin_desc list[] = {
		    desc("leia-sr", 50, false),
		    desc("sim-display", 200, true),
		};
		CHECK(cli_dp_predict_active(list, 2, nullptr) == 1);
		const target_plugin_desc all_off[] = {desc("leia-sr", 50, false)};
		CHECK(cli_dp_predict_active(all_off, 1, nullptr) == -1);
	}
}

TEST_CASE("cli_dp_predict_active: the PreferredPlugin override")
{
	const target_plugin_desc list[] = {
	    desc("acme-panel", 50, true),
	    desc("sim-display", 200, true),
	    desc("off-panel", 10, false),
	};

	SECTION("preferred outranks ProbeOrder")
	{
		CHECK(cli_dp_predict_active(list, 3, "sim-display") == 1);
	}

	SECTION("an unregistered preferred id falls through")
	{
		CHECK(cli_dp_predict_active(list, 3, "nope") == 0);
	}

	SECTION("a disabled preferred id falls through — Enabled=0 is the stronger verb")
	{
		CHECK(cli_dp_predict_active(list, 3, "off-panel") == 0);
	}
}

TEST_CASE("cli_dp_find: the registered gate behind dp enable|disable")
{
	const target_plugin_desc list[] = {
	    desc("acme-panel", 50, true),
	    desc("off-panel", 10, false),
	};

	SECTION("a registered id is found whether enabled or not")
	{
		CHECK(cli_dp_find(list, 2, "acme-panel") == 0);
		CHECK(cli_dp_find(list, 2, "off-panel") == 1);
	}

	SECTION("an unregistered id is refused — no orphan per-user row")
	{
		CHECK(cli_dp_find(list, 2, "nope") == -1);
		CHECK(cli_dp_find(list, 2, "") == -1);
		CHECK(cli_dp_find(list, 2, nullptr) == -1);
		CHECK(cli_dp_find(nullptr, 0, "acme-panel") == -1);
	}
}

TEST_CASE("cli_dp_id_valid: what can name a plug-in row")
{
	CHECK(cli_dp_id_valid("sim-display"));
	CHECK(cli_dp_id_valid("leia-sr"));
	CHECK_FALSE(cli_dp_id_valid(nullptr));
	CHECK_FALSE(cli_dp_id_valid(""));
	CHECK_FALSE(cli_dp_id_valid("--machine"));
	CHECK_FALSE(cli_dp_id_valid("a\\b"));
	CHECK_FALSE(cli_dp_id_valid("a/b"));
	char too_long[70];
	std::memset(too_long, 'x', sizeof(too_long) - 1);
	too_long[sizeof(too_long) - 1] = '\0';
	CHECK_FALSE(cli_dp_id_valid(too_long));
}
