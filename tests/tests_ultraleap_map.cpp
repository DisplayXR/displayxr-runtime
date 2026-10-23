// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Ultraleap provider's display-plane mapping: relative (display
 *         heights — the default, after the reference viewer app's scene) and
 *         physical (measured mount) modes, radius / velocity gain, rotation
 *         pass-through and the settings parse.
 *
 * `ultraleap_map.c` is pure (no LeapC, no OS, no hub), so this compiles it
 * straight in and needs neither a device nor the Leap SDK. Inputs are fed in
 * LeapC's own units — desktop-frame millimetres, mm/s, bone widths in mm,
 * unit quaternions — exactly as the provider unpacks `LEAP_VECTOR` /
 * `LEAP_QUATERNION`.
 */

#include "catch_amalgamated.hpp"

#include "ultraleap_map.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>


namespace {

constexpr float kEps = 1e-5f;

//! A 15.6" 16:9 panel and a 32" one — the gain is in display heights, so both must feel the same.
constexpr float kH156 = 0.1942f;
constexpr float kH32 = 0.3985f;

//! Mirrors LeapC's LEAP_VECTOR (millimetres, desktop frame: +X right, +Y up, +Z toward the user).
struct leap_vector
{
	float x, y, z;
};

//! Mirrors LeapC's LEAP_QUATERNION.
struct leap_quaternion
{
	float x, y, z, w;
};

xrt_vec3
map_pos(const ul_map &m, leap_vector v)
{
	return ul_map_position_mm(&m, v.x, v.y, v.z);
}

ul_map
resolve_defaults(float h)
{
	ul_map_config cfg;
	ul_map_config_defaults(&cfg);
	ul_map m;
	ul_map_resolve(&cfg, h, &m);
	return m;
}

// A fake host get_setting: the store is a key -> value map, keyed on the provider id.
std::map<std::string, std::string> g_store;

bool
fake_get_setting(const char *id, const char *key, char *buf, uint32_t buf_size)
{
	buf[0] = '\0';
	if (std::strcmp(id, UL_PROVIDER_ID) != 0) {
		return false;
	}
	auto it = g_store.find(key);
	if (it == g_store.end()) {
		return false;
	}
	std::strncpy(buf, it->second.c_str(), buf_size - 1);
	buf[buf_size - 1] = '\0';
	return true;
}

} // namespace


TEST_CASE("relative mapping: a hand 25 cm above the device lands at the display centre", "[ultraleap_map]")
{
	for (float h : {kH156, kH32}) {
		const ul_map m = resolve_defaults(h);
		const xrt_vec3 p = map_pos(m, {0.f, 250.f, 0.f});

		// anchor -0.625 H + 2.5 H/m * 0.25 m = 0: the display centre, on the
		// glass.
		CHECK(std::fabs(p.x) < kEps);
		CHECK(std::fabs(p.y) < 0.005f * h);
		CHECK(std::fabs(p.z) < kEps);
		CHECK(p.y == Catch::Approx(0.f).margin(kEps));
	}
}

TEST_CASE("relative mapping: anchor and gain are in display heights", "[ultraleap_map]")
{
	const ul_map m = resolve_defaults(kH156);

	SECTION("the Leap origin sits on the display plane, 0.625 H below the centre")
	{
		const xrt_vec3 o = map_pos(m, {0.f, 0.f, 0.f});
		CHECK(o.x == Catch::Approx(0.f).margin(kEps));
		CHECK(o.y == Catch::Approx(-0.625f * kH156).margin(kEps));
		CHECK(o.z == Catch::Approx(0.f).margin(kEps));
	}

	SECTION("2.5 H per metre along every axis, Leap axes = display-plane axes")
	{
		const xrt_vec3 o = map_pos(m, {0.f, 250.f, 0.f});
		const xrt_vec3 r = map_pos(m, {100.f, 250.f, 0.f}); // 10 cm right
		const xrt_vec3 u = map_pos(m, {0.f, 350.f, 0.f});   // 10 cm up
		const xrt_vec3 t = map_pos(m, {0.f, 250.f, 100.f}); // 10 cm toward the user
		CHECK(r.x - o.x == Catch::Approx(0.25f * kH156).margin(kEps));
		CHECK(u.y - o.y == Catch::Approx(0.25f * kH156).margin(kEps));
		CHECK(t.z - o.z ==
		      Catch::Approx(0.25f * kH156).margin(kEps)); // +Z = out of the glass, toward the viewer
		CHECK(r.y == Catch::Approx(o.y).margin(kEps));
		CHECK(t.x == Catch::Approx(o.x).margin(kEps));
	}

	SECTION("the same hand motion is the same fraction of the panel on any panel size")
	{
		const ul_map big = resolve_defaults(kH32);
		const leap_vector v = {-60.f, 180.f, 40.f};
		const xrt_vec3 a = map_pos(m, v);
		const xrt_vec3 b = map_pos(big, v);
		CHECK(a.x / kH156 == Catch::Approx(b.x / kH32).margin(kEps));
		CHECK(a.y / kH156 == Catch::Approx(b.y / kH32).margin(kEps));
		CHECK(a.z / kH156 == Catch::Approx(b.z / kH32).margin(kEps));
	}

	SECTION("custom anchor + gain")
	{
		ul_map_config cfg;
		ul_map_config_defaults(&cfg);
		cfg.anchor_h = {0.1f, -0.3f, 0.25f};
		cfg.gain_h_per_m = 2.0f;
		ul_map c;
		ul_map_resolve(&cfg, 0.2f, &c);
		const xrt_vec3 p = map_pos(c, {50.f, 100.f, -50.f});
		CHECK(p.x == Catch::Approx(0.02f + 2.0f * 0.2f * 0.05f).margin(kEps));
		CHECK(p.y == Catch::Approx(-0.06f + 2.0f * 0.2f * 0.10f).margin(kEps));
		CHECK(p.z == Catch::Approx(0.05f - 2.0f * 0.2f * 0.05f).margin(kEps));
	}
}

TEST_CASE("relative mapping: radii and linear velocities scale by the gain, rotations pass through", "[ultraleap_map]")
{
	const ul_map m = resolve_defaults(kH156);
	const float gain_m_per_m = 2.5f * kH156;

	// A 20 mm-wide bone: radius 10 mm in Leap space, scaled to the scene.
	CHECK(ul_map_radius_from_width_mm(&m, 20.f) == Catch::Approx(0.010f * gain_m_per_m).margin(1e-7f));

	// Velocity is scaled, never offset by the anchor.
	const xrt_vec3 v = ul_map_velocity_mm_s(&m, 1000.f, -500.f, 0.f);
	CHECK(v.x == Catch::Approx(1.0f * gain_m_per_m).margin(kEps));
	CHECK(v.y == Catch::Approx(-0.5f * gain_m_per_m).margin(kEps));
	CHECK(v.z == Catch::Approx(0.f).margin(kEps));

	// Leap desktop frame and the display plane share axes: the quaternion is unchanged.
	const float s = std::sqrt(0.5f);
	const leap_quaternion q = {s, 0.f, 0.f, s}; // palm facing down = 90 deg about +X
	const xrt_quat r = ul_map_orientation(q.x, q.y, q.z, q.w);
	CHECK(r.x == q.x);
	CHECK(r.y == q.y);
	CHECK(r.z == q.z);
	CHECK(r.w == q.w);
}

TEST_CASE("relative mapping: unknown display height uses the nominal panel", "[ultraleap_map]")
{
	const ul_map unknown = resolve_defaults(0.f);
	const ul_map nominal = resolve_defaults(UL_MAP_FALLBACK_DISPLAY_HEIGHT_M);
	CHECK(unknown.scale == Catch::Approx(nominal.scale));
	CHECK(unknown.offset_m.y == Catch::Approx(nominal.offset_m.y));
	CHECK(unknown.scale > 0.f);
}

TEST_CASE("physical mapping: 1:1 metres from the measured mount", "[ultraleap_map]")
{
	ul_map_config cfg;
	ul_map_config_defaults(&cfg);
	cfg.mode = UL_MAPPING_PHYSICAL;
	cfg.mount_m = {0.05f, -0.18f, 0.12f};

	for (float h : {0.f, kH156, kH32}) { // the display height must not matter
		ul_map m;
		ul_map_resolve(&cfg, h, &m);
		CHECK(m.scale == 1.0f);

		const xrt_vec3 p = map_pos(m, {-30.f, 250.f, 20.f});
		CHECK(p.x == Catch::Approx(0.05f - 0.030f).margin(kEps));
		CHECK(p.y == Catch::Approx(-0.18f + 0.250f).margin(kEps));
		CHECK(p.z == Catch::Approx(0.12f + 0.020f).margin(kEps));

		CHECK(ul_map_radius_from_width_mm(&m, 20.f) == Catch::Approx(0.010f).margin(1e-7f));
		const xrt_vec3 v = ul_map_velocity_mm_s(&m, 0.f, 300.f, 0.f);
		CHECK(v.y == Catch::Approx(0.3f).margin(kEps));
	}
}

TEST_CASE("settings: defaults, MAPPING, anchors, gain, mount", "[ultraleap_map]")
{
	ul_map_config cfg;

	SECTION("no getter (old runtime) = the compiled defaults")
	{
		ul_map_config_from_settings(nullptr, &cfg);
		CHECK(cfg.mode == UL_MAPPING_RELATIVE);
		CHECK(cfg.anchor_h.x == UL_MAP_DEFAULT_ANCHOR_X_H);
		CHECK(cfg.anchor_h.y == UL_MAP_DEFAULT_ANCHOR_Y_H);
		CHECK(cfg.anchor_h.z == UL_MAP_DEFAULT_ANCHOR_Z_H);
		CHECK(cfg.gain_h_per_m == UL_MAP_DEFAULT_GAIN_H_PER_M);
		CHECK(cfg.mount_m.y == UL_MAP_DEFAULT_MOUNT_Y_M);
	}

	SECTION("every key is read")
	{
		g_store = {{"MAPPING", "Physical"}, {"ANCHOR_X_H", "0.1"},   {"ANCHOR_Y_H", "-0.35"},
		           {"ANCHOR_Z_H", "0.2"},   {"GAIN_H_PER_M", "1.5"}, {"MOUNT_X_M", "0.02"},
		           {"MOUNT_Y_M", "-0.22"},  {"MOUNT_Z_M", "0.3"}};
		ul_map_config_from_settings(fake_get_setting, &cfg);
		CHECK(cfg.mode == UL_MAPPING_PHYSICAL);
		CHECK(cfg.anchor_h.x == Catch::Approx(0.1f));
		CHECK(cfg.anchor_h.y == Catch::Approx(-0.35f));
		CHECK(cfg.anchor_h.z == Catch::Approx(0.2f));
		CHECK(cfg.gain_h_per_m == Catch::Approx(1.5f));
		CHECK(cfg.mount_m.x == Catch::Approx(0.02f));
		CHECK(cfg.mount_m.y == Catch::Approx(-0.22f));
		CHECK(cfg.mount_m.z == Catch::Approx(0.3f));
	}

	SECTION("bad values keep their defaults")
	{
		g_store = {{"MAPPING", "sideways"}, {"ANCHOR_Y_H", "low"}, {"GAIN_H_PER_M", "-2"}};
		ul_map_config_from_settings(fake_get_setting, &cfg);
		CHECK(cfg.mode == UL_MAPPING_RELATIVE);
		CHECK(cfg.anchor_h.y == UL_MAP_DEFAULT_ANCHOR_Y_H);
		CHECK(cfg.gain_h_per_m == UL_MAP_DEFAULT_GAIN_H_PER_M);

		g_store = {{"GAIN_H_PER_M", "0"}};
		ul_map_config_from_settings(fake_get_setting, &cfg);
		CHECK(cfg.gain_h_per_m == UL_MAP_DEFAULT_GAIN_H_PER_M);
	}

	g_store.clear();
}

TEST_CASE("describe_settings declares every key with the compiled defaults", "[ultraleap_map]")
{
	const xrt_component_setting_desc *descs = nullptr;
	const uint32_t n = ul_map_describe_settings(&descs);
	REQUIRE(descs != nullptr);

	std::map<std::string, std::string> declared;
	for (uint32_t i = 0; i < n; i++) {
		declared[descs[i].key] = descs[i].default_value;
	}
	REQUIRE(declared.size() == 8);
	CHECK(declared["MAPPING"] == "relative");
	CHECK(std::stof(declared["ANCHOR_X_H"]) == UL_MAP_DEFAULT_ANCHOR_X_H);
	CHECK(std::stof(declared["ANCHOR_Y_H"]) == UL_MAP_DEFAULT_ANCHOR_Y_H);
	CHECK(std::stof(declared["ANCHOR_Z_H"]) == UL_MAP_DEFAULT_ANCHOR_Z_H);
	CHECK(std::stof(declared["GAIN_H_PER_M"]) == UL_MAP_DEFAULT_GAIN_H_PER_M);
	CHECK(std::stof(declared["MOUNT_X_M"]) == UL_MAP_DEFAULT_MOUNT_X_M);
	CHECK(std::stof(declared["MOUNT_Y_M"]) == UL_MAP_DEFAULT_MOUNT_Y_M);
	CHECK(std::stof(declared["MOUNT_Z_M"]) == UL_MAP_DEFAULT_MOUNT_Z_M);
}
