// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "util/u_camera_profile.h"
#include "os/os_time.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <cstring>
#include <cmath>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using Catch::Approx;

TEST_CASE("Camera profiles use camera-rig units and preserve defaults", "[camera-profile]")
{
	u_camera_profile profile{};
	REQUIRE(u_camera_profile_parse("{}", 0, &profile, nullptr, 0));
	CHECK(profile.ipd_factor == 1);
	CHECK(profile.parallax_factor == 1);
	CHECK(profile.inv_convergence_distance == .5f);
	CHECK(profile.half_tan_vfov == .3249f);
	CHECK(profile.m2v == 1);
	REQUIRE(u_camera_profile_parse(
	    R"({"ipdFactor":1.5,"parallaxFactor":0.2,"convergenceDiopters":0,"verticalFov":1.2,"metersToVirtual":2})",
	    0, &profile, nullptr, 0));
	CHECK(profile.ipd_factor == 1.5f);
	CHECK(profile.parallax_factor == .2f);
	CHECK(profile.inv_convergence_distance == 0);
	CHECK(profile.half_tan_vfov == Approx(.6841368083));
	CHECK(profile.m2v == 2);
}

TEST_CASE("Profile aliases resolve against the canvas aspect and fixed reference IPD", "[camera-profile]")
{
	const char *json = R"({"horizontalFovDeg":100,"renderedBaselineMeters":0.036815625,"convergenceMeters":0.8})";
	u_camera_profile widescreen{}, tall{};
	REQUIRE(u_camera_profile_parse(json, 16.f / 9.f, &widescreen, nullptr, 0));
	REQUIRE(u_camera_profile_parse(json, 16.f / 10.f, &tall, nullptr, 0));
	CHECK(widescreen.half_tan_vfov == Approx(.6703613958));
	CHECK(tall.half_tan_vfov == Approx(.7448459954));
	CHECK(widescreen.m2v == Approx(.584375));
	CHECK(widescreen.inv_convergence_distance == 1.25f);
	CHECK_FALSE(u_camera_profile_parse(json, 0, &widescreen, nullptr, 0));
}

TEST_CASE("Malformed or ambiguous profiles never partly apply", "[camera-profile]")
{
	for (const char *json :
	     {R"({"ipdFactor\uZZZZjunk":2})", R"({"ipdFactor\u000gjunk":2})", R"({"ipdFactor\u0x00junk":2})",
	      R"({"ipdFactor\u00":2})", R"({"horizontalFovDeg":0})", R"({"horizontalFovDeg":180})"}) {
		CAPTURE(json);
		u_camera_profile profile{7, 8, 9, 10, 11}, before = profile;
		CHECK_FALSE(u_camera_profile_parse(json, 1, &profile, nullptr, 0));
		CHECK(std::memcmp(&before, &profile, sizeof(profile)) == 0);
	}
	for (const char *json : {R"({"ipdFactor":01})", R"({"ipdFactor":1.})", R"({"ipdFactor":-.5})",
	                         R"({"ipdFactor\u0000junk":2})", "{\"ipdFactor\":\v2}"}) {
		CAPTURE(json);
		u_camera_profile profile{7, 8, 9, 10, 11}, before = profile;
		CHECK_FALSE(u_camera_profile_parse(json, 1, &profile, nullptr, 0));
		CHECK(std::memcmp(&before, &profile, sizeof(profile)) == 0);
	}
	for (const char *json :
	     {"[]", "null", "{} trailing", R"({"pose":{}})", R"({"ipdFactor":"1"})", R"({"ipdFactor":1e999})",
	      R"({"ipdFactor":1,"ipdFactor":2})", R"({"verticalFov":1,"horizontalFovDeg":90})",
	      R"({"convergenceMeters":0})", R"({"renderedBaselineMeters":-1})",
	      R"({"metersToVirtual":1,"renderedBaselineMeters":0.063})", R"({"parallaxFactor":0.2,"unknown":1})"}) {
		CAPTURE(json);
		u_camera_profile profile{7, 8, 9, 10, 11}, before = profile;
		char reason[160]{};
		CHECK_FALSE(u_camera_profile_parse(json, 1, &profile, reason, sizeof(reason)));
		CHECK(std::memcmp(&before, &profile, sizeof(profile)) == 0);
		CHECK(reason[0] != '\0');
	}
}

TEST_CASE("Legal JSON number and key escapes remain supported", "[camera-profile]")
{
	u_camera_profile profile{};
	REQUIRE(u_camera_profile_parse(R"({"ipd\u0046actor":1e+1,"parallaxFactor":0.5,"convergenceDiopters":-0})", 1,
	                               &profile, nullptr, 0));
	CHECK(profile.ipd_factor == 10);
	CHECK(profile.parallax_factor == .5f);
	CHECK(profile.inv_convergence_distance == 0);
}

TEST_CASE("Wide horizontal FOV aliases retain their canonical vertical equivalent", "[camera-profile]")
{
	u_camera_profile profile{};
	REQUIRE(u_camera_profile_parse(R"({"horizontalFovDeg":179.5})", 16.f / 9.f, &profile, nullptr, 0));
	CHECK(2.0 * std::atan(profile.half_tan_vfov) == Approx(3.126079).margin(0.000001));
}

TEST_CASE("Camera descriptor scalar bounds match the chained-rig convention", "[camera-profile]")
{
	u_camera_profile profile{};
	REQUIRE(u_camera_profile_parse(
	    R"({"ipdFactor":-1,"parallaxFactor":1e99,"convergenceDiopters":1e99,"verticalFov":-1,"metersToVirtual":0})",
	    1, &profile, nullptr, 0));
	CHECK(profile.ipd_factor == 0);
	CHECK(profile.parallax_factor == 10000);
	CHECK(profile.inv_convergence_distance == 20);
	CHECK(profile.half_tan_vfov == Approx(.005000041667));
	CHECK(profile.m2v == 1);
}

namespace {
std::string
utf8(const std::filesystem::path &path)
{
	const auto bytes = path.u8string();
	return {bytes.begin(), bytes.end()};
}
struct ProfileFiles
{
	std::filesystem::path dir = std::filesystem::temp_directory_path() /
	                            ("displayxr-camera-profile-" + std::to_string(os_monotonic_get_ns()));
	ProfileFiles()
	{
		REQUIRE(std::filesystem::create_directory(dir));
	}
	~ProfileFiles()
	{
		// This exclusively-created temporary directory contains only this test's files.
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}
	std::string
	write(const std::string &name, const std::string &data)
	{
		auto path = dir / std::filesystem::u8path(name);
		std::ofstream file(path, std::ios::binary);
		file.write(data.data(), data.size());
		file.close();
		REQUIRE(file.good());
		return utf8(path);
	}
};
} // namespace

TEST_CASE("Per-title profile lookup and developer overrides have explicit precedence", "[camera-profile]")
{
	ProfileFiles files;
	files.write("Sample Title.exe.json", R"({"metersToVirtual":2})");
	const auto override = files.write("override.json", R"({"metersToVirtual":3})");
	u_camera_profile profile{};
	const auto dir = utf8(files.dir);
	REQUIRE(u_camera_profile_load("Sample Title.exe", dir.c_str(), nullptr, 1, &profile, nullptr, 0) ==
	        U_CAMERA_PROFILE_LOADED);
	CHECK(profile.m2v == 2);
	REQUIRE(u_camera_profile_load("Sample Title.exe", dir.c_str(), override.c_str(), 1, &profile, nullptr, 0) ==
	        U_CAMERA_PROFILE_LOADED);
	CHECK(profile.m2v == 3);
	REQUIRE(u_camera_profile_load("Sample Title.exe", dir.c_str(), " \n{\"metersToVirtual\":4}", 1, &profile,
	                              nullptr, 0) == U_CAMERA_PROFILE_LOADED);
	CHECK(profile.m2v == 4);
	CHECK(u_camera_profile_load("Other.exe", dir.c_str(), nullptr, 1, &profile, nullptr, 0) ==
	      U_CAMERA_PROFILE_NONE);
	CHECK(profile.m2v == 4);
	CHECK(u_camera_profile_load("Sample Title.exe", dir.c_str(), "{bad}", 1, &profile, nullptr, 0) ==
	      U_CAMERA_PROFILE_INVALID);
	CHECK(profile.m2v == 4); // Invalid override does not fall through to per-title data.
	CHECK(u_camera_profile_load("../Sample Title.exe", dir.c_str(), nullptr, 1, &profile, nullptr, 0) ==
	      U_CAMERA_PROFILE_INVALID);
}

TEST_CASE("Profile files are bounded and support UTF-8 paths", "[camera-profile]")
{
	ProfileFiles files;
	u_camera_profile profile{};
	const auto unicode = files.write("camera-\xc3\xa9.json", R"({"ipdFactor":0.4})");
	CHECK(u_camera_profile_load(nullptr, nullptr, unicode.c_str(), 1, &profile, nullptr, 0) ==
	      U_CAMERA_PROFILE_LOADED);
	CHECK(profile.ipd_factor == .4f);
	const auto oversized = files.write("large.json", "{}" + std::string(65536, ' '));
	CHECK(u_camera_profile_load(nullptr, nullptr, oversized.c_str(), 1, &profile, nullptr, 0) ==
	      U_CAMERA_PROFILE_INVALID);
	const auto nul = files.write("nul.json", std::string("{}\0{}", 5));
	CHECK(u_camera_profile_load(nullptr, nullptr, nul.c_str(), 1, &profile, nullptr, 0) ==
	      U_CAMERA_PROFILE_INVALID);
	CHECK(profile.ipd_factor == .4f);
}

#ifdef _WIN32
namespace {
struct Environment
{
	std::wstring name, previous;
	bool existed;
	Environment(const wchar_t *key, const wchar_t *value) : name(key)
	{
		SetLastError(ERROR_SUCCESS);
		DWORD count = GetEnvironmentVariableW(key, nullptr, 0);
		existed = count != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
		if (count) {
			previous.resize(count);
			DWORD copied = GetEnvironmentVariableW(key, previous.data(), count);
			REQUIRE(copied < count);
			previous.resize(copied);
		}
		REQUIRE(SetEnvironmentVariableW(key, value));
	}
	~Environment()
	{
		SetEnvironmentVariableW(name.c_str(), existed ? previous.c_str() : nullptr);
	}
};
} // namespace

TEST_CASE("Process lookup uses ProgramData and the actual executable basename", "[camera-profile]")
{
	ProfileFiles files;
	Environment root(L"ProgramData", files.dir.c_str());
	Environment override(L"DXR_LEGACY_CAMERA_RIG", L"");
	wchar_t image[8192];
	DWORD length = GetModuleFileNameW(nullptr, image, 8192);
	REQUIRE(length > 0);
	REQUIRE(length < 8192);
	const auto basename = utf8(std::filesystem::path(image).filename());
	REQUIRE(std::filesystem::create_directories(files.dir / "DisplayXR" / "app-profiles"));
	files.write("DisplayXR/app-profiles/" + basename + ".json", R"({"metersToVirtual":0.75})");
	u_camera_profile profile{};
	REQUIRE(u_camera_profile_load_for_process(1, &profile, nullptr, 0) == U_CAMERA_PROFILE_LOADED);
	CHECK(profile.m2v == .75f);
	REQUIRE(SetEnvironmentVariableW(L"DXR_LEGACY_CAMERA_RIG", L"{\"metersToVirtual\":0.25}"));
	REQUIRE(u_camera_profile_load_for_process(1, &profile, nullptr, 0) == U_CAMERA_PROFILE_LOADED);
	CHECK(profile.m2v == .25f); // Reads live process environment, not a stale CRT cache.
}
#endif
