// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The per-screen display-processor preference store (display
 *         dashboard phase 7): the env / per-user / machine precedence and the
 *         two parsers, over the pure helpers in util/u_setting.h.
 *
 * The live accessor reads the real stores (getenv, the user's settings file,
 * HKLM); these cases drive the same chain through
 * u_setting_per_screen_resolve() with synthetic tiers, so they never touch the
 * machine's state.
 */

#include "catch_amalgamated.hpp"

#include "util/u_setting.h"

#include <string>

namespace {

const char *const kDs1 = "AUO-B194-0000ABCD";
const char *const kLaptop = "AUO-1234-00000000@DISPLAY1";

std::string
resolve(const char *key, const char *env, const char *user, const char *machine, u_setting_source *src)
{
	char buf[64];
	const char *r = u_setting_per_screen_resolve(key, env, user, machine, buf, sizeof(buf), src);
	return r != nullptr ? std::string(r) : std::string("<unset>");
}

} // namespace

TEST_CASE("per-screen env spec: key=id pairs separated by ';'", "[u_setting][per_screen]")
{
	char buf[64];
	CHECK_FALSE(u_setting_per_screen_env_lookup(nullptr, kDs1, buf, sizeof(buf)));
	CHECK_FALSE(u_setting_per_screen_env_lookup("", kDs1, buf, sizeof(buf)));

	REQUIRE(u_setting_per_screen_env_lookup("AUO-B194-0000ABCD=sim-display", kDs1, buf, sizeof(buf)));
	CHECK(std::string(buf) == "sim-display");

	// Whitespace trimmed, key case-insensitive, any position, device-qualified keys.
	const char *spec = " AUO-1234-00000000@DISPLAY1 = leia-sr ; auo-b194-0000abcd= sim-display ;";
	REQUIRE(u_setting_per_screen_env_lookup(spec, kDs1, buf, sizeof(buf)));
	CHECK(std::string(buf) == "sim-display");
	REQUIRE(u_setting_per_screen_env_lookup(spec, kLaptop, buf, sizeof(buf)));
	CHECK(std::string(buf) == "leia-sr");

	// Malformed entries never match; good ones still do.
	CHECK_FALSE(u_setting_per_screen_env_lookup("AUO-B194-0000ABCD", kDs1, buf, sizeof(buf)));
	CHECK_FALSE(u_setting_per_screen_env_lookup("AUO-B194-0000ABCD=", kDs1, buf, sizeof(buf)));
	CHECK_FALSE(u_setting_per_screen_env_lookup("=sim-display", kDs1, buf, sizeof(buf)));
	REQUIRE(u_setting_per_screen_env_lookup("junk;=x;AUO-B194-0000ABCD=leia-sr", kDs1, buf, sizeof(buf)));
	CHECK(std::string(buf) == "leia-sr");
	CHECK_FALSE(u_setting_per_screen_env_lookup("OTHER-0000-00000001=sim-display", kDs1, buf, sizeof(buf)));
}

TEST_CASE("per-screen settings file: the preferred_plugin_per_screen object", "[u_setting][per_screen]")
{
	char buf[64];
	const char *file = R"({
	  "DXR_WEAVE_REPAINT": "1",
	  "preferred_plugin_per_screen": {
	    "AUO-B194-0000ABCD": "sim-display",
	    "AUO-1234-00000000@DISPLAY1": "leia-sr",
	    "BAD-0000-00000000": 7,
	    "EMPTY-0000-00000000": ""
	  },
	  "_written": "2026-10-09"
	})";
	REQUIRE(u_setting_per_screen_json_lookup(file, kDs1, buf, sizeof(buf)));
	CHECK(std::string(buf) == "sim-display");
	REQUIRE(u_setting_per_screen_json_lookup(file, kLaptop, buf, sizeof(buf)));
	CHECK(std::string(buf) == "leia-sr");
	// Every failure is "not set".
	CHECK_FALSE(u_setting_per_screen_json_lookup(file, "BAD-0000-00000000", buf, sizeof(buf)));
	CHECK_FALSE(u_setting_per_screen_json_lookup(file, "EMPTY-0000-00000000", buf, sizeof(buf)));
	CHECK_FALSE(u_setting_per_screen_json_lookup(file, "NOPE-0000-00000000", buf, sizeof(buf)));
	CHECK_FALSE(u_setting_per_screen_json_lookup("{not json", kDs1, buf, sizeof(buf)));
	CHECK_FALSE(u_setting_per_screen_json_lookup(R"({"preferred_plugin_per_screen": "x"})", kDs1, buf, sizeof(buf)));
	CHECK_FALSE(u_setting_per_screen_json_lookup(R"([1,2])", kDs1, buf, sizeof(buf)));
	CHECK_FALSE(u_setting_per_screen_json_lookup(nullptr, kDs1, buf, sizeof(buf)));
}

TEST_CASE("per-screen chain: env > user > machine > not set", "[u_setting][per_screen]")
{
	const char *user = R"({"preferred_plugin_per_screen": {"AUO-B194-0000ABCD": "user-dp"}})";
	const char *env = "AUO-B194-0000ABCD=env-dp";
	u_setting_source src = U_SETTING_SOURCE_ENV;

	CHECK(resolve(kDs1, env, user, "machine-dp", &src) == "env-dp");
	CHECK(src == U_SETTING_SOURCE_ENV);

	CHECK(resolve(kDs1, nullptr, user, "machine-dp", &src) == "user-dp");
	CHECK(src == U_SETTING_SOURCE_USER);

	CHECK(resolve(kDs1, nullptr, nullptr, "machine-dp", &src) == "machine-dp");
	CHECK(src == U_SETTING_SOURCE_MACHINE);

	CHECK(resolve(kDs1, nullptr, nullptr, nullptr, &src) == "<unset>");
	CHECK(src == U_SETTING_SOURCE_DEFAULT);

	// A tier that names a DIFFERENT screen does not shadow a lower one.
	CHECK(resolve(kDs1, "OTHER-0000-00000001=env-dp", user, "machine-dp", &src) == "user-dp");
	CHECK(src == U_SETTING_SOURCE_USER);
	CHECK(resolve(kLaptop, env, user, nullptr, &src) == "<unset>");
	CHECK(src == U_SETTING_SOURCE_DEFAULT);

	// A broken user file is "not set", never an error: the machine tier stands.
	CHECK(resolve(kDs1, nullptr, "{broken", "machine-dp", &src) == "machine-dp");
	CHECK(src == U_SETTING_SOURCE_MACHINE);

	// No key: nothing.
	CHECK(resolve("", env, user, "machine-dp", &src) == "<unset>");
	CHECK(src == U_SETTING_SOURCE_DEFAULT);
}

TEST_CASE("per-screen preference is not a scalar option", "[u_setting][per_screen]")
{
	// Allow-listed by construction, never through the scalar allow-list: the
	// generic chain cannot read or write it.
	CHECK_FALSE(u_setting_is_managed(U_SETTING_PER_SCREEN_ENV));
	CHECK_FALSE(u_setting_is_managed(U_SETTING_PER_SCREEN_JSON_KEY));
	CHECK_FALSE(u_setting_user_set(U_SETTING_PER_SCREEN_JSON_KEY, "x"));
	// The writer refuses keys the env spec could not carry.
	CHECK_FALSE(u_setting_user_set_preferred_plugin_for_screen("", "sim-display"));
	CHECK_FALSE(u_setting_user_set_preferred_plugin_for_screen("A=B", "sim-display"));
	CHECK_FALSE(u_setting_user_set_preferred_plugin_for_screen("A;B", "sim-display"));
	CHECK_FALSE(u_setting_user_set_preferred_plugin_for_screen(nullptr, "sim-display"));
}
