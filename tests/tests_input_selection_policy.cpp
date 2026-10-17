// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Per-client input-provider selection policy (ADR-034 Amendment 5,
 *        #2181): the per-family precedence, the per-app key, and the POSIX
 *        per-app pin store.
 *
 * Three halves:
 *  - `target_input_resolve_preference` is pure, so its whole matrix is pinned
 *    on every platform: Lock -> per-app pin -> session request ->
 *    PreferredPlugin -> ProbeOrder, the "only families the provider claims"
 *    fall-through, and DXR_INPUT_PROVIDERS=0 / ForceQwerty beating every pin;
 *  - `u_exe_basename_*`: the per-app key is the executable's basename (with
 *    extension), never truncated, never a path;
 *  - the POSIX `input-preferred-<exe>` pin, written / read / listed / cleared
 *    live with HOME redirected. The registry half needs HKCU and is covered on
 *    the Windows box.
 *
 * Host-only, no GPU, no plug-in.
 */

#include "target_component_loader.h"
#include "target_input_plugin_loader.h"

#include "util/u_exe_basename.h"
#include "util/u_status_snapshot.h"
#include "xrt/xrt_input_selection.h"

#include "catch_amalgamated.hpp"

#include <cjson/cJSON.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif


namespace {

struct Pref
{
	target_input_pref_inputs in{};
	char out[64] = {};

	target_input_pref_source
	resolve()
	{
		return target_input_resolve_preference(&in, out, sizeof(out));
	}
};

} // namespace


TEST_CASE("input selection: per-family precedence", "[input_selection]")
{
	Pref p;

	SECTION("nothing set: the default walk")
	{
		CHECK(p.resolve() == TARGET_INPUT_PREF_NONE);
		CHECK(std::string(p.out).empty());
	}
	SECTION("the user default alone")
	{
		p.in.user_default = "sim-input";
		p.in.user_default_claims = true;
		CHECK(p.resolve() == TARGET_INPUT_PREF_USER_DEFAULT);
		CHECK(std::string(p.out) == "sim-input");
	}
	SECTION("a session request beats the user default")
	{
		p.in.user_default = "a";
		p.in.user_default_claims = true;
		p.in.request = "b";
		p.in.request_claims = true;
		CHECK(p.resolve() == TARGET_INPUT_PREF_REQUEST);
		CHECK(std::string(p.out) == "b");
	}
	SECTION("the per-app pin beats the session request (the user overrides the app)")
	{
		p.in.app_pin = "pin";
		p.in.app_pin_claims = true;
		p.in.request = "req";
		p.in.request_claims = true;
		p.in.user_default = "def";
		p.in.user_default_claims = true;
		CHECK(p.resolve() == TARGET_INPUT_PREF_APP_PIN);
		CHECK(std::string(p.out) == "pin");
	}
	SECTION("a pin to a provider that does not claim the family falls through")
	{
		// A controllers-only provider pinned: hand tracking keeps the next level.
		p.in.app_pin = "controllers-only";
		p.in.app_pin_claims = false;
		p.in.request = "hands";
		p.in.request_claims = true;
		CHECK(p.resolve() == TARGET_INPUT_PREF_REQUEST);
		CHECK(std::string(p.out) == "hands");
		p.in.request_claims = false;
		CHECK(p.resolve() == TARGET_INPUT_PREF_NONE);
		CHECK(std::string(p.out).empty());
	}
	SECTION("Policy\\Lock hides the per-user pin and the app's request, not the machine default")
	{
		p.in.lock = true;
		p.in.app_pin = "pin";
		p.in.app_pin_claims = true;
		p.in.request = "req";
		p.in.request_claims = true;
		CHECK(p.resolve() == TARGET_INPUT_PREF_NONE);
		p.in.user_default = "machine";
		p.in.user_default_claims = true;
		CHECK(p.resolve() == TARGET_INPUT_PREF_USER_DEFAULT);
		CHECK(std::string(p.out) == "machine");
	}
	SECTION("DXR_INPUT_PROVIDERS=0 / ForceQwerty beat every pin")
	{
		p.in.providers_disabled = true;
		p.in.app_pin = "pin";
		p.in.app_pin_claims = true;
		p.in.user_default = "def";
		p.in.user_default_claims = true;
		CHECK(p.resolve() == TARGET_INPUT_PREF_NONE);
		CHECK(std::string(p.out).empty());
	}
	SECTION("empty strings are unset, and NULL input / output are total")
	{
		p.in.app_pin = "";
		p.in.app_pin_claims = true;
		CHECK(p.resolve() == TARGET_INPUT_PREF_NONE);
		CHECK(target_input_resolve_preference(nullptr, p.out, sizeof(p.out)) == TARGET_INPUT_PREF_NONE);
		p.in.request = "r";
		p.in.request_claims = true;
		CHECK(target_input_resolve_preference(&p.in, nullptr, 0) == TARGET_INPUT_PREF_REQUEST);
	}
	SECTION("source names are stable")
	{
		CHECK(std::string(target_input_pref_source_str(TARGET_INPUT_PREF_NONE)) == "none");
		CHECK(std::string(target_input_pref_source_str(TARGET_INPUT_PREF_APP_PIN)) == "app_pin");
		CHECK(std::string(target_input_pref_source_str(TARGET_INPUT_PREF_REQUEST)) == "request");
		CHECK(std::string(target_input_pref_source_str(TARGET_INPUT_PREF_USER_DEFAULT)) == "user_default");
	}
}

TEST_CASE("input selection: the per-app key is the executable basename", "[input_selection]")
{
	char out[XRT_INPUT_EXE_BASENAME_SIZE];

	SECTION("POSIX and Windows paths reduce to the file name, extension kept")
	{
		REQUIRE(u_exe_basename_from_path("/usr/bin/cube_handle_vk_linux", out, sizeof(out)));
		CHECK(std::string(out) == "cube_handle_vk_linux");
		REQUIRE(u_exe_basename_from_path("C:\\Games\\Sample\\SampleGame.exe", out, sizeof(out)));
		CHECK(std::string(out) == "SampleGame.exe");
		REQUIRE(u_exe_basename_from_path("C:/mixed\\sep/App.exe", out, sizeof(out)));
		CHECK(std::string(out) == "App.exe");
		REQUIRE(u_exe_basename_from_path("bare.exe", out, sizeof(out)));
		CHECK(std::string(out) == "bare.exe");
	}
	SECTION("UTF-8 basenames pass through byte for byte")
	{
		REQUIRE(u_exe_basename_from_path("/opt/\xC3\xA9tude/\xE6\xB8\xB8\xE6\x88\x8F.exe", out, sizeof(out)));
		CHECK(std::string(out) == "\xE6\xB8\xB8\xE6\x88\x8F.exe");
	}
	SECTION("no basename: refused and empty")
	{
		CHECK_FALSE(u_exe_basename_from_path(nullptr, out, sizeof(out)));
		CHECK(out[0] == '\0');
		CHECK_FALSE(u_exe_basename_from_path("", out, sizeof(out)));
		CHECK_FALSE(u_exe_basename_from_path("/usr/bin/", out, sizeof(out)));
		CHECK_FALSE(u_exe_basename_from_path("C:\\dir\\", out, sizeof(out)));
		CHECK_FALSE(u_exe_basename_from_path("/a/..", out, sizeof(out)));
		CHECK_FALSE(u_exe_basename_from_path(".", out, sizeof(out)));
	}
	SECTION("a basename that does not fit is refused, never truncated into another app's key")
	{
		std::string long_name(XRT_INPUT_EXE_BASENAME_SIZE, 'x');
		CHECK_FALSE(u_exe_basename_from_path(("/bin/" + long_name).c_str(), out, sizeof(out)));
		CHECK(out[0] == '\0');
		std::string fits(XRT_INPUT_EXE_BASENAME_SIZE - 1, 'y');
		CHECK(u_exe_basename_from_path(("/bin/" + fits).c_str(), out, sizeof(out)));
	}
	SECTION("control characters are not a key")
	{
		CHECK_FALSE(u_exe_basename_valid("bad\tname", 64));
		CHECK_FALSE(u_exe_basename_valid("bad\nname", 64));
		CHECK(u_exe_basename_valid("ok name.exe", 64));
	}
	SECTION("this process resolves its own executable")
	{
		REQUIRE(u_exe_basename_self(out, sizeof(out)));
		CHECK(std::string(out).find("tests_input_selection_policy") != std::string::npos);
	}
}

TEST_CASE("input selection: live holders as JSON (status snapshot / input list)", "[input_selection]")
{
	struct xrt_input_client_holders rows[2];
	std::memset(rows, 0, sizeof(rows));
	rows[0].key = 7;
	std::snprintf(rows[0].exe_basename, sizeof(rows[0].exe_basename), "%s", "A.exe");
	std::snprintf(rows[0].holder[XRT_INPUT_ROLE_FAMILY_CONTROLLERS], XRT_INPUT_PROVIDER_ID_SIZE, "%s", "ctl");
	std::snprintf(rows[0].preferred[XRT_INPUT_ROLE_FAMILY_CONTROLLERS], XRT_INPUT_PROVIDER_ID_SIZE, "%s", "ctl");
	std::snprintf(rows[0].holder[XRT_INPUT_ROLE_FAMILY_HAND_TRACKING], XRT_INPUT_PROVIDER_ID_SIZE, "%s", "hands");
	rows[0].generation = 5;
	rows[1].key = 8; // a client nobody holds anything for

	cJSON *root = cJSON_CreateObject();
	u_status_input_holders_add_cjson(root, rows, 2);
	cJSON *arr = cJSON_GetObjectItem(root, "input_holders");
	REQUIRE(cJSON_IsArray(arr));
	REQUIRE(cJSON_GetArraySize(arr) == 2);
	cJSON *a = cJSON_GetArrayItem(arr, 0);
	CHECK(cJSON_GetObjectItem(a, "client")->valuedouble == 7);
	CHECK(std::string(cJSON_GetObjectItem(a, "app")->valuestring) == "A.exe");
	CHECK(cJSON_GetObjectItem(a, "generation")->valuedouble == 5);
	cJSON *held = cJSON_GetObjectItem(a, "holders");
	CHECK(std::string(cJSON_GetObjectItem(held, "controllers")->valuestring) == "ctl");
	CHECK(std::string(cJSON_GetObjectItem(held, "hand_tracking")->valuestring) == "hands");
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(held, "gamepad")));
	cJSON *pref = cJSON_GetObjectItem(a, "preferred");
	CHECK(std::string(cJSON_GetObjectItem(pref, "controllers")->valuestring) == "ctl");
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(pref, "hand_tracking")));
	CHECK(cJSON_IsNull(
	    cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 1), "holders"), "controllers")));
	cJSON_Delete(root);
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

//! HOME / XDG_DATA_HOME redirected to a throwaway per-user dir for one test.
struct Sandbox
{
	std::string home;
	std::string user_dir;
	std::string saved_home;
	std::string saved_xdg;
	bool had_xdg = false;

	Sandbox()
	{
		const char *t = std::getenv("TMPDIR");
		std::string root = (t != nullptr && t[0] != '\0') ? t : "/tmp";
		if (root.back() == '/') {
			root.pop_back();
		}
		home = root + "/tests_input_selection_" + std::to_string((unsigned long)getpid()) + "/home";
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
		set_env("HOME", home.c_str());
	}

	~Sandbox()
	{
		std::remove((user_dir + "/input-preferred-App.exe").c_str());
		std::remove((user_dir + "/input-preferred-other").c_str());
		set_env("HOME", saved_home.c_str());
		set_env("XDG_DATA_HOME", had_xdg ? saved_xdg.c_str() : nullptr);
	}
};

} // namespace

TEST_CASE("input selection: the POSIX per-app pin file, live", "[input_selection]")
{
	Sandbox sb;
	char id[64];

	SECTION("absent: no pin")
	{
		CHECK_FALSE(target_input_plugin_get_app_pin("App.exe", id, sizeof(id)));
		CHECK(id[0] == '\0');
		struct target_input_app_pin pins[4];
		CHECK(target_input_plugin_list_app_pins(pins, 4) == 0);
	}
	SECTION("write creates the per-user dir, read and list return it, clear removes it")
	{
		REQUIRE(target_input_plugin_set_app_pin("App.exe", "sim-input") == TARGET_INPUT_PIN_WRITE_OK);
		REQUIRE(target_input_plugin_get_app_pin("App.exe", id, sizeof(id)));
		CHECK(std::string(id) == "sim-input");

		struct stat st;
		CHECK(stat((sb.user_dir + "/input-preferred-App.exe").c_str(), &st) == 0);

		REQUIRE(target_input_plugin_set_app_pin("other", XRT_INPUT_PROVIDER_ID_EMULATED) ==
		        TARGET_INPUT_PIN_WRITE_OK);
		struct target_input_app_pin pins[8];
		const int n = target_input_plugin_list_app_pins(pins, 8);
		REQUIRE(n == 2);
		bool saw_app = false, saw_other = false;
		for (int i = 0; i < n; i++) {
			saw_app |= std::string(pins[i].exe_basename) == "App.exe" &&
			           std::string(pins[i].provider_id) == "sim-input";
			saw_other |= std::string(pins[i].exe_basename) == "other" &&
			             std::string(pins[i].provider_id) == XRT_INPUT_PROVIDER_ID_EMULATED;
		}
		CHECK(saw_app);
		CHECK(saw_other);

		// The other app's pin is its own: per-app means per-app.
		REQUIRE(target_input_plugin_get_app_pin("other", id, sizeof(id)));
		CHECK(std::string(id) == XRT_INPUT_PROVIDER_ID_EMULATED);

		REQUIRE(target_input_plugin_set_app_pin("App.exe", nullptr) == TARGET_INPUT_PIN_WRITE_OK);
		CHECK_FALSE(target_input_plugin_get_app_pin("App.exe", id, sizeof(id)));
		CHECK(target_input_plugin_list_app_pins(pins, 8) == 1);
		// Clearing what is not there is fine.
		CHECK(target_input_plugin_set_app_pin("App.exe", "") == TARGET_INPUT_PIN_WRITE_OK);
	}
	SECTION("a hand-edited file is trimmed, like input-preferred")
	{
		mkdir_p(sb.user_dir);
		FILE *f = std::fopen((sb.user_dir + "/input-preferred-App.exe").c_str(), "wb");
		REQUIRE(f != nullptr);
		std::fputs("  sim-input \r\n", f);
		std::fclose(f);
		// Leading spaces are part of the id (input-preferred's rule trims the tail only).
		REQUIRE(target_input_plugin_get_app_pin("App.exe", id, sizeof(id)));
		CHECK(std::string(id) == "  sim-input");
	}
	SECTION("invalid keys and ids are refused with nothing written")
	{
		CHECK(target_input_plugin_set_app_pin("../escape", "sim-input") == TARGET_INPUT_PIN_WRITE_INVALID);
		CHECK(target_input_plugin_set_app_pin("", "sim-input") == TARGET_INPUT_PIN_WRITE_INVALID);
		CHECK(target_input_plugin_set_app_pin("App.exe", "bad/id") == TARGET_INPUT_PIN_WRITE_INVALID);
		CHECK_FALSE(target_input_plugin_get_app_pin("../escape", id, sizeof(id)));
		struct target_input_app_pin pins[4];
		CHECK(target_input_plugin_list_app_pins(pins, 4) == 0);
	}
}

#endif // POSIX
