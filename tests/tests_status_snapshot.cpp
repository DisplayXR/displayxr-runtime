// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-051 display status snapshot: the §4 warning rules, the §3 JSON
 *         shape and the §7 text formatter, over synthetic snapshots.
 *
 * Pure: no instance, no loader, no service (util/u_status_snapshot.h).
 */

#include "catch_amalgamated.hpp"

#include "util/u_openxr_runtime.h"
#include "util/u_screen_info.h"
#include "util/u_status_snapshot.h"
#include "xrt/xrt_display_status.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"

#include <cjson/cJSON.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

// Crosses IPC by value in phase 2: no pointers, no constructors.
static_assert(std::is_trivially_copyable<xrt_status_snapshot>::value, "snapshot must be trivially copyable");
static_assert(std::is_standard_layout<xrt_status_snapshot>::value, "snapshot must be standard layout");
static_assert(std::is_trivially_copyable<xrt_status_screen>::value, "screen must be trivially copyable");
static_assert(std::is_trivially_copyable<xrt_status_client>::value, "client must be trivially copyable");

namespace {

constexpr uint64_t kScreenA = 0x8c413a2f61152ce7ull;
constexpr uint64_t kScreenB = 0xb72ea4c616544d01ull;

void
set(char *dst, size_t cap, const char *src)
{
	std::snprintf(dst, cap, "%s", src);
}
#define SET(field, str) set(field, sizeof(field), str)

void
add_plugin(xrt_status_snapshot &s, const char *id, uint32_t state, bool fallback, bool active, const char *load)
{
	xrt_status_plugin &p = s.plugins[s.plugin_count++];
	SET(p.id, id);
	SET(p.name, id);
	SET(p.load, load);
	p.platform_state = state;
	p.fallback = fallback;
	p.active = active;
	p.probe_order = fallback ? 200 : 50;
}

void
add_screen(xrt_status_snapshot &s, uint64_t id, const char *plugin, uint32_t confidence)
{
	xrt_status_screen &sc = s.screens[s.screen_count];
	sc.id = id;
	sc.index = s.screen_count++;
	SET(sc.device_name, sc.index == 0 ? "\\\\.\\DISPLAY1" : "\\\\.\\DISPLAY5");
	SET(sc.friendly_name, "AUO B194");
	SET(sc.key, sc.index == 0 ? "AUO-B194-00000000@DISPLAY1" : "AUO-B194-00000000@DISPLAY5");
	SET(sc.edid.manufacturer, "AUO");
	SET(sc.edid.product, "B194");
	sc.desktop = {sc.index == 0 ? 0 : 3840, 0, 3840, 2160, 2.5f};
	sc.native = {3840, 2160, 60000, true, XRT_STATUS_NATIVE_BASIS_DESKTOP, 0, 0};
	sc.physical = {344, 194, XRT_STATUS_MM_SOURCE_PLUGIN};
	sc.roles.os_main = sc.index == 0;
	sc.roles.runtime_default = sc.index == 0;
	SET(sc.claim.plugin_id, plugin);
	sc.claim.confidence = confidence;
	SET(sc.claim.serial, "QALA2137AL0011");
	sc.claim.apply = sc.index == 0 ? XRT_STATUS_APPLY_NEXT_SESSION : XRT_STATUS_APPLY_LIVE;
	sc.claim.apis =
	    XRT_STATUS_API_BIT_D3D11 | XRT_STATUS_API_BIT_D3D12 | XRT_STATUS_API_BIT_VK | XRT_STATUS_API_BIT_GL;
	sc.layout = {0.3442f, 0.1936f, 0.0f, 0.1f, 0.6f, XRT_SCREEN_INFO_SOURCE_SYSTEM};
	sc.eye_tracking.supported = 1;
	sc.eye_tracking.state = XRT_STATUS_TRACKING_TRACKING;
	sc.dp_count = 1;
	sc.dps[0] = {3, XRT_STATUS_DP_API_D3D11, XRT_STATUS_DP_KIND_PRIMARY, XRT_STATUS_DP_BACKEND_OK};
}

//! A healthy two-panel service snapshot: no rule fires.
std::unique_ptr<xrt_status_snapshot>
healthy()
{
	auto s = std::make_unique<xrt_status_snapshot>();
	std::memset(s.get(), 0, sizeof(*s));
	s->schema = XRT_STATUS_SCHEMA;
	s->source = XRT_STATUS_SOURCE_SERVICE;
	s->generation = {17, 2412};
	SET(s->runtime.version, "2.32.0");
	SET(s->runtime.git_tag, "v2.32.0");
	s->runtime.plugin_abi = 5;
	add_plugin(*s, "leia-sr", XRT_PLUGIN_PLATFORM_STATE_READY, false, true, "ACTIVE");
	add_plugin(*s, "sim-display", XRT_PLUGIN_PLATFORM_STATE_UNKNOWN, true, false, "NOT_ATTEMPTED");
	add_screen(*s, kScreenA, "leia-sr", XRT_DISPLAY_CLAIM_VERIFIED);
	add_screen(*s, kScreenB, "leia-sr", XRT_DISPLAY_CLAIM_VERIFIED);
	return s;
}

std::vector<std::string>
codes(const xrt_status_warning *arr, uint32_t n)
{
	std::vector<std::string> v;
	for (uint32_t i = 0; i < n; i++) {
		v.emplace_back(arr[i].code);
	}
	return v;
}

std::vector<std::string>
screen_codes(const xrt_status_snapshot &s, uint32_t i)
{
	return codes(s.screens[i].warnings, s.screens[i].warning_count);
}

std::vector<std::string>
sys_codes(const xrt_status_snapshot &s)
{
	return codes(s.warnings, s.warning_count);
}

const xrt_status_warning *
find(const xrt_status_warning *arr, uint32_t n, const char *code)
{
	for (uint32_t i = 0; i < n; i++) {
		if (std::strcmp(arr[i].code, code) == 0) {
			return &arr[i];
		}
	}
	return nullptr;
}

using V = std::vector<std::string>;

//! Owns the snapshot's JSON (the full helper set lives with the JSON tests).
struct json_doc_light
{
	cJSON *root;
	explicit json_doc_light(const xrt_status_snapshot &s) : root(u_status_snapshot_to_cjson(&s)) {}
	~json_doc_light()
	{
		cJSON_Delete(root);
	}
};

} // namespace

TEST_CASE("status: size report", "[status]")
{
	// Phase 2 crosses this over IPC; well above the message budget, so in pieces.
	WARN("sizeof(xrt_status_snapshot) = " << sizeof(xrt_status_snapshot) << " (screen " << sizeof(xrt_status_screen)
	                                      << ", client " << sizeof(xrt_status_client) << ", plugin "
	                                      << sizeof(xrt_status_plugin) << ")");
	CHECK(sizeof(xrt_status_snapshot) > 0);
}

TEST_CASE("status: a healthy service snapshot raises nothing", "[status][warnings]")
{
	auto s = healthy();
	u_status_warnings_derive(s.get());
	CHECK(sys_codes(*s).empty());
	CHECK(screen_codes(*s, 0).empty());
	CHECK(screen_codes(*s, 1).empty());

	// Idempotent.
	u_status_warnings_derive(s.get());
	CHECK(sys_codes(*s).empty());
	CHECK(screen_codes(*s, 0).empty());
}

TEST_CASE("status: SERVICE_HEADLESS is a system-level info for headless only", "[status][warnings]")
{
	auto s = healthy();
	s->source = XRT_STATUS_SOURCE_HEADLESS;
	u_status_warnings_derive(s.get());
	REQUIRE(sys_codes(*s) == V{"SERVICE_HEADLESS"});
	CHECK(s->warnings[0].level == XRT_STATUS_LEVEL_INFO);
	CHECK(screen_codes(*s, 0).empty());
}

TEST_CASE("status: NOT_NATIVE fires only on a known, different native mode of a claimed screen", "[status][warnings]")
{
	auto s = healthy();
	s->screens[1].desktop.width = 1920;
	s->screens[1].desktop.height = 1080;
	u_status_warnings_derive(s.get());
	CHECK(screen_codes(*s, 0).empty());
	REQUIRE(screen_codes(*s, 1) == V{"NOT_NATIVE"});
	CHECK(s->screens[1].warnings[0].level == XRT_STATUS_LEVEL_CRITICAL);

	SECTION("unknown native never warns")
	{
		s->screens[1].native.width = 0;
		s->screens[1].native.height = 0;
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 1).empty());
	}
	SECTION("unclaimed never warns")
	{
		s->screens[1].claim.plugin_id[0] = '\0';
		s->screens[1].claim.confidence = 0;
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 1).empty());
	}
}

TEST_CASE("status: compositor native rule — scaled GNOME Wayland, current == preferred", "[status][warnings][native]")
{
	// ds1-linux: eDP-1 at 167 %. Mutter: current 2880x1800 == preferred; the
	// desktop rect is XWayland's logical 3456x2160.
	auto s = healthy();
	s->screens[0].desktop = {0, 0, 3456, 2160, 1.6667f};
	u_status_native_resolve(&s->screens[0].native, 2880, 1800, 2880, 1800, 3456, 2160);
	CHECK(s->screens[0].native.is_native);
	CHECK(s->screens[0].native.basis == XRT_STATUS_NATIVE_BASIS_COMPOSITOR);
	CHECK(s->screens[0].native.width == 2880);
	CHECK(s->screens[0].native.height == 1800);
	u_status_warnings_derive(s.get());
	CHECK(screen_codes(*s, 0).empty());
}

TEST_CASE("status: compositor native rule — current mode is not the preferred one", "[status][warnings][native]")
{
	auto s = healthy();
	s->screens[0].desktop = {0, 0, 2304, 1440, 1.0f};
	u_status_native_resolve(&s->screens[0].native, 2304, 1440, 2880, 1800, 2304, 1440);
	CHECK_FALSE(s->screens[0].native.is_native);
	CHECK(s->screens[0].native.width == 2880); // native = the preferred mode
	u_status_warnings_derive(s.get());
	REQUIRE(screen_codes(*s, 0) == V{"NOT_NATIVE"});
	CHECK(std::string(s->screens[0].warnings[0].text).find("native 2880x1800 (now 2304x1440)") !=
	      std::string::npos);
	CHECK(s->screens[0].warnings[0].level == XRT_STATUS_LEVEL_CRITICAL);

	SECTION("even when the logical rect happens to equal the preferred mode")
	{
		s->screens[0].desktop.width = 2880;
		s->screens[0].desktop.height = 1800;
		u_status_native_resolve(&s->screens[0].native, 2304, 1440, 2880, 1800, 2880, 1800);
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 0) == V{"NOT_NATIVE"});
	}
}

TEST_CASE("status: no compositor info keeps the desktop-rect rule", "[status][warnings][native]")
{
	auto s = healthy();
	u_status_native_resolve(&s->screens[0].native, 3840, 2160, 0, 0, 3840, 2160);
	CHECK(s->screens[0].native.is_native);
	CHECK(s->screens[0].native.basis == XRT_STATUS_NATIVE_BASIS_DESKTOP);
	u_status_warnings_derive(s.get());
	CHECK(screen_codes(*s, 0).empty());

	// The same scaled rect with no compositor facts: the old rule fires.
	s->screens[0].desktop = {0, 0, 3456, 2160, 0.0f};
	u_status_native_resolve(&s->screens[0].native, 2880, 1800, 0, 0, 3456, 2160);
	CHECK_FALSE(s->screens[0].native.is_native);
	u_status_warnings_derive(s.get());
	REQUIRE(screen_codes(*s, 0) == V{"NOT_NATIVE"});
	CHECK(std::string(s->screens[0].warnings[0].text).find("native 2880x1800 (now 3456x2160)") !=
	      std::string::npos);
}

TEST_CASE("status: macOS backing rule — Retina default mode is native", "[status][warnings][native]")
{
	// 14" MacBook Pro default: looks like 1512x982 points @2x = 3024x1964
	// backing on a 3024x1964 panel. The desktop rect is in points.
	auto s = healthy();
	s->screens[0].desktop = {0, 0, 1512, 982, 2.0f};
	u_status_native_resolve_backing(&s->screens[0].native, 3024, 1964, 3024, 1964);
	CHECK(s->screens[0].native.is_native);
	CHECK(s->screens[0].native.basis == XRT_STATUS_NATIVE_BASIS_BACKING);
	CHECK(s->screens[0].native.width == 3024);
	CHECK(s->screens[0].native.height == 1964);
	CHECK(s->screens[0].native.current_width == 3024);
	u_status_warnings_derive(s.get());
	CHECK(screen_codes(*s, 0).empty());
}

TEST_CASE("status: macOS backing rule — More Space scaled mode warns", "[status][warnings][native]")
{
	// looks like 1800x1169 @2x = 3600x2338 backing, downsampled to the panel.
	auto s = healthy();
	s->screens[0].desktop = {0, 0, 1800, 1169, 2.0f};
	u_status_native_resolve_backing(&s->screens[0].native, 3600, 2338, 3024, 1964);
	CHECK_FALSE(s->screens[0].native.is_native);
	u_status_warnings_derive(s.get());
	REQUIRE(screen_codes(*s, 0) == V{"NOT_NATIVE"});
	const std::string t = s->screens[0].warnings[0].text;
	CHECK(t ==
	      "macOS 'Displays' is set to a scaled resolution (rendered 3600x2338, downsampled to the "
	      "3024x1964 panel): choose Default.");
	CHECK(t.find("Set this screen") == std::string::npos); // never "type a pixel size"
	CHECK(s->screens[0].warnings[0].level == XRT_STATUS_LEVEL_CRITICAL);
}

TEST_CASE("status: macOS backing rule — Larger Text scaled mode warns", "[status][warnings][native]")
{
	// looks like 1280x831 @2x = 2560x1662 backing, upsampled to the panel.
	auto s = healthy();
	s->screens[0].desktop = {0, 0, 1280, 831, 2.0f};
	u_status_native_resolve_backing(&s->screens[0].native, 2560, 1662, 3024, 1964);
	CHECK_FALSE(s->screens[0].native.is_native);
	u_status_warnings_derive(s.get());
	REQUIRE(screen_codes(*s, 0) == V{"NOT_NATIVE"});
	CHECK(std::string(s->screens[0].warnings[0].text)
	          .find("(rendered 2560x1662, upsampled to the 3024x1964 panel)") != std::string::npos);
}

TEST_CASE("status: macOS backing rule — unknown panel grid never warns", "[status][warnings][native]")
{
	auto s = healthy();
	s->screens[0].desktop = {0, 0, 1512, 982, 2.0f};
	u_status_native_resolve_backing(&s->screens[0].native, 3024, 1964, 0, 0);
	CHECK(s->screens[0].native.width == 0);
	CHECK(s->screens[0].native.current_width == 3024);
	u_status_warnings_derive(s.get());
	CHECK(screen_codes(*s, 0).empty());

	// No mode at all either.
	u_status_native_resolve_backing(&s->screens[0].native, 0, 0, 0, 0);
	u_status_warnings_derive(s.get());
	CHECK(screen_codes(*s, 0).empty());
}

TEST_CASE("status: CLAIM_FALLBACK needs an unhealthy vendor plug-in", "[status][warnings]")
{
	auto s = healthy();
	SET(s->screens[1].claim.plugin_id, "sim-display");
	s->screens[1].claim.confidence = XRT_DISPLAY_CLAIM_FALLBACK;

	SECTION("vendor healthy: a plain monitor on the fallback is normal")
	{
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 1).empty());
	}
	SECTION("vendor failed to load")
	{
		SET(s->plugins[0].load, "ABI_MISMATCH");
		s->plugins[0].active = false;
		u_status_warnings_derive(s.get());
		REQUIRE(screen_codes(*s, 1) == V{"CLAIM_FALLBACK"});
		CHECK(s->screens[1].warnings[0].level == XRT_STATUS_LEVEL_WARN);
	}
	SECTION("vendor platform not ready")
	{
		s->plugins[0].platform_state = XRT_PLUGIN_PLATFORM_STATE_PLATFORM_NOT_RUNNING;
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 1) == V{"CLAIM_FALLBACK"});
		// The vendor's own screen carries PLUGIN_NOT_READY, not CLAIM_FALLBACK.
		CHECK(screen_codes(*s, 0) == V{"PLUGIN_NOT_READY"});
	}
}

TEST_CASE("status: CLAIM_EDID_ONLY is info on an EDID-confidence claim", "[status][warnings]")
{
	auto s = healthy();
	s->screens[0].claim.confidence = XRT_DISPLAY_CLAIM_EDID;
	u_status_warnings_derive(s.get());
	REQUIRE(screen_codes(*s, 0) == V{"CLAIM_EDID_ONLY"});
	CHECK(s->screens[0].warnings[0].level == XRT_STATUS_LEVEL_INFO);
	CHECK(screen_codes(*s, 1).empty());
}

TEST_CASE("status: CLAIM_FORCED is info on a screen a per-screen preference forced", "[status][warnings]")
{
	auto s = healthy();
	// The second panel forced to the fallback by the user's preference.
	SET(s->screens[1].claim.plugin_id, "sim-display");
	s->screens[1].claim.confidence = XRT_DISPLAY_CLAIM_FALLBACK;
	s->screens[1].claim.forced = true;
	SET(s->screens[1].claim.preferred_plugin, "sim-display");
	s->screens[1].claim.preferred_source = XRT_STATUS_PREF_SOURCE_USER;
	u_status_warnings_derive(s.get());
	REQUIRE(screen_codes(*s, 1) == V{"CLAIM_FORCED"});
	CHECK(s->screens[1].warnings[0].level == XRT_STATUS_LEVEL_INFO);
	CHECK(std::string(s->screens[1].warnings[0].text) == "Plug-in forced by a per-screen preference (user)");
	CHECK(screen_codes(*s, 0).empty());

	json_doc_light d(*s);
	const cJSON *claim = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(d.root, "screens"), 1), "claim");
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(claim, "forced")));
	CHECK(std::string(cJSON_GetObjectItem(claim, "preferred_plugin")->valuestring) == "sim-display");
	CHECK(std::string(cJSON_GetObjectItem(claim, "preferred_source")->valuestring) == "user");
	CHECK(std::string(cJSON_GetObjectItem(claim, "apply")->valuestring) == "live");

	SECTION("a preference that was ignored is reported, but nothing is forced")
	{
		SET(s->screens[0].claim.preferred_plugin, "other-vendor");
		s->screens[0].claim.preferred_source = XRT_STATUS_PREF_SOURCE_ENV;
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 0).empty());
	}
}

TEST_CASE("status: NO_PHYSICAL_SIZE is the 0 m trap", "[status][warnings]")
{
	auto s = healthy();
	s->screens[1].physical = {0, 0, XRT_STATUS_MM_SOURCE_NONE};
	s->screens[1].layout.width_m = 0.0f;
	s->screens[1].layout.height_m = 0.0f;
	u_status_warnings_derive(s.get());
	REQUIRE(screen_codes(*s, 1) == V{"NO_PHYSICAL_SIZE"});
	CHECK(s->screens[1].warnings[0].level == XRT_STATUS_LEVEL_WARN);

	SECTION("EDID mm is enough")
	{
		s->screens[1].physical = {340, 190, XRT_STATUS_MM_SOURCE_EDID};
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 1).empty());
	}
}

TEST_CASE("status: a FALLBACK claim's plug-in mm give way to the EDID's", "[status][physical]")
{
	xrt_status_physical p{};

	SECTION("FALLBACK + EDID: the EDID mm")
	{
		u_status_physical_pick(XRT_DISPLAY_CLAIM_FALLBACK, 0.344f, 0.194f, 300, 190, &p);
		CHECK(p.width_mm == 300);
		CHECK(p.height_mm == 190);
		CHECK(p.source == XRT_STATUS_MM_SOURCE_EDID);
	}
	SECTION("FALLBACK, no EDID size: the plug-in's")
	{
		u_status_physical_pick(XRT_DISPLAY_CLAIM_FALLBACK, 0.344f, 0.194f, 0, 0, &p);
		CHECK(p.width_mm == 344);
		CHECK(p.source == XRT_STATUS_MM_SOURCE_PLUGIN);
	}
	SECTION("EDID and VERIFIED claims: the plug-in's win")
	{
		u_status_physical_pick(XRT_DISPLAY_CLAIM_EDID, 0.344f, 0.194f, 300, 190, &p);
		CHECK(p.width_mm == 344);
		CHECK(p.source == XRT_STATUS_MM_SOURCE_PLUGIN);
		u_status_physical_pick(XRT_DISPLAY_CLAIM_VERIFIED, 0.344f, 0.194f, 300, 190, &p);
		CHECK(p.height_mm == 194);
		CHECK(p.source == XRT_STATUS_MM_SOURCE_PLUGIN);
	}
	SECTION("neither: NONE, and NO_PHYSICAL_SIZE still fires")
	{
		u_status_physical_pick(XRT_DISPLAY_CLAIM_FALLBACK, 0.0f, 0.0f, 0, 0, &p);
		CHECK(p.source == XRT_STATUS_MM_SOURCE_NONE);
		auto s = healthy();
		s->screens[1].physical = p;
		s->screens[1].layout.width_m = 0.0f;
		s->screens[1].layout.height_m = 0.0f;
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 1) == V{"NO_PHYSICAL_SIZE"});
	}
}

TEST_CASE("status: a FALLBACK claim's layout metres follow the EDID", "[status][physical]")
{
	xrt_screen_info info{};
	info.width_m = 0.344f;
	info.height_m = 0.194f;
	info.nominal_viewer_z_m = 0.6f;
	info.source = XRT_SCREEN_INFO_SOURCE_SYSTEM;

	xrt_screen_info a = info;
	CHECK(u_screen_info_prefer_edid_mm(&a, XRT_DISPLAY_CLAIM_FALLBACK, 300, 190));
	CHECK(a.width_m == Catch::Approx(0.300f));
	CHECK(a.height_m == Catch::Approx(0.190f));
	CHECK(a.nominal_viewer_z_m == Catch::Approx(0.6f)); // the rest stays the plug-in's

	xrt_screen_info b = info;
	CHECK_FALSE(u_screen_info_prefer_edid_mm(&b, XRT_DISPLAY_CLAIM_VERIFIED, 300, 190));
	CHECK(b.width_m == Catch::Approx(0.344f));

	xrt_screen_info c = info;
	CHECK_FALSE(u_screen_info_prefer_edid_mm(&c, XRT_DISPLAY_CLAIM_FALLBACK, 0, 0));
	CHECK(c.width_m == Catch::Approx(0.344f));

	xrt_screen_info d = info;
	d.source = XRT_SCREEN_INFO_SOURCE_DERIVED; // already EDID-derived: untouched
	d.width_m = 0.5f;
	CHECK_FALSE(u_screen_info_prefer_edid_mm(&d, XRT_DISPLAY_CLAIM_FALLBACK, 300, 190));
	CHECK(d.width_m == Catch::Approx(0.5f));
}

namespace {

struct fake_fs
{
	std::set<std::string> files;
};

bool
fake_exists(const char *path, void *user)
{
	return static_cast<fake_fs *>(user)->files.count(path) != 0;
}

std::string
resolve(const u_openxr_runtime_env &env, fake_fs &fs)
{
	char out[512];
	if (!u_openxr_active_runtime_resolve(&env, fake_exists, &fs, out, sizeof(out))) {
		return "<none>";
	}
	return out;
}

} // namespace

TEST_CASE("status: active OpenXR runtime follows the loader's POSIX chain", "[status][runtime]")
{
	fake_fs fs;
	u_openxr_runtime_env env{};
	env.home = "/home/u";
	env.arch = "x86_64";

	CHECK(resolve(env, fs) == "<none>");

	SECTION("XR_RUNTIME_JSON wins, even when the file is missing")
	{
		fs.files = {"/home/u/.config/openxr/1/active_runtime.json"};
		env.xr_runtime_json = "/opt/dev/openxr_displayxr-dev.json";
		CHECK(resolve(env, fs) == "/opt/dev/openxr_displayxr-dev.json");
	}
	SECTION("XDG_CONFIG_HOME, defaulting to ~/.config, before the system dirs")
	{
		fs.files = {"/home/u/.config/openxr/1/active_runtime.json", "/etc/xdg/openxr/1/active_runtime.json"};
		CHECK(resolve(env, fs) == "/home/u/.config/openxr/1/active_runtime.json");
		env.xdg_config_home = "/cfg";
		CHECK(resolve(env, fs) == "/etc/xdg/openxr/1/active_runtime.json");
		fs.files.insert("/cfg/openxr/1/active_runtime.json");
		CHECK(resolve(env, fs) == "/cfg/openxr/1/active_runtime.json");
	}
	SECTION("the arch-suffixed name beats the plain one, within one directory")
	{
		fs.files = {"/etc/xdg/openxr/1/active_runtime.json", "/etc/xdg/openxr/1/active_runtime.x86_64.json",
		            "/usr/share/openxr/1/active_runtime.x86_64.json"};
		CHECK(resolve(env, fs) == "/etc/xdg/openxr/1/active_runtime.x86_64.json");
		env.arch = nullptr;
		CHECK(resolve(env, fs) == "/etc/xdg/openxr/1/active_runtime.json");
	}
	SECTION("XDG_CONFIG_DIRS in order (Ubuntu puts xdg-ubuntu first), default /etc/xdg")
	{
		fs.files = {"/etc/xdg/openxr/1/active_runtime.json"};
		env.xdg_config_dirs = "/etc/xdg/xdg-ubuntu:/etc/xdg";
		CHECK(resolve(env, fs) == "/etc/xdg/openxr/1/active_runtime.json");
		fs.files.insert("/etc/xdg/xdg-ubuntu/openxr/1/active_runtime.json");
		CHECK(resolve(env, fs) == "/etc/xdg/xdg-ubuntu/openxr/1/active_runtime.json");
		env.xdg_config_dirs = "/nowhere"; // replaces the default
		CHECK(resolve(env, fs) == "<none>");
	}
	SECTION("then /etc, /usr/local/share, /usr/share")
	{
		fs.files = {"/usr/share/openxr/1/active_runtime.json", "/usr/local/share/openxr/1/active_runtime.json"};
		CHECK(resolve(env, fs) == "/usr/local/share/openxr/1/active_runtime.json");
		fs.files.insert("/etc/openxr/1/active_runtime.json");
		CHECK(resolve(env, fs) == "/etc/openxr/1/active_runtime.json");
	}
	SECTION("no HOME and no XDG_CONFIG_HOME skips the per-user step")
	{
		env.home = nullptr;
		fs.files = {"/usr/share/openxr/1/active_runtime.json"};
		CHECK(resolve(env, fs) == "/usr/share/openxr/1/active_runtime.json");
	}
}

TEST_CASE("status: SEGMENT_FLAT_2D marks the screen of a DP-less segment", "[status][warnings]")
{
	auto s = healthy();
	xrt_status_client &c = s->clients[s->client_count++];
	c.id = 3;
	c.segments.count = 2;
	c.segments.split = true;
	c.segments.items[0] = {kScreenA, {0, 0, 822, 1875}, true, true, XRT_STATUS_EYE_SOURCE_DP};
	c.segments.items[1] = {kScreenB, {822, 0, 842, 1875}, true, true, XRT_STATUS_EYE_SOURCE_DP};
	u_status_warnings_derive(s.get());
	CHECK(screen_codes(*s, 1).empty());

	c.segments.items[1].has_dp = false;
	c.segments.items[1].woven = false;
	u_status_warnings_derive(s.get());
	CHECK(screen_codes(*s, 0).empty());
	REQUIRE(screen_codes(*s, 1) == V{"SEGMENT_FLAT_2D"});
	CHECK(s->screens[1].warnings[0].level == XRT_STATUS_LEVEL_WARN);
}

TEST_CASE("status: TRACKER_DOWN needs timing, a bound DP and a viewer tracked elsewhere", "[status][warnings]")
{
	auto s = healthy();
	s->screens[1].eye_tracking.state = XRT_STATUS_TRACKING_NOT_TRACKING;

	SECTION("over 5 s while screen 0 tracks")
	{
		s->screens[1].eye_tracking.not_tracking_ms = 6000;
		u_status_warnings_derive(s.get());
		REQUIRE(screen_codes(*s, 1) == V{"TRACKER_DOWN"});
		CHECK(s->screens[1].warnings[0].level == XRT_STATUS_LEVEL_WARN);
	}
	SECTION("under 5 s")
	{
		s->screens[1].eye_tracking.not_tracking_ms = 4000;
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 1).empty());
	}
	SECTION("nobody tracked anywhere (the viewer left)")
	{
		s->screens[1].eye_tracking.not_tracking_ms = 6000;
		s->screens[0].eye_tracking.state = XRT_STATUS_TRACKING_NOT_TRACKING;
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 1).empty());
	}
	SECTION("no DP bound")
	{
		s->screens[1].eye_tracking.not_tracking_ms = 6000;
		s->screens[1].dp_count = 0;
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 1).empty());
	}
	SECTION("tracking not supported")
	{
		s->screens[1].eye_tracking.not_tracking_ms = 6000;
		s->screens[1].eye_tracking.supported = 0;
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 1).empty());
	}
}

TEST_CASE("status: PLUGIN_NOT_READY carries the plug-in hint verbatim", "[status][warnings]")
{
	auto s = healthy();
	s->plugins[0].platform_state = XRT_PLUGIN_PLATFORM_STATE_NO_DISPLAY;
	SET(s->plugins[0].hint, "Connect the 3D display, then restart DisplayXR.");
	u_status_warnings_derive(s.get());
	for (uint32_t i = 0; i < 2; i++) {
		REQUIRE(screen_codes(*s, i) == V{"PLUGIN_NOT_READY"});
		CHECK(s->screens[i].warnings[0].level == XRT_STATUS_LEVEL_CRITICAL);
		CHECK(std::string(s->screens[i].warnings[0].text) == "Connect the 3D display, then restart DisplayXR.");
	}
	CHECK(sys_codes(*s).empty());

	SECTION("UNKNOWN (not reported) never warns")
	{
		s->plugins[0].platform_state = XRT_PLUGIN_PLATFORM_STATE_UNKNOWN;
		u_status_warnings_derive(s.get());
		CHECK(screen_codes(*s, 0).empty());
	}
	SECTION("the active plug-in claiming nothing goes to system level")
	{
		s->screen_count = 0;
		u_status_warnings_derive(s.get());
		REQUIRE(sys_codes(*s) == V{"PLUGIN_NOT_READY"});
		CHECK(std::string(s->warnings[0].text) == "Connect the 3D display, then restart DisplayXR.");
	}
	SECTION("no hint: a generic sentence naming the state")
	{
		s->plugins[0].hint[0] = '\0';
		u_status_warnings_derive(s.get());
		CHECK(std::string(s->screens[0].warnings[0].text).find("NO_DISPLAY") != std::string::npos);
	}
}

TEST_CASE("status: DP_DEGRADED / DP_STALE follow the bound DP's backend", "[status][warnings]")
{
	auto s = healthy();
	s->screens[0].dps[0].backend = XRT_STATUS_DP_BACKEND_DEGRADED;
	s->screens[1].dps[0].backend = XRT_STATUS_DP_BACKEND_STALE;
	u_status_warnings_derive(s.get());
	REQUIRE(screen_codes(*s, 0) == V{"DP_DEGRADED"});
	CHECK(s->screens[0].warnings[0].level == XRT_STATUS_LEVEL_WARN);
	REQUIRE(screen_codes(*s, 1) == V{"DP_STALE"});
	CHECK(s->screens[1].warnings[0].level == XRT_STATUS_LEVEL_CRITICAL);
}

TEST_CASE("status: warnings never overflow their arrays", "[status][warnings]")
{
	auto s = healthy();
	xrt_status_screen &sc = s->screens[0];
	sc.dp_count = XRT_STATUS_MAX_SCREEN_DPS;
	for (uint32_t i = 0; i < XRT_STATUS_MAX_SCREEN_DPS; i++) {
		sc.dps[i] = {i, XRT_STATUS_DP_API_D3D11, XRT_STATUS_DP_KIND_SEGMENT, XRT_STATUS_DP_BACKEND_STALE};
	}
	sc.desktop.width = 1920; // + NOT_NATIVE
	u_status_warnings_derive(s.get());
	CHECK(sc.warning_count == XRT_STATUS_MAX_WARNINGS);
	CHECK(find(sc.warnings, sc.warning_count, "NOT_NATIVE") != nullptr);
}


/*
 *
 * JSON (§3).
 *
 */

namespace {

std::vector<std::string>
keys(const cJSON *o)
{
	std::vector<std::string> v;
	for (const cJSON *c = o != nullptr ? o->child : nullptr; c != nullptr; c = c->next) {
		v.emplace_back(c->string);
	}
	return v;
}

std::string
str(const cJSON *o, const char *k)
{
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(it) ? std::string(it->valuestring) : std::string("<not a string>");
}

std::vector<std::string>
str_array(const cJSON *a)
{
	std::vector<std::string> v;
	const cJSON *it = nullptr;
	cJSON_ArrayForEach(it, a)
	{
		v.emplace_back(cJSON_IsString(it) ? it->valuestring : "<?>");
	}
	return v;
}

struct json_doc
{
	cJSON *root;
	explicit json_doc(const xrt_status_snapshot &s) : root(u_status_snapshot_to_cjson(&s)) {}
	~json_doc()
	{
		cJSON_Delete(root);
	}
};

} // namespace

TEST_CASE("status: JSON has schema first and the §3 key set", "[status][json]")
{
	auto s = healthy();
	xrt_status_client &c = s->clients[s->client_count++];
	c.id = 3;
	c.pid = 24416;
	c.class_verified = true;
	c.client_class = XRT_CLIENT_CLASS_APP;
	SET(c.name, "cube_handle_d3d11_win.exe");
	c.presenter = XRT_STATUS_PRESENTER_APP_HWND;
	c.lease = XRT_STATUS_LEASE_SLOT;
	c.window = {true, 3018, 285, 1664, 2093};
	c.owner_screen = kScreenB;
	c.segments.generation = 41;
	c.segments.split = true;
	c.segments.count = 1;
	c.segments.items[0] = {kScreenA, {0, 0, 822, 1875}, false, false, XRT_STATUS_EYE_SOURCE_NONE};
	c.views = {2, 2, 4};
	c.integrity = {1811, 1809, 2, "scanout"};
	u_status_warnings_derive(s.get());

	json_doc d(*s);
	REQUIRE(d.root != nullptr);
	REQUIRE(d.root->child != nullptr);
	CHECK(std::string(d.root->child->string) == "schema");
	CHECK(d.root->child->valuedouble == XRT_STATUS_SCHEMA);

	CHECK(keys(d.root) ==
	      V{"schema", "source", "generation", "runtime", "plugins", "screens", "clients", "workspace", "warnings",
	        "conv_providers", "conv_tasks", "input_holders",
	        "dp_preference"}); // ADR-053 (#2006), ADR-034 Am.5 (#2181), global DP choice: keys only added, schema 1
	CHECK(keys(cJSON_GetObjectItem(d.root, "dp_preference")) == V{"plugin_id", "source"});
	CHECK(str(d.root, "source") == "service");
	CHECK(keys(cJSON_GetObjectItem(d.root, "generation")) == V{"topology", "status", "conversion"});
	CHECK(keys(cJSON_GetObjectItem(d.root, "runtime")) ==
	      V{"version", "git_tag", "plugin_abi", "active_openxr_runtime"});

	const cJSON *p0 = cJSON_GetArrayItem(cJSON_GetObjectItem(d.root, "plugins"), 0);
	CHECK(keys(p0) == V{"id", "name", "vendor", "version", "load", "platform_state", "hint", "fallback", "enabled",
	                    "probe_order", "licence",
	                    // ADR-045 Amendment 2026-11-22: platform remedy, flat, always present.
	                    "update_recommended", "update_required", "installed_version", "required_version",
	                    "action_label", "action_url"});
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(p0, "update_recommended")));
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(p0, "enabled"))); // a zeroed row reads enabled (`disabled` is the field)
	// ADR-054 N5: the compact licence object; a plug-in with nothing to report says so.
	CHECK(keys(cJSON_GetObjectItem(p0, "licence")) == V{"state", "source", "expires_unix"});
	CHECK(str(cJSON_GetObjectItem(p0, "licence"), "state") == "not_applicable");
	CHECK(str(p0, "load") == "ACTIVE");
	CHECK(str(p0, "platform_state") == "READY");

	const cJSON *sc = cJSON_GetArrayItem(cJSON_GetObjectItem(d.root, "screens"), 0);
	CHECK(keys(sc) == V{"id", "index", "device_name", "friendly_name", "key", "edid", "desktop", "native",
	                    "physical_mm", "roles", "claim", "layout", "eye_tracking", "viewer_tracking", "mode",
	                    "mode_3d", "dps", "vendor", "warnings"});
	CHECK(str(sc, "id") == "0x8c413a2f61152ce7");
	CHECK(keys(cJSON_GetObjectItem(sc, "edid")) == V{"manufacturer", "product", "serial"});
	CHECK(keys(cJSON_GetObjectItem(sc, "desktop")) == V{"left", "top", "width", "height", "scale"});
	CHECK(keys(cJSON_GetObjectItem(sc, "native")) == V{"width", "height", "refresh_mhz", "is_native"});
	CHECK(keys(cJSON_GetObjectItem(sc, "physical_mm")) == V{"width", "height", "source"});
	CHECK(str(cJSON_GetObjectItem(sc, "physical_mm"), "source") == "plugin");
	CHECK(keys(cJSON_GetObjectItem(sc, "roles")) == V{"os_main", "runtime_default", "vendor_primary"});
	const cJSON *claim = cJSON_GetObjectItem(sc, "claim");
	CHECK(keys(claim) == V{"plugin_id", "confidence", "confidence_value", "serial", "apis", "forced",
	                       "preferred_plugin", "preferred_source", "apply"});
	CHECK(str(sc, "key") == "AUO-B194-00000000@DISPLAY1");
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(claim, "forced")));
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(claim, "preferred_plugin")));
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(claim, "preferred_source")));
	CHECK(str(claim, "apply") == "next-session");
	CHECK(str(claim, "confidence") == "VERIFIED");
	CHECK(str_array(cJSON_GetObjectItem(claim, "apis")) == V{"d3d11", "d3d12", "vk", "gl"});
	const cJSON *lay = cJSON_GetObjectItem(sc, "layout");
	CHECK(keys(lay) == V{"width_m", "height_m", "nominal_viewer_m", "source"});
	CHECK(keys(cJSON_GetObjectItem(lay, "nominal_viewer_m")) == V{"x", "y", "z"});
	CHECK(str(lay, "source") == "plugin");
	const cJSON *et = cJSON_GetObjectItem(sc, "eye_tracking");
	// ADR-054 N6 Part 2: the viewer source (null when not evaluated in the builder's process).
	CHECK(keys(et) == V{"supported", "default", "state", "viewer_source", "viewer_reason", "viewer_policy",
	                    "viewer_provider", "viewer_host"});
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(et, "viewer_host")));
	CHECK(str_array(cJSON_GetObjectItem(et, "supported")) == V{"MANAGED"});
	CHECK(str(et, "default") == "MANAGED");
	CHECK(str(et, "state") == "TRACKING");
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(sc, "mode")));
	const cJSON *dp0 = cJSON_GetArrayItem(cJSON_GetObjectItem(sc, "dps"), 0);
	CHECK(keys(dp0) == V{"client_id", "api", "kind", "backend"});
	CHECK(str(dp0, "api") == "d3d11");
	CHECK(str(dp0, "kind") == "primary");
	CHECK(str(dp0, "backend") == "OK");
	const cJSON *ven = cJSON_GetObjectItem(sc, "vendor");
	CHECK(keys(ven) == V{"present", "ready", "verified", "calibrated", "tracker", "lens", "model", "serial",
	                     "worst_warning", "dashboard_command"});
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(ven, "present")));
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(ven, "worst_warning")));

	// Screen A carries SEGMENT_FLAT_2D (the DP-less segment above).
	const cJSON *w0 = cJSON_GetArrayItem(cJSON_GetObjectItem(sc, "warnings"), 0);
	CHECK(keys(w0) == V{"code", "level", "text"});
	CHECK(str(w0, "code") == "SEGMENT_FLAT_2D");
	CHECK(str(w0, "level") == "warn");

	const cJSON *cl = cJSON_GetArrayItem(cJSON_GetObjectItem(d.root, "clients"), 0);
	CHECK(keys(cl) == V{"id", "pid", "class", "name", "flags", "presenter", "lease", "window", "owner_screen",
	                    "segments", "views", "integrity", "visibility"});
	// ADR-062: an untracked window has no visibility block.
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(cl, "visibility")));
	CHECK(str(cl, "class") == "APP");
	CHECK(str(cl, "presenter") == "APP_HWND");
	CHECK(str(cl, "lease") == "slot");
	CHECK(str(cl, "owner_screen") == "0xb72ea4c616544d01");
	CHECK(keys(cJSON_GetObjectItem(cl, "flags")) == V{"active", "visible", "focused", "overlay", "input_focused"});
	CHECK(keys(cJSON_GetObjectItem(cl, "window")) == V{"left", "top", "width", "height"});
	const cJSON *sg = cJSON_GetObjectItem(cl, "segments");
	CHECK(keys(sg) == V{"generation", "split", "items"});
	const cJSON *it = cJSON_GetArrayItem(cJSON_GetObjectItem(sg, "items"), 0);
	CHECK(keys(it) == V{"screen", "canvas", "has_dp", "woven", "eye_source"});
	CHECK(keys(cJSON_GetObjectItem(it, "canvas")) == V{"x", "y", "w", "h"});
	CHECK(str(it, "eye_source") == "NONE");
	CHECK(keys(cJSON_GetObjectItem(cl, "views")) == V{"capacity", "active", "reported"});
	CHECK(keys(cJSON_GetObjectItem(cl, "integrity")) == V{"paint", "present", "skip", "weave_placement"});
	CHECK(str(cJSON_GetObjectItem(cl, "integrity"), "weave_placement") == "scanout");

	CHECK(keys(cJSON_GetObjectItem(d.root, "workspace")) == V{"enabled", "controller", "screens"});
}

TEST_CASE("status: a tracked client window reports its visibility (ADR-062)", "[status][json]")
{
	auto s = std::make_unique<xrt_status_snapshot>();
	std::memset(s.get(), 0, sizeof(*s));
	s->source = XRT_STATUS_SOURCE_SERVICE;
	s->client_count = 1;
	xrt_status_client &c = s->clients[0];
	c.id = 1;
	c.visibility.tracked = true;
	c.visibility.hidden = true;
	std::snprintf(c.visibility.reason, sizeof(c.visibility.reason), "covered");
	std::snprintf(c.visibility.occluder, sizeof(c.visibility.occluder), "notepad.exe");

	cJSON *root = u_status_snapshot_to_cjson(s.get());
	REQUIRE(root != nullptr);
	const cJSON *cl = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "clients"), 0);
	const cJSON *vi = cJSON_GetObjectItem(cl, "visibility");
	REQUIRE(cJSON_IsObject(vi));
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(vi, "hidden")));
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(vi, "hidden_everywhere")));
	CHECK(std::string(cJSON_GetObjectItem(vi, "reason")->valuestring) == "covered");
	CHECK(std::string(cJSON_GetObjectItem(vi, "occluder")->valuestring) == "notepad.exe");
	cJSON_Delete(root);

	std::vector<char> text(64 * 1024);
	u_status_snapshot_format_text(s.get(), text.data(), text.size());
	CHECK(std::string(text.data()).find("visibility: hidden (covered, covered by notepad.exe) - lens 2D") !=
	      std::string::npos);
}

TEST_CASE("status: a plug-in's platform remedy reaches the JSON and the text (ADR-045 Am. 2026-11-22)",
          "[status][json]")
{
	auto s = healthy();
	xrt_status_platform_remedy &r = s->plugins[0].remedy;
	r.update_recommended = true;
	SET(r.installed_version, "1.2.0");
	SET(r.required_version, "1.4.0");
	SET(r.action_label, "Download the update");
	SET(r.action_url, "https://example.com/update");
	u_status_warnings_derive(s.get());
	CHECK(sys_codes(*s).empty()); // advisory: no snapshot warning, nothing re-routes

	json_doc_light d(*s);
	const cJSON *p0 = cJSON_GetArrayItem(cJSON_GetObjectItem(d.root, "plugins"), 0);
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(p0, "update_recommended")));
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(p0, "update_required")));
	CHECK(str(p0, "installed_version") == "1.2.0");
	CHECK(str(p0, "action_url") == "https://example.com/update");

	std::vector<char> text(64 * 1024);
	u_status_snapshot_format_text(s.get(), text.data(), text.size());
	CHECK(std::string(text.data())
	          .find(std::string("PLUG-IN  ") + s->plugins[0].id +
	                ": Update recommended: 1.2.0 -> 1.4.0. Download the update: "
	                "https://example.com/update\n") != std::string::npos);
}

TEST_CASE("status: the global display-driver preference reaches the JSON and the text", "[status][json][text]")
{
	{
		// None set: the key is there, both members null; no text line.
		auto s = healthy();
		json_doc_light d(*s);
		const cJSON *p = cJSON_GetObjectItem(d.root, "dp_preference");
		REQUIRE(cJSON_IsObject(p));
		CHECK(cJSON_IsNull(cJSON_GetObjectItem(p, "plugin_id")));
		CHECK(cJSON_IsNull(cJSON_GetObjectItem(p, "source")));
		std::vector<char> text(64 * 1024);
		u_status_snapshot_format_text(s.get(), text.data(), text.size());
		CHECK(std::string(text.data()).find("DP PREF") == std::string::npos);
		char line[160];
		CHECK_FALSE(u_status_dp_preference_format(&s->dp_preference, line, sizeof(line)));
		CHECK(line[0] == '\0');
	}
	const struct
	{
		xrt_status_pref_source src;
		const char *word;
	} cases[] = {{XRT_STATUS_PREF_SOURCE_USER, "user"},
	             {XRT_STATUS_PREF_SOURCE_MACHINE, "machine"},
	             {XRT_STATUS_PREF_SOURCE_ENV, "env"}};
	for (const auto &c : cases) {
		auto s = healthy();
		SET(s->dp_preference.plugin_id, "sim-display");
		s->dp_preference.source = c.src;
		u_status_warnings_derive(s.get());
		CHECK(sys_codes(*s).empty()); // a choice, not a problem: no warning

		json_doc_light d(*s);
		const cJSON *p = cJSON_GetObjectItem(d.root, "dp_preference");
		REQUIRE(cJSON_IsObject(p));
		CHECK(str(p, "plugin_id") == "sim-display");
		CHECK(str(p, "source") == c.word);
		// Additive: the per-screen claim keys are untouched.
		const cJSON *claim =
		    cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(d.root, "screens"), 0), "claim");
		CHECK(cJSON_IsNull(cJSON_GetObjectItem(claim, "preferred_plugin")));

		std::vector<char> text(64 * 1024);
		u_status_snapshot_format_text(s.get(), text.data(), text.size());
		const std::string t(text.data());
		const std::string want = std::string("DP PREF  sim-display for every screen (") + c.word +
		                         "); a per-screen choice still wins\n";
		CHECK(t.find(want) != std::string::npos);
		CHECK(t.find("DP PREF") == t.rfind("DP PREF")); // one line only
	}
}

TEST_CASE("status: the workspace block lists the screens it presents on (ADR-052)", "[status][json]")
{
	{
		// Off: an empty array, never absent (additive, always the same shape).
		auto off = healthy();
		json_doc d(*off);
		REQUIRE(d.root != nullptr);
		const cJSON *scr = cJSON_GetObjectItem(cJSON_GetObjectItem(d.root, "workspace"), "screens");
		REQUIRE(cJSON_IsArray(scr));
		CHECK(cJSON_GetArraySize(scr) == 0);
	}

	auto s = healthy();
	s->workspace.enabled = true;
	SET(s->workspace.controller, "ctl");
	s->workspace.screen_count = 2;
	s->workspace.screens[0] = {kScreenA, true, true};
	s->workspace.screens[1] = {kScreenB, false, false};

	json_doc d(*s);
	REQUIRE(d.root != nullptr);
	const cJSON *scr = cJSON_GetObjectItem(cJSON_GetObjectItem(d.root, "workspace"), "screens");
	REQUIRE(cJSON_IsArray(scr));
	REQUIRE(cJSON_GetArraySize(scr) == 2);
	const cJSON *a = cJSON_GetArrayItem(scr, 0);
	CHECK(keys(a) == V{"id", "primary", "woven"});
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(a, "primary")));
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(a, "woven")));
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(scr, 1), "woven")));
}

TEST_CASE("status: enums serialise as the documented strings", "[status][json]")
{
	CHECK(std::string(u_status_source_str(XRT_STATUS_SOURCE_SERVICE)) == "service");
	CHECK(std::string(u_status_source_str(XRT_STATUS_SOURCE_HEADLESS)) == "headless");
	CHECK(std::string(u_status_tracking_str(XRT_STATUS_TRACKING_TRACKING)) == "TRACKING");
	CHECK(std::string(u_status_tracking_str(XRT_STATUS_TRACKING_NOT_TRACKING)) == "NOT_TRACKING");
	CHECK(std::string(u_status_tracking_str(XRT_STATUS_TRACKING_NO_DP)) == "NO_DP");
	CHECK(std::string(u_status_tracking_str(XRT_STATUS_TRACKING_UNKNOWN)) == "UNKNOWN");
	CHECK(std::string(u_status_presenter_str(XRT_STATUS_PRESENTER_APP_HWND)) == "APP_HWND");
	CHECK(std::string(u_status_presenter_str(XRT_STATUS_PRESENTER_CLIENT_TEXTURE)) == "CLIENT_TEXTURE");
	CHECK(std::string(u_status_presenter_str(XRT_STATUS_PRESENTER_NONE)) == "NONE");
	CHECK(std::string(u_status_lease_str(XRT_STATUS_LEASE_CONTROLLER)) == "controller");
	CHECK(std::string(u_status_lease_str(XRT_STATUS_LEASE_SLOT)) == "slot");
	CHECK(std::string(u_status_lease_str(XRT_STATUS_LEASE_NONE)) == "none");
	CHECK(std::string(u_status_level_str(XRT_STATUS_LEVEL_INFO)) == "info");
	CHECK(std::string(u_status_level_str(XRT_STATUS_LEVEL_WARN)) == "warn");
	CHECK(std::string(u_status_level_str(XRT_STATUS_LEVEL_CRITICAL)) == "critical");
	CHECK(std::string(u_status_vendor_tracker_str(XRT_STATUS_VENDOR_TRACKER_RUNNING)) == "RUNNING");
	CHECK(std::string(u_status_vendor_tracker_str(XRT_STATUS_VENDOR_TRACKER_DOWN)) == "DOWN");
	CHECK(std::string(u_status_vendor_lens_str(XRT_STATUS_VENDOR_LENS_3D)) == "3D");
	CHECK(std::string(u_status_vendor_lens_str(XRT_STATUS_VENDOR_LENS_2D)) == "2D");
	CHECK(std::string(u_status_mm_source_str(XRT_STATUS_MM_SOURCE_EDID)) == "edid");
	CHECK(std::string(u_status_confidence_str(XRT_DISPLAY_CLAIM_FALLBACK)) == "FALLBACK");
	CHECK(std::string(u_status_confidence_str(XRT_DISPLAY_CLAIM_EDID)) == "EDID");
	CHECK(std::string(u_status_confidence_str(XRT_DISPLAY_CLAIM_VERIFIED)) == "VERIFIED");
	CHECK(std::string(u_status_layout_source_str(XRT_SCREEN_INFO_SOURCE_DERIVED)) == "derived");
	CHECK(std::string(u_status_platform_state_str(XRT_PLUGIN_PLATFORM_STATE_NO_DISPLAY)) == "NO_DISPLAY");
	CHECK(std::string(u_status_client_class_str(XRT_CLIENT_CLASS_CONTROLLER, true)) == "CONTROLLER");
	CHECK(std::string(u_status_client_class_str(XRT_CLIENT_CLASS_APP, false)) == "UNVERIFIED");
	CHECK(std::string(u_status_dp_backend_str(XRT_STATUS_DP_BACKEND_STALE)) == "STALE");
	CHECK(std::string(u_status_eye_source_str(XRT_STATUS_EYE_SOURCE_DP)) == "DP");
	CHECK(u_status_pref_source_str(XRT_STATUS_PREF_SOURCE_NONE) == nullptr);
	CHECK(std::string(u_status_pref_source_str(XRT_STATUS_PREF_SOURCE_USER)) == "user");
	CHECK(std::string(u_status_pref_source_str(XRT_STATUS_PREF_SOURCE_MACHINE)) == "machine");
	CHECK(std::string(u_status_pref_source_str(XRT_STATUS_PREF_SOURCE_ENV)) == "env");
	CHECK(std::string(u_status_apply_str(XRT_STATUS_APPLY_LIVE)) == "live");
	CHECK(std::string(u_status_apply_str(XRT_STATUS_APPLY_NEXT_SESSION)) == "next-session");
}

TEST_CASE("status: headless JSON nulls what it cannot know", "[status][json]")
{
	auto s = healthy();
	s->source = XRT_STATUS_SOURCE_HEADLESS;
	s->generation = {0, 0};
	for (uint32_t i = 0; i < s->screen_count; i++) {
		s->screens[i].dp_count = 0;
		s->screens[i].eye_tracking.state = XRT_STATUS_TRACKING_NO_DP;
	}
	u_status_warnings_derive(s.get());
	json_doc d(*s);
	CHECK(str(d.root, "source") == "headless");
	CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(d.root, "clients")) == 0);
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(d.root, "workspace"), "controller")));
	const cJSON *w = cJSON_GetArrayItem(cJSON_GetObjectItem(d.root, "warnings"), 0);
	CHECK(str(w, "code") == "SERVICE_HEADLESS");
	CHECK(str(w, "level") == "info");
	const cJSON *sc = cJSON_GetArrayItem(cJSON_GetObjectItem(d.root, "screens"), 0);
	CHECK(str(cJSON_GetObjectItem(sc, "eye_tracking"), "state") == "NO_DP");
}


/*
 *
 * Text (§7).
 *
 */

namespace {

void
fill_long(char *dst, size_t cap)
{
	std::memset(dst, 'X', cap - 1);
	dst[cap - 1] = '\0';
}

//! Every array full, every string at its maximum: the worst case for the formatter.
std::unique_ptr<xrt_status_snapshot>
worst_case()
{
	auto s = healthy();
	s->plugin_count = 0;
	for (uint32_t i = 0; i < XRT_STATUS_MAX_PLUGINS; i++) {
		xrt_status_plugin &p = s->plugins[s->plugin_count++];
		fill_long(p.id, sizeof(p.id));
		fill_long(p.load, sizeof(p.load));
	}
	s->screen_count = 0;
	for (uint32_t i = 0; i < XRT_STATUS_MAX_SCREENS; i++) {
		add_screen(*s, 0x1000 + i, "leia-sr", XRT_DISPLAY_CLAIM_VERIFIED);
		xrt_status_screen &sc = s->screens[i];
		fill_long(sc.device_name, sizeof(sc.device_name));
		fill_long(sc.friendly_name, sizeof(sc.friendly_name));
		fill_long(sc.claim.plugin_id, sizeof(sc.claim.plugin_id));
		sc.desktop = {-2147483647, -2147483647, 4294967295u, 4294967295u, 123.456f};
		sc.roles = {true, true, true};
		sc.vendor.present = true;
		fill_long(sc.vendor.model, sizeof(sc.vendor.model));
		fill_long(sc.vendor.serial, sizeof(sc.vendor.serial));
		sc.dp_count = XRT_STATUS_MAX_SCREEN_DPS;
		for (uint32_t d = 0; d < XRT_STATUS_MAX_SCREEN_DPS; d++) {
			sc.dps[d] = {4294967295u, XRT_STATUS_DP_API_METAL, XRT_STATUS_DP_KIND_SEGMENT,
			             XRT_STATUS_DP_BACKEND_DEGRADED};
		}
		sc.warning_count = XRT_STATUS_MAX_WARNINGS;
		for (uint32_t w = 0; w < XRT_STATUS_MAX_WARNINGS; w++) {
			fill_long(sc.warnings[w].code, sizeof(sc.warnings[w].code));
			fill_long(sc.warnings[w].text, sizeof(sc.warnings[w].text));
		}
	}
	s->client_count = 0;
	for (uint32_t i = 0; i < XRT_STATUS_MAX_CLIENTS; i++) {
		xrt_status_client &c = s->clients[s->client_count++];
		c.id = 4294967295u;
		fill_long(c.name, sizeof(c.name));
		c.window = {true, -2147483647, -2147483647, 4294967295u, 4294967295u};
		c.owner_screen = 0x1000;
		c.segments.count = XRT_STATUS_MAX_SEGMENTS;
		for (uint32_t k = 0; k < XRT_STATUS_MAX_SEGMENTS; k++) {
			c.segments.items[k] = {0x1000 + k,
			                       {-2147483647, -2147483647, 4294967295u, 4294967295u},
			                       true,
			                       true,
			                       XRT_STATUS_EYE_SOURCE_PRIMARY};
		}
		c.integrity.paint = ~0ull;
		fill_long(c.integrity.weave_placement, sizeof(c.integrity.weave_placement));
		c.visibility.tracked = true;
		c.visibility.hidden = true;
		c.visibility.hidden_everywhere = true;
		fill_long(c.visibility.reason, sizeof(c.visibility.reason));
		fill_long(c.visibility.occluder, sizeof(c.visibility.occluder));
	}
	s->warning_count = XRT_STATUS_MAX_WARNINGS;
	for (uint32_t w = 0; w < XRT_STATUS_MAX_WARNINGS; w++) {
		fill_long(s->warnings[w].code, sizeof(s->warnings[w].code));
		fill_long(s->warnings[w].text, sizeof(s->warnings[w].text));
	}
	s->workspace.enabled = true;
	fill_long(s->workspace.controller, sizeof(s->workspace.controller));
	s->workspace.screen_count = XRT_STATUS_WORKSPACE_MAX_SCREENS;
	for (uint32_t w = 0; w < XRT_STATUS_WORKSPACE_MAX_SCREENS; w++) {
		s->workspace.screens[w] = {~0ull - w, w == 0, false};
	}
	return s;
}

} // namespace

TEST_CASE("status: text formatting never overflows (16 screens x all clients)", "[status][text]")
{
	auto s = worst_case();
	const size_t need = u_status_snapshot_format_text(s.get(), nullptr, 0);
	REQUIRE(need > 0);

	// Exactly-sized buffer: the whole text, NUL-terminated.
	std::vector<char> full(need + 1 + 64, '\x7f');
	CHECK(u_status_snapshot_format_text(s.get(), full.data(), need + 1) == need);
	CHECK(std::strlen(full.data()) == need);
	for (size_t i = need + 1; i < full.size(); i++) {
		REQUIRE(full[i] == '\x7f');
	}

	// Small buffers: truncated, terminated, guard bytes untouched.
	for (size_t cap : {size_t(1), size_t(2), size_t(100), size_t(4096), need / 2, need}) {
		std::vector<char> b(cap + 64, '\x7f');
		CHECK(u_status_snapshot_format_text(s.get(), b.data(), cap) == need);
		CHECK(std::strlen(b.data()) == cap - 1);
		CHECK(std::strncmp(b.data(), full.data(), cap - 1) == 0);
		for (size_t i = cap; i < b.size(); i++) {
			REQUIRE(b[i] == '\x7f');
		}
	}

	// And the JSON of the same worst case builds.
	json_doc d(*s);
	CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(d.root, "screens")) == XRT_STATUS_MAX_SCREENS);
	CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(d.root, "clients")) == XRT_STATUS_MAX_CLIENTS);
}

TEST_CASE("status: text shows the §7 sections", "[status][text]")
{
	auto s = healthy();
	s->source = XRT_STATUS_SOURCE_HEADLESS;
	u_status_warnings_derive(s.get());
	std::vector<char> b(u_status_snapshot_format_text(s.get(), nullptr, 0) + 1);
	u_status_snapshot_format_text(s.get(), b.data(), b.size());
	const std::string t(b.data());
	CHECK(t.rfind("DisplayXR 2.32.0 (v2.32.0)", 0) == 0);
	CHECK(t.find("source: headless") != std::string::npos);
	CHECK(t.find("plug-ins: leia-sr READY, sim-display fallback") != std::string::npos);
	CHECK(t.find("SCREENS  2 monitors") != std::string::npos);
	CHECK(t.find("2 claimed (2 VERIFIED)") != std::string::npos);
	CHECK(t.find("leia-sr VERIFIED") != std::string::npos);
	CHECK(t.find("roles: OS main · runtime default") != std::string::npos);
	CHECK(t.find("WINDOWS") != std::string::npos);
	CHECK(t.find("[info] SERVICE_HEADLESS") != std::string::npos);
}

TEST_CASE("status: per-screen viewer_tracking object (ADR-066 Part B)", "[status][json][viewer_tracking]")
{
	auto s = healthy();
	// Screen A: a laptop-style screen set to 3d, served by the provider, in the 3D mode right now.
	xrt_status_viewer_tracking &a = s->screens[0].eye_tracking.viewer_tracking;
	a.evaluated = true;
	SET(a.setting, "3d");
	SET(a.setting_source, "user");
	SET(a.effective, "3d");
	a.tracked_3d = true;
	a.available = true;
	SET(a.reason, "webcam 'Integrated Camera'");
	SET(a.provider, "sim-tracking");
	s->screens[0].mode = {true, 1, "3D", 2, true};
	// Screen B: a panel that tracks the viewer itself: not offered.
	xrt_status_viewer_tracking &b = s->screens[1].eye_tracking.viewer_tracking;
	b.evaluated = true;
	SET(b.setting, "default");
	SET(b.setting_source, "default");
	SET(b.effective, "off");
	SET(b.reason, "this display tracks the viewer itself");

	json_doc d(*s);
	const cJSON *screens = cJSON_GetObjectItem(d.root, "screens");
	const cJSON *va = cJSON_GetObjectItem(cJSON_GetArrayItem(screens, 0), "viewer_tracking");
	CHECK(keys(va) == std::vector<std::string>{"setting", "setting_source", "effective", "tracked", "current_mode",
	                                           "active", "provider", "available", "reason", "detail"});
	CHECK(str(va, "setting") == "3d");
	CHECK(str(va, "effective") == "3d");
	CHECK(str(va, "current_mode") == "3d");
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(va, "active")));
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetObjectItem(va, "tracked"), "2d")));
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(va, "tracked"), "3d")));
	CHECK(str(va, "provider") == "sim-tracking");
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(va, "available")));
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(va, "reason")));
	CHECK(str(va, "detail") == "webcam 'Integrated Camera'");

	const cJSON *vb = cJSON_GetObjectItem(cJSON_GetArrayItem(screens, 1), "viewer_tracking");
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(vb, "available")));
	CHECK(str(vb, "reason") == "this display tracks the viewer itself");
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(vb, "detail")));
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(vb, "current_mode"))); // no session on it: no mode
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(vb, "active")));
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(vb, "provider")));
}

namespace {

//! The head device's table as the vendor plug-in declares it: one 3D entry.
void
leia_head_mode_3d(xrt_status_mode_3d &m)
{
	std::memset(&m, 0, sizeof(m));
	m.valid = true;
	SET(m.active, "leiasr");
	SET(m.driver, "leia-sr");
	m.source = XRT_STATUS_MODE_3D_SOURCE_RUNTIME_DEFAULT;
	m.choice_count = 1;
	SET(m.choices[0].name, "leiasr");
	SET(m.choices[0].label, "LeiaSR");
	m.choices[0].views = 2;
	m.choices[0].tracked = true;
	m.choices[0].applies = XRT_STATUS_MODE_3D_APPLIES_NOW;
}

//! `dp use sim-display --screen <primary>`: the primary is claimed (woven) by sim-display, forced.
void
force_primary_to_sim(xrt_status_snapshot &s)
{
	xrt_status_screen &sc = s.screens[0];
	SET(sc.claim.plugin_id, "sim-display");
	sc.claim.confidence = XRT_DISPLAY_CLAIM_FALLBACK;
	sc.claim.forced = true;
	SET(sc.claim.preferred_plugin, "sim-display");
	sc.claim.preferred_source = XRT_STATUS_PREF_SOURCE_USER;
	sc.claim.apply = XRT_STATUS_APPLY_NEXT_SESSION;
}

} // namespace

TEST_CASE("status: the primary screen's 3D modes say whose they are when another driver weaves it",
          "[status][mode_3d][driver]")
{
	auto s = healthy();
	leia_head_mode_3d(s->screens[0].mode_3d);
	leia_head_mode_3d(s->screens[1].mode_3d);

	SECTION("the head's own screen: no note, `driver` names the table's plug-in")
	{
		CHECK_FALSE(u_status_mode_3d_driver_differs(&s->screens[0]));
		CHECK_FALSE(u_status_mode_3d_note_driver(&s->screens[0]));
		CHECK(s->screens[0].mode_3d.reason[0] == '\0');
		cJSON *j = u_status_mode_3d_to_cjson(&s->screens[0].mode_3d);
		CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItem(j, "driver"))) == "leia-sr");
		CHECK(cJSON_IsNull(cJSON_GetObjectItem(j, "reason")));
		cJSON_Delete(j);
	}
	SECTION("dp use sim-display --screen <primary>: the table stays the head's, and says so")
	{
		force_primary_to_sim(*s);
		CHECK(u_status_mode_3d_driver_differs(&s->screens[0]));
		REQUIRE(u_status_mode_3d_note_driver(&s->screens[0]));
		const xrt_status_mode_3d &m = s->screens[0].mode_3d;
		CHECK(std::string(m.reason) ==
		      "its 3D modes are those of the head display driver 'leia-sr'; 'sim-display' only weaves it");
		// The table itself is untouched: what the head device maps slot 1 to.
		CHECK(std::string(m.active) == "leiasr");
		CHECK(m.choice_count == 1);
		cJSON *j = u_status_mode_3d_to_cjson(&m);
		CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItem(j, "driver"))) == "leia-sr");
		CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItem(j, "reason"))) == m.reason);
		cJSON_Delete(j);
	}
	SECTION("the note replaces a stored-choice reason: the split is why the name is missing")
	{
		force_primary_to_sim(*s);
		SET(s->screens[0].mode_3d.setting, "anaglyph");
		SET(s->screens[0].mode_3d.reason,
		    "the chosen 3D mode 'anaglyph' is not offered by this display's driver; the default is used");
		REQUIRE(u_status_mode_3d_note_driver(&s->screens[0]));
		CHECK(std::string(s->screens[0].mode_3d.reason).rfind("its 3D modes are those of", 0) == 0);
		CHECK(std::string(s->screens[0].mode_3d.setting) == "anaglyph");
	}
	SECTION("only the system-default screen: a secondary screen's table is its own plug-in's")
	{
		SET(s->screens[1].claim.plugin_id, "sim-display");
		CHECK_FALSE(u_status_mode_3d_driver_differs(&s->screens[1]));
		CHECK_FALSE(u_status_mode_3d_note_driver(&s->screens[1]));
	}
	SECTION("nothing to say without a table, or without a known driver")
	{
		force_primary_to_sim(*s);
		s->screens[0].mode_3d.driver[0] = '\0'; // an older service's row
		CHECK_FALSE(u_status_mode_3d_driver_differs(&s->screens[0]));
		cJSON *j = u_status_mode_3d_to_cjson(&s->screens[0].mode_3d);
		CHECK(cJSON_IsNull(cJSON_GetObjectItem(j, "driver")));
		cJSON_Delete(j);
		SET(s->screens[0].mode_3d.driver, "leia-sr");
		s->screens[0].mode_3d.valid = false;
		CHECK_FALSE(u_status_mode_3d_driver_differs(&s->screens[0]));
	}
	SECTION("long plug-in ids are cut, never overflow")
	{
		SET(s->screens[0].claim.plugin_id, "a-very-long-display-driver-id-31c");
		SET(s->screens[0].mode_3d.driver, "another-very-long-driver-id-31ch");
		REQUIRE(u_status_mode_3d_note_driver(&s->screens[0]));
		CHECK(std::strlen(s->screens[0].mode_3d.reason) < sizeof(s->screens[0].mode_3d.reason));
	}
}

#if defined(DXR_TEST_HAVE_TARGET_LISTS) && !defined(_WIN32)
#include "target_status_snapshot.h"
#include "util/u_setting.h"

#include <cstdlib>
#include <unistd.h>

namespace {

void
add_choice(
    xrt_status_mode_3d &m, const char *name, const char *label, uint32_t views, uint32_t applies, const char *reason)
{
	xrt_status_mode_3d_choice &c = m.choices[m.choice_count++];
	SET(c.name, name);
	SET(c.label, label);
	c.views = views;
	c.passthrough = views == 1;
	c.applies = applies;
	SET(c.reason, reason);
}

//! sim-display's table as a running instance reports it: Quad declared only at start-up.
void
sim_mode_3d(xrt_status_mode_3d &m)
{
	std::memset(&m, 0, sizeof(m));
	m.valid = true;
	SET(m.active, "same_as_2d");
	m.source = XRT_STATUS_MODE_3D_SOURCE_DRIVER_DEFAULT;
	add_choice(m, "same_as_2d", "Same as 2D", 1, XRT_STATUS_MODE_3D_APPLIES_NOW, "");
	add_choice(m, "anaglyph", "Anaglyph", 2, XRT_STATUS_MODE_3D_APPLIES_NOW, "");
	add_choice(m, "cropped_sbs", "Cropped SBS", 2, XRT_STATUS_MODE_3D_APPLIES_NOW, "");
	add_choice(m, "quad", "Quad", 4, XRT_STATUS_MODE_3D_APPLIES_RESTART_APP,
	           "declared only at start-up: restart the app to apply");
}

std::string
mode_3d_json(const xrt_status_mode_3d &m)
{
	cJSON *j = u_status_mode_3d_to_cjson(&m);
	char *txt = cJSON_PrintUnformatted(j);
	std::string out = txt != nullptr ? txt : "";
	cJSON_free(txt);
	cJSON_Delete(j);
	return out;
}

} // namespace

TEST_CASE("status: a stored 3D mode the driver does not offer is reported, with a reason, until reset",
          "[status][mode_3d]")
{
	char tmpl[] = "/tmp/dxr_m3d_cfg_XXXXXX";
	REQUIRE(mkdtemp(tmpl) != nullptr);
	const char *old_cfg = getenv("XDG_CONFIG_HOME");
	const std::string saved = old_cfg != nullptr ? old_cfg : "";
	setenv("XDG_CONFIG_HOME", tmpl, 1);

	auto s = healthy();
	const std::string key = s->screens[0].key;
	sim_mode_3d(s->screens[0].mode_3d);

	SECTION("not in the table: setting = the stored name, source = its store, active = what is drawn")
	{
		REQUIRE(u_setting_user_set_mode_3d_for_screen(key.c_str(), "lenticular_9"));
		target_status_snapshot_fill_mode_3d_setting(s.get());
		const xrt_status_mode_3d &m = s->screens[0].mode_3d;
		CHECK(std::string(m.setting) == "lenticular_9");
		CHECK(m.source == XRT_STATUS_MODE_3D_SOURCE_USER);
		CHECK(std::string(m.active) == "same_as_2d");
		CHECK(std::string(m.reason) ==
		      "the chosen 3D mode 'lenticular_9' is not offered by this display's driver; the default is used");
		std::printf("stale-choice mode_3d JSON: %s\n", mode_3d_json(m).c_str());

		// Headless says the same: a name it cannot find is never shown as active.
		sim_mode_3d(s->screens[0].mode_3d);
		s->source = XRT_STATUS_SOURCE_HEADLESS;
		target_status_snapshot_fill_mode_3d_setting(s.get());
		CHECK(std::string(m.active) == "same_as_2d");
		CHECK(m.reason[0] != '\0');

		// `dp mode reset --screen <key>` (the same writer, NULL) clears it.
		REQUIRE(u_setting_user_set_mode_3d_for_screen(key.c_str(), nullptr));
		sim_mode_3d(s->screens[0].mode_3d);
		target_status_snapshot_fill_mode_3d_setting(s.get());
		CHECK(m.setting[0] == '\0');
		CHECK(m.reason[0] == '\0');
		CHECK(m.source == XRT_STATUS_MODE_3D_SOURCE_DRIVER_DEFAULT);
	}
	SECTION("a restart_app choice is chosen and waiting: setting != active, no top-level reason")
	{
		REQUIRE(u_setting_user_set_mode_3d_for_screen(key.c_str(), "quad"));
		target_status_snapshot_fill_mode_3d_setting(s.get());
		const xrt_status_mode_3d &m = s->screens[0].mode_3d;
		CHECK(std::string(m.setting) == "quad");
		CHECK(m.source == XRT_STATUS_MODE_3D_SOURCE_USER);
		CHECK(std::string(m.active) == "same_as_2d");
		CHECK(m.reason[0] == '\0');
		CHECK(m.choices[3].applies == XRT_STATUS_MODE_3D_APPLIES_RESTART_APP);
		std::printf("restart_app-waiting mode_3d JSON: %s\n", mode_3d_json(m).c_str());
	}
	SECTION("a live choice, headless: shown active, its row `now` with no reason")
	{
		REQUIRE(u_setting_user_set_mode_3d_for_screen(key.c_str(), "anaglyph"));
		s->source = XRT_STATUS_SOURCE_HEADLESS;
		target_status_snapshot_fill_mode_3d_setting(s.get());
		const xrt_status_mode_3d &m = s->screens[0].mode_3d;
		CHECK(std::string(m.active) == "anaglyph");
		CHECK(m.choices[1].applies == XRT_STATUS_MODE_3D_APPLIES_NOW);
		CHECK(m.choices[1].reason[0] == '\0');
	}

	(void)u_setting_user_clear_mode_3d_per_screen();
	const std::string f = std::string(tmpl) + "/displayxr/settings.json";
	std::remove(f.c_str());
	rmdir((std::string(tmpl) + "/displayxr").c_str());
	rmdir(tmpl);
	if (old_cfg != nullptr) {
		setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
	} else {
		unsetenv("XDG_CONFIG_HOME");
	}
}

TEST_CASE("status: the active 3D mode's row reads `now` with no reason", "[status][mode_3d]")
{
	// An instance that started with the start-up-only Quad chosen: Quad is
	// declared and drawn. Its row is not "restart the app to apply".
	xrt_status_mode_3d m;
	sim_mode_3d(m);
	SET(m.active, "quad");
	m.choices[3].applies = XRT_STATUS_MODE_3D_APPLIES_NOW; // declared for this instance
	m.choices[3].reason[0] = '\0';
	SET(m.choices[3].reason, "4 views: stereo apps show 2D");
	u_status_mode_3d_mark_active(&m);
	CHECK(m.choices[3].applies == XRT_STATUS_MODE_3D_APPLIES_NOW);
	CHECK(m.choices[3].reason[0] == '\0');
	// The others are live switches away from it.
	for (uint32_t k = 0; k < 3; k++) {
		CHECK(m.choices[k].applies == XRT_STATUS_MODE_3D_APPLIES_NOW);
		CHECK(m.choices[k].reason[0] == '\0');
	}
	// Moving `active` again restores the caveat on the row left behind.
	SET(m.active, "anaglyph");
	u_status_mode_3d_mark_active(&m);
	CHECK(std::string(m.choices[3].reason) == "4 views: stereo apps show 2D");
	CHECK(m.choices[1].reason[0] == '\0');
}
TEST_CASE("status: a stored choice on a primary woven by another driver keeps the head's table, with the note",
          "[status][mode_3d][driver]")
{
	char tmpl[] = "/tmp/dxr_m3d_drv_XXXXXX";
	REQUIRE(mkdtemp(tmpl) != nullptr);
	const char *old_cfg = getenv("XDG_CONFIG_HOME");
	const std::string saved = old_cfg != nullptr ? old_cfg : "";
	setenv("XDG_CONFIG_HOME", tmpl, 1);

	auto s = healthy();
	const std::string key = s->screens[0].key;
	leia_head_mode_3d(s->screens[0].mode_3d);
	force_primary_to_sim(*s);

	SECTION("nothing stored: the note alone")
	{
		target_status_snapshot_fill_mode_3d_setting(s.get());
		const xrt_status_mode_3d &m = s->screens[0].mode_3d;
		CHECK(m.setting[0] == '\0');
		CHECK(std::string(m.active) == "leiasr");
		CHECK(std::string(m.reason).rfind("its 3D modes are those of the head display driver 'leia-sr'", 0) ==
		      0);
		std::printf("primary-woven-by-sim mode_3d JSON: %s\n", mode_3d_json(m).c_str());
	}
	SECTION("a sim-display name stored for the screen: kept, not drawn, and the note says why")
	{
		REQUIRE(u_setting_user_set_mode_3d_for_screen(key.c_str(), "anaglyph"));
		s->source = XRT_STATUS_SOURCE_HEADLESS;
		target_status_snapshot_fill_mode_3d_setting(s.get());
		const xrt_status_mode_3d &m = s->screens[0].mode_3d;
		CHECK(std::string(m.setting) == "anaglyph");
		CHECK(m.source == XRT_STATUS_MODE_3D_SOURCE_USER);
		CHECK(std::string(m.active) == "leiasr"); // never shown as drawn
		CHECK(std::string(m.reason).rfind("its 3D modes are those of", 0) == 0);
	}
	SECTION("the head's own entry stored: honoured, the note stays")
	{
		REQUIRE(u_setting_user_set_mode_3d_for_screen(key.c_str(), "leiasr"));
		target_status_snapshot_fill_mode_3d_setting(s.get());
		const xrt_status_mode_3d &m = s->screens[0].mode_3d;
		CHECK(std::string(m.setting) == "leiasr");
		CHECK(std::string(m.active) == "leiasr");
		CHECK(std::string(m.reason).rfind("its 3D modes are those of", 0) == 0);
	}

	(void)u_setting_user_clear_mode_3d_per_screen();
	const std::string f = std::string(tmpl) + "/displayxr/settings.json";
	std::remove(f.c_str());
	rmdir((std::string(tmpl) + "/displayxr").c_str());
	rmdir(tmpl);
	if (old_cfg != nullptr) {
		setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
	} else {
		unsetenv("XDG_CONFIG_HOME");
	}
}
#endif
