// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The `EmulatedControllers` gate's value / source resolution (ADR-054
 *        N7, #2143).
 *
 * Two halves. The root-policy rule the Windows read is built on
 * (`target_component_policy_resolve`) is pure, so its whole matrix is pinned
 * here on every platform: user over machine, `Policy\Lock` hides the user
 * tier, absent leaves the value alone. The input loader's own read
 * (`target_input_plugin_get_emulated_controllers`) is then driven live
 * through the POSIX per-user file with HOME / XDG redirected to a throwaway
 * dir: absent = on / "default", '0' = off / "file", anything else = on /
 * "file". The registry half is covered by the Windows conformance cell in
 * build-windows.yml (it needs HKLM).
 *
 * Host-only, no GPU, no plug-in.
 */

#include "target_component_loader.h"
#include "target_input_plugin_loader.h"

#include "catch_amalgamated.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif


TEST_CASE("emulated controllers gate: the root-policy rule is user over machine, under Lock", "[input_emulated]")
{
	uint32_t v = 7;

	SECTION("neither tier: absent, value untouched")
	{
		CHECK(target_component_policy_resolve(false, 0, false, 0, false, &v) ==
		      TARGET_COMPONENT_SETTING_ABSENT);
		CHECK(v == 7);
		CHECK(target_component_policy_resolve(false, 0, false, 0, true, &v) == TARGET_COMPONENT_SETTING_ABSENT);
		CHECK(v == 7);
	}
	SECTION("machine only")
	{
		CHECK(target_component_policy_resolve(false, 1, true, 0, false, &v) ==
		      TARGET_COMPONENT_SETTING_MACHINE);
		CHECK(v == 0);
	}
	SECTION("user only wins; under Lock it is invisible")
	{
		CHECK(target_component_policy_resolve(true, 0, false, 1, false, &v) == TARGET_COMPONENT_SETTING_USER);
		CHECK(v == 0);
		v = 7;
		CHECK(target_component_policy_resolve(true, 0, false, 1, true, &v) == TARGET_COMPONENT_SETTING_ABSENT);
		CHECK(v == 7);
	}
	SECTION("both: user wins, Lock flips it to the machine")
	{
		CHECK(target_component_policy_resolve(true, 1, true, 0, false, &v) == TARGET_COMPONENT_SETTING_USER);
		CHECK(v == 1);
		CHECK(target_component_policy_resolve(true, 1, true, 0, true, &v) == TARGET_COMPONENT_SETTING_MACHINE);
		CHECK(v == 0);
	}
	SECTION("a NULL out-value is allowed")
	{
		CHECK(target_component_policy_resolve(true, 1, true, 0, false, nullptr) ==
		      TARGET_COMPONENT_SETTING_USER);
	}
}

TEST_CASE("emulated controllers gate: the source is never NULL and defaults to on", "[input_emulated]")
{
	// Whatever the box holds, the call is total: a source string always comes
	// back, and a NULL out-source is accepted.
	const char *source = nullptr;
	(void)target_input_plugin_get_emulated_controllers(&source);
	REQUIRE(source != nullptr);
	CHECK(std::strlen(source) > 0);
	(void)target_input_plugin_get_emulated_controllers(nullptr);
}

#if !defined(_WIN32) && !defined(__ANDROID__)

namespace {

void
set_env(const char *name, const char *value)
{
	if (value != nullptr) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
}

std::string
tmp_root()
{
	const char *t = std::getenv("TMPDIR");
	std::string root = (t != nullptr && t[0] != '\0') ? t : "/tmp";
	if (root.back() == '/') {
		root.pop_back();
	}
	root += "/tests_input_emulated_" + std::to_string((unsigned long)getpid());
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

//! HOME / XDG_DATA_HOME redirected to a throwaway per-user dir for one test.
struct Sandbox
{
	std::string root = tmp_root();
	std::string home = root + "/home";
	std::string user_dir;
	std::string saved_home;
	std::string saved_xdg;
	bool had_xdg = false;

	Sandbox()
	{
		const char *h = std::getenv("HOME");
		saved_home = h != nullptr ? h : "";
		const char *x = std::getenv("XDG_DATA_HOME");
		had_xdg = x != nullptr;
		saved_xdg = had_xdg ? x : "";
#ifdef __APPLE__
		user_dir = home + "/Library/Application Support/DisplayXR/DisplayProcessors";
#else
		user_dir = home + "/xdg/DisplayXR/DisplayProcessors";
		set_env("XDG_DATA_HOME", (home + "/xdg").c_str());
#endif
		mkdir_p(user_dir);
		set_env("HOME", home.c_str());
	}

	~Sandbox()
	{
		set_env("HOME", saved_home.c_str());
		set_env("XDG_DATA_HOME", had_xdg ? saved_xdg.c_str() : nullptr);
		std::remove((user_dir + "/emulated_controllers").c_str());
	}

	std::string
	gate_path() const
	{
		return user_dir + "/emulated_controllers";
	}
};

} // namespace

TEST_CASE("emulated controllers gate: the POSIX per-user file, live", "[input_emulated]")
{
	Sandbox sb;
	char dir[1024];
	REQUIRE(target_component_user_dir(dir, sizeof(dir)));
	REQUIRE(sb.user_dir == dir); // the loader reads the dir the sandbox redirected

	const char *source = nullptr;

	SECTION("absent: on, source default")
	{
		CHECK(target_input_plugin_get_emulated_controllers(&source));
		CHECK(std::string(source) == "default");
	}
	SECTION("'0': off, source file — what `input emulated off` writes")
	{
		write_file(sb.gate_path(), "0\n");
		CHECK_FALSE(target_input_plugin_get_emulated_controllers(&source));
		CHECK(std::string(source) == "file");
	}
	SECTION("'1': on, source file — what `input emulated on` writes")
	{
		write_file(sb.gate_path(), "1\n");
		CHECK(target_input_plugin_get_emulated_controllers(&source));
		CHECK(std::string(source) == "file");
	}
	SECTION("any other first byte is on (the force_qwerty convention), an empty file too")
	{
		write_file(sb.gate_path(), "off");
		CHECK(target_input_plugin_get_emulated_controllers(&source));
		CHECK(std::string(source) == "file");
		write_file(sb.gate_path(), "");
		CHECK(target_input_plugin_get_emulated_controllers(&source));
	}
	SECTION("removing the file restores the default")
	{
		write_file(sb.gate_path(), "0");
		REQUIRE_FALSE(target_input_plugin_get_emulated_controllers(nullptr));
		std::remove(sb.gate_path().c_str());
		CHECK(target_input_plugin_get_emulated_controllers(&source));
		CHECK(std::string(source) == "default");
	}
}

#endif
