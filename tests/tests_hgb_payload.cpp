// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-hypr-geometry-bridge (targets/hypr_geometry_bridge/
 *         hgb_payload.c): Hyprland's j/clients + j/monitors replies to the
 *         schema-1 org.displayxr.WindowGeometry1 snapshot the runtime reads.
 *
 * Fixtures in tests/data/hyprland/ have the shape of real `hyprctl -j`
 * output (Hyprland 0.4x-0.5x field names); they are not captured from a
 * live session (no Omarchy machine yet).
 */

#include "catch_amalgamated.hpp"

#include "hgb_payload.h"

#include <cjson/cJSON.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#ifndef HGB_FIXTURE_DIR
#error "HGB_FIXTURE_DIR must point at tests/data/hyprland"
#endif

namespace {

std::string
fixture(const char *name)
{
	std::ifstream f(std::string(HGB_FIXTURE_DIR) + "/" + name, std::ios::binary);
	REQUIRE(f.good());
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

struct Snap
{
	cJSON *root = nullptr;
	hgb_snapshot_info info{};
	std::string text;

	Snap(const std::string &clients, const std::string &monitors)
	{
		char *s = hgb_build_snapshot(clients.c_str(), monitors.c_str(), &info);
		if (s != nullptr) {
			text = s;
			hgb_free(s);
			root = cJSON_Parse(text.c_str());
		}
	}
	~Snap() { cJSON_Delete(root); }

	const cJSON *
	window(int pid) const
	{
		const cJSON *w = nullptr;
		cJSON_ArrayForEach(w, cJSON_GetObjectItemCaseSensitive(root, "windows"))
		{
			if ((int)cJSON_GetObjectItemCaseSensitive(w, "pid")->valuedouble == pid) {
				return w;
			}
		}
		return nullptr;
	}
};

double
at(const cJSON *obj, const char *key, int i)
{
	return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(obj, key), i)->valuedouble;
}

double
mon(const cJSON *win, const char *key)
{
	return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(win, "monitor"), key)->valuedouble;
}

bool
flag(const cJSON *win, const char *key)
{
	return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(win, key));
}

} // namespace

TEST_CASE("hgb: header is schema 1, logical")
{
	Snap s(fixture("clients_two.json"), fixture("monitors_two.json"));
	REQUIRE(s.root != nullptr);
	CHECK(cJSON_GetObjectItemCaseSensitive(s.root, "version")->valuedouble == 1.0);
	CHECK(std::string(cJSON_GetObjectItemCaseSensitive(s.root, "layout_mode")->valuestring) == "logical");
}

TEST_CASE("hgb: panel window at scale 1")
{
	Snap s(fixture("clients_two.json"), fixture("monitors_two.json"));
	const cJSON *w = s.window(4242);
	REQUIRE(w != nullptr);
	CHECK(at(w, "frame", 0) == 100);
	CHECK(at(w, "frame", 1) == 200);
	CHECK(at(w, "frame", 2) == 1280);
	CHECK(at(w, "frame", 3) == 720);
	CHECK(at(w, "buffer", 2) == 1280);
	CHECK(at(w, "buffer_device", 0) == 100);
	CHECK(at(w, "buffer_device", 1) == 200);
	CHECK(mon(w, "w") == 2560);
	CHECK(mon(w, "h") == 1600);
	CHECK(mon(w, "scale") == 1.0);
	CHECK(mon(w, "device_scale") == 1.0);
	CHECK(flag(w, "focus"));
	CHECK_FALSE(flag(w, "hidden"));
	CHECK(flag(w, "floating"));
	CHECK(std::string(cJSON_GetObjectItemCaseSensitive(w, "app_id")->valuestring) == "cube_hosted_vk");
	CHECK(s.info.any_floating_visible);
}

TEST_CASE("hgb: second monitor at fractional scale 1.5")
{
	Snap s(fixture("clients_two.json"), fixture("monitors_two.json"));
	const cJSON *w = s.window(555);
	REQUIRE(w != nullptr);
	// Logical monitor: 2880x1800 / 1.5.
	CHECK(mon(w, "x") == 2560);
	CHECK(mon(w, "w") == 1920);
	CHECK(mon(w, "h") == 1200);
	CHECK(mon(w, "device_scale") == 1.5);
	// Device px relative to the monitor: (2600-2560)*1.5, 40*1.5, 1800*1.5, 1100*1.5.
	CHECK(at(w, "buffer_device", 0) == 60);
	CHECK(at(w, "buffer_device", 1) == 60);
	CHECK(at(w, "buffer_device", 2) == 2700);
	CHECK(at(w, "buffer_device", 3) == 1650);
	CHECK_FALSE(flag(w, "focus"));
}

TEST_CASE("hgb: windows on workspaces nobody is looking at are hidden")
{
	Snap s(fixture("clients_two.json"), fixture("monitors_two.json"));
	const cJSON *other_ws = s.window(777);
	REQUIRE(other_ws != nullptr);
	CHECK(flag(other_ws, "hidden"));

	// The special workspace is not shown on DP-1 (specialWorkspace id 0).
	const cJSON *special = s.window(888);
	REQUIRE(special != nullptr);
	CHECK(flag(special, "hidden"));
	CHECK(flag(special, "xwayland"));
	CHECK(flag(special, "fullscreen")); // 0.42+ numeric fullscreen state
}

TEST_CASE("hgb: no pid, no window; no titles ever")
{
	Snap s(fixture("clients_two.json"), fixture("monitors_two.json"));
	CHECK(s.window(0) == nullptr);
	CHECK(s.info.window_count == 4);
	CHECK(s.text.find("title") == std::string::npos);
	CHECK(s.text.find("private") == std::string::npos);
}

TEST_CASE("hgb: quarter-turned panel transposes the logical size")
{
	Snap s(fixture("clients_rotated.json"), fixture("monitors_rotated.json"));
	const cJSON *w = s.window(31337);
	REQUIRE(w != nullptr);
	// 2560x1600 mode, transform 1 (90°), scale 1.25: 1280 x 2048 logical.
	CHECK(mon(w, "x") == -1280);
	CHECK(mon(w, "w") == 1280);
	CHECK(mon(w, "h") == 2048);
	// (-1200 - -1280)*1.25 = 100, 100*1.25 = 125, 1000*1.25, 800*1.25.
	CHECK(at(w, "buffer_device", 0) == 100);
	CHECK(at(w, "buffer_device", 1) == 125);
	CHECK(at(w, "buffer_device", 2) == 1250);
	CHECK(at(w, "buffer_device", 3) == 1000);
	CHECK_FALSE(s.info.any_floating_visible);
}

TEST_CASE("hgb: malformed replies are refused, empty ones are an empty snapshot")
{
	CHECK(hgb_build_snapshot("not json", "[]", nullptr) == nullptr);
	CHECK(hgb_build_snapshot("[]", "{\"id\":0}", nullptr) == nullptr);
	CHECK(hgb_build_snapshot(nullptr, "[]", nullptr) == nullptr);
	Snap s("[]", "[]");
	REQUIRE(s.root != nullptr);
	CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(s.root, "windows")) == 0);
}

TEST_CASE("hgb: a window on an unknown monitor is left out")
{
	Snap s(fixture("clients_two.json"), "[]");
	REQUIRE(s.root != nullptr);
	CHECK(s.info.window_count == 0);
}
