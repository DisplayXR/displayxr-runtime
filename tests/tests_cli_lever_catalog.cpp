// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The generated Developer-settings lever catalog (perf-settings Phase 2A).
 *
 * `cli_lever_catalog.h` is generated from the census by
 * scripts/gen_lever_catalog.py, and lint checks it is current. What is pinned
 * here is what the CLI relies on: only Tier 1 / Tier 2 entries, sorted, unique,
 * ASCII, no Tier-4 switch, and the managed (store-settable) Tier 1 levers are
 * all present, so `perf list --all` offers a Reset for every lever a store can
 * actually hold.
 */

#include "catch_amalgamated.hpp"

#include "util/u_setting.h"

#include "cli_lever_catalog.h"

#include <cstring>
#include <set>
#include <string>


TEST_CASE("cli_lever_catalog: Tier 1 and Tier 2 only, sorted by tier then name, unique")
{
	REQUIRE(CLI_LEVER_CATALOG_COUNT > 0);
	std::set<std::string> seen;
	for (size_t i = 0; i < CLI_LEVER_CATALOG_COUNT; i++) {
		const cli_lever &l = cli_lever_catalog[i];
		INFO(l.name);
		CHECK((l.tier == 1 || l.tier == 2));
		CHECK(std::strncmp(l.name, "DXR_", 4) == 0);
		CHECK(seen.insert(l.name).second);
		CHECK(l.proc != nullptr);
		CHECK(l.default_text != nullptr);
		CHECK(l.description != nullptr);
		CHECK(std::strlen(l.description) <= 200);
		if (i > 0) {
			const cli_lever &p = cli_lever_catalog[i - 1];
			CHECK((p.tier < l.tier || (p.tier == l.tier && std::strcmp(p.name, l.name) < 0)));
		}
		for (const char *s : {l.name, l.proc, l.default_text, l.description}) {
			for (const char *c = s; *c != '\0'; c++) {
				CHECK(static_cast<unsigned char>(*c) < 128);
			}
		}
	}
}

TEST_CASE("cli_lever_catalog: Tier 4 and test switches never appear")
{
	for (const char *name :
	     {"DXR_ALLOW_DEV_PLUGIN_PATHS", "DXR_ALLOW_UNVERIFIED_CONTROLLER", "DXR_WEAVE_REPAINT_FORCE",
	      "DXR_PLUGIN_EXCLUSIVE", "DXR_SIM_INPUT", "DXR_TEST_SPLIT_FAIL_STAGEA"}) {
		INFO(name);
		CHECK(cli_lever_find(name) == nullptr);
	}
	for (size_t i = 0; i < CLI_LEVER_CATALOG_COUNT; i++) {
		INFO(cli_lever_catalog[i].name);
		CHECK(std::strncmp(cli_lever_catalog[i].name, "DXR_TEST_", 9) != 0);
	}
}

TEST_CASE("cli_lever_find: lookup by exact name")
{
	const cli_lever *l = cli_lever_find("DXR_WEAVE_ON_SCANOUT");
	REQUIRE(l != nullptr);
	CHECK(l->tier == 1);
	CHECK(cli_lever_find("DXR_COMMIT_PACE") != nullptr);
	CHECK(cli_lever_find("DXR_COMMIT_PACE")->tier == 2);
	CHECK(cli_lever_find("DXR_NOT_A_LEVER") == nullptr);
	CHECK(cli_lever_find("dxr_weave_on_scanout") == nullptr);
	CHECK(cli_lever_find(nullptr) == nullptr);
}

TEST_CASE("cli_lever_catalog: the managed Tier 1 levers are listed (they get a Reset)")
{
	// The Performance page's Target GPU + Mode levers. The two Diagnostics
	// levers are Tier 3 (pure observers) and are deliberately not in the list.
	for (const char *name :
	     {"DXR_D3D_FORCE_GPU", "DXR_VK_FORCE_GPU", "DXR_WEAVE_ON_SCANOUT", "DXR_WEAVE_REPAINT"}) {
		INFO(name);
		CHECK(u_setting_is_managed(name));
		CHECK(cli_lever_find(name) != nullptr);
	}
	// And a non-managed lever stays environment-only (Phase 2A changes no allow-list).
	CHECK_FALSE(u_setting_is_managed("DXR_COMMIT_PACE"));
	CHECK_FALSE(u_setting_is_managed("DXR_LATE_WEAVE"));
}
