// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief N3c (ADR-054 decision 7): the one options contract's store — the
 *        resolution order, the `DXR_<CLASS>_<ID>_<KEY>` mapping, the per-user
 *        JSON lookup, and (desktop POSIX) a live set / get / unset round trip
 *        through a temp manifest dir + per-user file.
 *
 * What must hold (plugin-discovery.md § Component settings):
 *  - environment > user > machine / manifest > absent, and the environment
 *    wins even under the machine's Policy\Lock (the u_setting rule);
 *  - Policy\Lock hides the user tier, never the environment;
 *  - a set-but-empty environment variable is unset;
 *  - `DXR_<CLASS>_<ID>_<KEY>`: id and key upper-cased, `-` → `_`;
 *  - keys are `[A-Za-z0-9_]`, not digit-first, matched case-insensitively;
 *  - the per-user file is a flat object; strings, numbers and booleans are
 *    values, anything else is absent.
 *
 * Host-only, no GPU. Nothing here touches HKLM or a real per-user dir: the
 * live round trip redirects HOME / XDG_DATA_HOME and XRT_PLUGIN_SEARCH_PATH
 * to throwaway directories.
 */

#include "target_component_loader.h"

#include "xrt/xrt_component_settings.h"

#include "catch_amalgamated.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif


namespace {

using Class = ::target_component_class;

void
set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value != nullptr ? value : "");
#else
	if (value != nullptr) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
#endif
}

Class
test_class(const char *env_class)
{
	Class c;
	std::memset(&c, 0, sizeof(c));
	c.name = "test component";
	c.log_tag = "test loader";
	c.entrypoint = "xrtTestNegotiate";
	c.abi_current = 1;
	c.registry_root = "Software\\DisplayXR-Test\\NoSuchRoot";
	c.manifest_kind = TARGET_PLUGIN_MANIFEST_DISPLAY;
	c.android_prefix = nullptr;
	c.env_class = env_class;
	return c;
}

} // namespace


/*
 *
 * Pure helpers.
 *
 */

TEST_CASE("component settings: the resolution order is environment > user > stored > absent", "[component_settings]")
{
	char buf[64];
	uint32_t src = 99;

	SECTION("nothing set it")
	{
		CHECK(target_component_setting_resolve(nullptr, nullptr, nullptr, false, false, buf, sizeof(buf),
		                                       &src) == nullptr);
		CHECK(src == TARGET_COMPONENT_SETTING_ABSENT);
		CHECK(buf[0] == '\0');
	}

	SECTION("the machine store / manifest is the floor")
	{
		CHECK(std::string(target_component_setting_resolve(nullptr, nullptr, "m", false, false, buf,
		                                                   sizeof(buf), &src)) == "m");
		CHECK(src == TARGET_COMPONENT_SETTING_MACHINE);
		CHECK(std::string(target_component_setting_resolve(nullptr, nullptr, "m", true, false, buf, sizeof(buf),
		                                                   &src)) == "m");
		CHECK(src == TARGET_COMPONENT_SETTING_MANIFEST);
	}

	SECTION("the user store outranks the machine store")
	{
		CHECK(std::string(target_component_setting_resolve(nullptr, "u", "m", false, false, buf, sizeof(buf),
		                                                   &src)) == "u");
		CHECK(src == TARGET_COMPONENT_SETTING_USER);
	}

	SECTION("Policy\\Lock hides the user store, and only the user store")
	{
		CHECK(std::string(target_component_setting_resolve(nullptr, "u", "m", false, true, buf, sizeof(buf),
		                                                   &src)) == "m");
		CHECK(src == TARGET_COMPONENT_SETTING_MACHINE);
		CHECK(target_component_setting_resolve(nullptr, "u", nullptr, false, true, buf, sizeof(buf), &src) ==
		      nullptr);
		CHECK(src == TARGET_COMPONENT_SETTING_ABSENT);
	}

	SECTION("the environment always wins — even under the lock (the u_setting rule)")
	{
		CHECK(std::string(target_component_setting_resolve("e", "u", "m", false, false, buf, sizeof(buf),
		                                                   &src)) == "e");
		CHECK(src == TARGET_COMPONENT_SETTING_ENVIRONMENT);
		CHECK(std::string(
		          target_component_setting_resolve("e", "u", "m", false, true, buf, sizeof(buf), &src)) == "e");
		CHECK(src == TARGET_COMPONENT_SETTING_ENVIRONMENT);
	}

	SECTION("a set-but-empty environment variable is unset")
	{
		CHECK(std::string(
		          target_component_setting_resolve("", "u", "m", false, false, buf, sizeof(buf), &src)) == "u");
		CHECK(src == TARGET_COMPONENT_SETTING_USER);
	}

	SECTION("a NULL source pointer is allowed, and the value is truncated to the buffer")
	{
		char tiny[3];
		CHECK(std::string(target_component_setting_resolve("abcdef", nullptr, nullptr, false, false, tiny,
		                                                   sizeof(tiny), nullptr)) == "ab");
	}
}

TEST_CASE("component settings: the environment name is DXR_<CLASS>_<ID>_<KEY>", "[component_settings]")
{
	char out[TARGET_COMPONENT_SETTING_ENV_MAX];

	// The three shipping classes against their reference components.
	REQUIRE(target_component_setting_env_name("CONV", "sim-conversion", "ENABLE", out, sizeof(out)));
	CHECK(std::string(out) == "DXR_CONV_SIM_CONVERSION_ENABLE");
	REQUIRE(target_component_setting_env_name("INPUT", "sim-input", "ENABLE", out, sizeof(out)));
	CHECK(std::string(out) == "DXR_INPUT_SIM_INPUT_ENABLE");
	REQUIRE(target_component_setting_env_name("DP", "sim-display", "TRACKING_PERIOD_MS", out, sizeof(out)));
	CHECK(std::string(out) == "DXR_DP_SIM_DISPLAY_TRACKING_PERIOD_MS");

	// Lower-case key spellings map to the same variable (keys are case-insensitive).
	REQUIRE(target_component_setting_env_name("DP", "acme-panel", "tracking", out, sizeof(out)));
	CHECK(std::string(out) == "DXR_DP_ACME_PANEL_TRACKING");

	// Missing / invalid parts.
	CHECK_FALSE(target_component_setting_env_name(nullptr, "sim-display", "TRACKING", out, sizeof(out)));
	CHECK(out[0] == '\0');
	CHECK_FALSE(target_component_setting_env_name("DP", "", "TRACKING", out, sizeof(out)));
	CHECK_FALSE(target_component_setting_env_name("DP", "sim-display", "", out, sizeof(out)));
	CHECK_FALSE(target_component_setting_env_name("DP", "sim-display", "BAD KEY", out, sizeof(out)));
	CHECK_FALSE(target_component_setting_env_name("DP", "a/b", "KEY", out, sizeof(out))); // not a variable name
	char small[8];
	CHECK_FALSE(target_component_setting_env_name("DP", "sim-display", "TRACKING", small, sizeof(small)));
}

TEST_CASE("component settings: what can be a key", "[component_settings]")
{
	CHECK(target_component_setting_key_valid("ENABLE"));
	CHECK(target_component_setting_key_valid("TRACKING_PERIOD_MS"));
	CHECK(target_component_setting_key_valid("STATE_4"));
	CHECK(target_component_setting_key_valid("enable")); // matched case-insensitively
	CHECK(target_component_setting_key_valid("_x"));
	CHECK_FALSE(target_component_setting_key_valid(nullptr));
	CHECK_FALSE(target_component_setting_key_valid(""));
	CHECK_FALSE(target_component_setting_key_valid("4STATE"));
	CHECK_FALSE(target_component_setting_key_valid("A-B"));
	CHECK_FALSE(target_component_setting_key_valid("A B"));
	CHECK_FALSE(target_component_setting_key_valid("A\\B"));
	char too_long[TARGET_COMPONENT_SETTING_KEY_MAX + 1];
	std::memset(too_long, 'K', sizeof(too_long) - 1);
	too_long[sizeof(too_long) - 1] = '\0';
	CHECK_FALSE(target_component_setting_key_valid(too_long));
	too_long[TARGET_COMPONENT_SETTING_KEY_MAX - 1] = '\0';
	CHECK(target_component_setting_key_valid(too_long));
}

TEST_CASE("component settings: the per-user file lookup", "[component_settings]")
{
	const char *json =
	    R"({"ENABLE": "1", "PERIOD_MS": 250, "FAST": true, "SLOW": false, "NESTED": {"x": 1}, "NIL": null})";
	char buf[64];

	CHECK(target_component_user_settings_json_lookup(json, "ENABLE", buf, sizeof(buf)));
	CHECK(std::string(buf) == "1");
	CHECK(target_component_user_settings_json_lookup(json, "enable", buf, sizeof(buf))); // case-insensitive
	CHECK(std::string(buf) == "1");
	CHECK(target_component_user_settings_json_lookup(json, "PERIOD_MS", buf, sizeof(buf)));
	CHECK(std::string(buf) == "250");
	CHECK(target_component_user_settings_json_lookup(json, "FAST", buf, sizeof(buf)));
	CHECK(std::string(buf) == "1");
	CHECK(target_component_user_settings_json_lookup(json, "SLOW", buf, sizeof(buf)));
	CHECK(std::string(buf) == "0");
	CHECK_FALSE(target_component_user_settings_json_lookup(json, "NESTED", buf, sizeof(buf)));
	CHECK_FALSE(target_component_user_settings_json_lookup(json, "NIL", buf, sizeof(buf)));
	CHECK_FALSE(target_component_user_settings_json_lookup(json, "ABSENT", buf, sizeof(buf)));
	CHECK(buf[0] == '\0');
	CHECK_FALSE(target_component_user_settings_json_lookup("not json", "ENABLE", buf, sizeof(buf)));
	CHECK_FALSE(target_component_user_settings_json_lookup("[1,2]", "ENABLE", buf, sizeof(buf)));
	CHECK_FALSE(target_component_user_settings_json_lookup(nullptr, "ENABLE", buf, sizeof(buf)));
}

TEST_CASE("component settings: source names are stable", "[component_settings]")
{
	CHECK(std::string(target_component_setting_source_str(TARGET_COMPONENT_SETTING_ABSENT)) == "absent");
	CHECK(std::string(target_component_setting_source_str(TARGET_COMPONENT_SETTING_ENVIRONMENT)) == "environment");
	CHECK(std::string(target_component_setting_source_str(TARGET_COMPONENT_SETTING_USER)) == "user");
	CHECK(std::string(target_component_setting_source_str(TARGET_COMPONENT_SETTING_MACHINE)) == "machine");
	CHECK(std::string(target_component_setting_source_str(TARGET_COMPONENT_SETTING_MANIFEST)) == "manifest");
	CHECK(std::string(target_component_setting_source_str(1234)) == "absent");
}

TEST_CASE("component settings: a plug-in's declaration merges with a schema by key", "[component_settings]")
{
	target_component_setting_desc out[4];
	std::memset(out, 0, sizeof(out));
	std::snprintf(out[0].key, sizeof(out[0].key), "%s", "ENABLE");
	std::snprintf(out[0].type, sizeof(out[0].type), "%s", "bool");

	const xrt_component_setting_desc declared[] = {
	    {"enable", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "ignored: the schema already has it"},
	    {"PERIOD_MS", XRT_COMPONENT_SETTING_TYPE_INT, "0", "square-wave period"},
	    {"bad key", XRT_COMPONENT_SETTING_TYPE_INT, "0", "skipped"},
	    {"TRACKING", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "fake tracking"},
	    {"OVERFLOW", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "does not fit"},
	};
	const int n = target_component_merge_setting_descs(out, 1, 3, declared, 5);
	REQUIRE(n == 3);                           // ENABLE kept, PERIOD_MS + TRACKING added, OVERFLOW does not fit
	CHECK(std::string(out[0].type) == "bool"); // the schema entry was kept as-is
	CHECK(std::string(out[1].key) == "PERIOD_MS");
	CHECK(std::string(out[1].type) == "int");
	CHECK(std::string(out[1].help) == "square-wave period");
	CHECK(std::string(out[2].key) == "TRACKING");

	// Degenerate inputs.
	CHECK(target_component_merge_setting_descs(out, 3, 4, nullptr, 0) == 3);
	CHECK(target_component_merge_setting_descs(nullptr, 0, 4, declared, 5) == 0);
}


/*
 *
 * The chain against the real stores — environment only, every platform.
 *
 */

TEST_CASE("component settings: the environment tier resolves through the chain", "[component_settings]")
{
	const Class cls = test_class("TEST");
	char buf[64];
	uint32_t src = 99;

	set_env("DXR_TEST_ACME_THING_KNOB", nullptr);
	CHECK_FALSE(target_component_get_setting(&cls, "acme-thing", "KNOB", buf, sizeof(buf), &src));
	CHECK(src == TARGET_COMPONENT_SETTING_ABSENT);
	CHECK(buf[0] == '\0');

	set_env("DXR_TEST_ACME_THING_KNOB", "42");
	CHECK(target_component_get_setting(&cls, "acme-thing", "KNOB", buf, sizeof(buf), &src));
	CHECK(std::string(buf) == "42");
	CHECK(src == TARGET_COMPONENT_SETTING_ENVIRONMENT);
	// Key case does not matter; the variable name is canonical.
	CHECK(target_component_get_setting(&cls, "acme-thing", "knob", buf, sizeof(buf), &src));
	CHECK(src == TARGET_COMPONENT_SETTING_ENVIRONMENT);
	set_env("DXR_TEST_ACME_THING_KNOB", nullptr);

	// A class without an environment token has no environment tier.
	const Class no_env = test_class(nullptr);
	set_env("DXR__ACME_THING_KNOB", "1");
	CHECK_FALSE(target_component_get_setting(&no_env, "acme-thing", "KNOB", buf, sizeof(buf), &src));
	set_env("DXR__ACME_THING_KNOB", nullptr);

	// Bad arguments never resolve.
	CHECK_FALSE(target_component_get_setting(&cls, "acme-thing", "bad key", buf, sizeof(buf), &src));
	CHECK_FALSE(target_component_get_setting(&cls, "", "KNOB", buf, sizeof(buf), &src));
	CHECK_FALSE(target_component_get_setting(nullptr, "acme-thing", "KNOB", buf, sizeof(buf), &src));
	CHECK_FALSE(target_component_get_setting(&cls, "acme-thing", "KNOB", nullptr, 0, &src));
}


/*
 *
 * Desktop POSIX: the manifest tier + the per-user file, live, in temp dirs.
 *
 */

#if !defined(_WIN32) && !defined(__ANDROID__)

namespace {

std::string
tmp_root()
{
	const char *t = std::getenv("TMPDIR");
	std::string root = (t != nullptr && t[0] != '\0') ? t : "/tmp";
	if (root.back() == '/') {
		root.pop_back();
	}
	root += "/tests_component_settings_" + std::to_string((unsigned long)getpid());
	return root;
}

void
mkdir_p(const std::string &path)
{
	std::string cur;
	for (size_t i = 0; i < path.size(); i++) {
		cur += path[i];
		if (path[i] == '/' && cur.size() > 1) {
			(void)mkdir(cur.c_str(), 0755);
		}
	}
	(void)mkdir(path.c_str(), 0755);
}

void
write_file(const std::string &path, const char *text)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	REQUIRE(f != nullptr);
	std::fputs(text, f);
	std::fclose(f);
}

std::string
read_file(const std::string &path)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr) {
		return "";
	}
	std::string s;
	char buf[512];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		s.append(buf, n);
	}
	std::fclose(f);
	return s;
}

//! HOME / XDG_DATA_HOME + XRT_PLUGIN_SEARCH_PATH redirected to throwaway dirs for one test.
struct Sandbox
{
	std::string root = tmp_root();
	std::string manifests = root + "/manifests";
	std::string home = root + "/home";
	std::string user_dir;
	std::string saved_home;
	std::string saved_xdg;
	std::string saved_search;
	bool had_xdg = false;
	bool had_search = false;

	Sandbox()
	{
		const char *h = std::getenv("HOME");
		saved_home = h != nullptr ? h : "";
		const char *x = std::getenv("XDG_DATA_HOME");
		had_xdg = x != nullptr;
		saved_xdg = had_xdg ? x : "";
		const char *s = std::getenv("XRT_PLUGIN_SEARCH_PATH");
		had_search = s != nullptr;
		saved_search = had_search ? s : "";

		mkdir_p(manifests);
#ifdef __APPLE__
		user_dir = home + "/Library/Application Support/DisplayXR/DisplayProcessors";
#else
		user_dir = home + "/xdg/DisplayXR/DisplayProcessors";
		set_env("XDG_DATA_HOME", (home + "/xdg").c_str());
#endif
		mkdir_p(user_dir);
		set_env("HOME", home.c_str());
		set_env("XRT_PLUGIN_SEARCH_PATH", manifests.c_str());
	}

	~Sandbox()
	{
		set_env("HOME", saved_home.c_str());
		set_env("XDG_DATA_HOME", had_xdg ? saved_xdg.c_str() : nullptr);
		set_env("XRT_PLUGIN_SEARCH_PATH", had_search ? saved_search.c_str() : nullptr);
		// Best-effort cleanup of what the test wrote.
		std::remove((user_dir + "/acme-thing.settings.json").c_str());
		std::remove((manifests + "/050-acme-thing.json").c_str());
	}
};

} // namespace

TEST_CASE("component settings: POSIX manifest tier, per-user file and the environment, live", "[component_settings]")
{
	Sandbox sb;
	const Class cls = test_class("TEST");
	write_file(sb.manifests + "/050-acme-thing.json", R"({
	  "file_format_version": "1.0",
	  "plugin": {
	    "id": "acme-thing",
	    "binary_path": "/opt/acme/libacme.so",
	    "settings": { "ENABLE": "1", "PERIOD_MS": 100 },
	    "settings_schema": [
	      { "key": "ENABLE", "type": "bool", "default": "0", "help": "turn it on" },
	      { "key": "PERIOD_MS", "type": "int", "default": "0", "help": "square wave" },
	      { "key": "bad key", "type": "int", "default": "0", "help": "skipped" },
	      { "key": "enable", "type": "bool", "default": "9", "help": "duplicate, skipped" }
	    ]
	  }
	})");

	char buf[64];
	uint32_t src = 99;

	// Tier 3: the manifest's settings object.
	REQUIRE(target_component_get_setting(&cls, "acme-thing", "ENABLE", buf, sizeof(buf), &src));
	CHECK(std::string(buf) == "1");
	CHECK(src == TARGET_COMPONENT_SETTING_MANIFEST);
	REQUIRE(target_component_get_setting(&cls, "acme-thing", "period_ms", buf, sizeof(buf), &src));
	CHECK(std::string(buf) == "100");
	CHECK_FALSE(target_component_get_setting(&cls, "acme-thing", "OTHER", buf, sizeof(buf), &src));
	CHECK(src == TARGET_COMPONENT_SETTING_ABSENT);
	CHECK_FALSE(target_component_get_setting(&cls, "no-such-thing", "ENABLE", buf, sizeof(buf), &src));

	// The manifest's schema, de-duplicated and validated.
	target_component_setting_desc descs[8];
	const int nd = target_component_manifest_settings_schema(&cls, "acme-thing", descs, 8);
	REQUIRE(nd == 2);
	CHECK(std::string(descs[0].key) == "ENABLE");
	CHECK(std::string(descs[0].type) == "bool");
	CHECK(std::string(descs[0].default_value) == "0");
	CHECK(std::string(descs[0].help) == "turn it on");
	CHECK(std::string(descs[1].key) == "PERIOD_MS");
	CHECK(target_component_manifest_settings_schema(&cls, "no-such-thing", descs, 8) == 0);

	// Tier 2: the per-user file, written by the CLI's `set`; it wins over the manifest.
	CHECK(target_component_set_setting(&cls, "acme-thing", "enable", "0", false) == TARGET_COMPONENT_STORE_OK);
	CHECK(target_component_set_setting(&cls, "acme-thing", "OTHER", "x y", false) == TARGET_COMPONENT_STORE_OK);
	const std::string user_file = sb.user_dir + "/acme-thing.settings.json";
	CHECK(read_file(user_file).find("\"ENABLE\"") != std::string::npos); // keys are stored upper-case
	REQUIRE(target_component_get_setting(&cls, "acme-thing", "ENABLE", buf, sizeof(buf), &src));
	CHECK(std::string(buf) == "0");
	CHECK(src == TARGET_COMPONENT_SETTING_USER);
	REQUIRE(target_component_get_setting(&cls, "acme-thing", "OTHER", buf, sizeof(buf), &src));
	CHECK(std::string(buf) == "x y");
	CHECK(src == TARGET_COMPONENT_SETTING_USER);
	char path[512];
	REQUIRE(target_component_setting_store_path(&cls, "acme-thing", false, path, sizeof(path)));
	CHECK(std::string(path) == user_file);
	REQUIRE(target_component_setting_store_path(&cls, "acme-thing", true, path, sizeof(path)));
	CHECK(std::string(path) == sb.manifests + "/050-acme-thing.json");

	// `--machine` has no store on POSIX: the manifest is edited by hand.
	CHECK(target_component_set_setting(&cls, "acme-thing", "ENABLE", "1", true) ==
	      TARGET_COMPONENT_STORE_UNSUPPORTED);

	// The listing: the union of both stores, each through the chain.
	target_component_setting_view views[8];
	const int nv = target_component_list_settings(&cls, "acme-thing", views, 8);
	REQUIRE(nv == 3);
	bool saw_enable = false, saw_period = false, saw_other = false;
	for (int i = 0; i < nv; i++) {
		const std::string key = views[i].key;
		if (key == "ENABLE") {
			saw_enable = true;
			CHECK(std::string(views[i].value) == "0");
			CHECK(views[i].source == TARGET_COMPONENT_SETTING_USER);
		} else if (key == "PERIOD_MS") {
			saw_period = true;
			CHECK(std::string(views[i].value) == "100");
			CHECK(views[i].source == TARGET_COMPONENT_SETTING_MANIFEST);
		} else if (key == "OTHER") {
			saw_other = true;
			CHECK(views[i].source == TARGET_COMPONENT_SETTING_USER);
		}
	}
	CHECK((saw_enable && saw_period && saw_other));

	// Tier 1: the environment beats both stores, and shows as such in the listing.
	set_env("DXR_TEST_ACME_THING_ENABLE", "1");
	REQUIRE(target_component_get_setting(&cls, "acme-thing", "ENABLE", buf, sizeof(buf), &src));
	CHECK(std::string(buf) == "1");
	CHECK(src == TARGET_COMPONENT_SETTING_ENVIRONMENT);
	const int nv2 = target_component_list_settings(&cls, "acme-thing", views, 8);
	for (int i = 0; i < nv2; i++) {
		if (std::string(views[i].key) == "ENABLE") {
			CHECK(views[i].source == TARGET_COMPONENT_SETTING_ENVIRONMENT);
		}
	}
	set_env("DXR_TEST_ACME_THING_ENABLE", nullptr);

	// `unset` reverts to the manifest; unsetting the last key removes the file.
	CHECK(target_component_unset_setting(&cls, "acme-thing", "ENABLE", false) == TARGET_COMPONENT_STORE_OK);
	REQUIRE(target_component_get_setting(&cls, "acme-thing", "ENABLE", buf, sizeof(buf), &src));
	CHECK(std::string(buf) == "1");
	CHECK(src == TARGET_COMPONENT_SETTING_MANIFEST);
	CHECK(target_component_unset_setting(&cls, "acme-thing", "OTHER", false) == TARGET_COMPONENT_STORE_OK);
	CHECK(read_file(user_file).empty());
	CHECK(target_component_unset_setting(&cls, "acme-thing", "OTHER", false) == TARGET_COMPONENT_STORE_OK);
	CHECK(target_component_unset_setting(&cls, "acme-thing", "ENABLE", true) == TARGET_COMPONENT_STORE_UNSUPPORTED);

	// Invalid keys / oversize values are refused before any write.
	CHECK(target_component_set_setting(&cls, "acme-thing", "bad key", "1", false) == TARGET_COMPONENT_STORE_FAILED);
	std::string huge(TARGET_COMPONENT_SETTING_VALUE_MAX, 'v');
	CHECK(target_component_set_setting(&cls, "acme-thing", "ENABLE", huge.c_str(), false) ==
	      TARGET_COMPONENT_STORE_FAILED);
	CHECK(read_file(user_file).empty());
}

#endif /* desktop POSIX */
