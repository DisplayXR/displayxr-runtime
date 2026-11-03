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

#include "math/m_api.h"
#include "math/m_space.h"

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

TEST_CASE("palm and wrist radii are half their thickness, not half their width", "[ultraleap_map]")
{
	ul_map phys;
	{
		ul_map_config cfg;
		ul_map_config_defaults(&cfg);
		cfg.mode = UL_MAPPING_PHYSICAL;
		ul_map_resolve(&cfg, kH156, &phys);
	}

	// Typical adult hand: palm 85 mm wide, wrist 60 mm wide.
	const float palm_r = ul_map_palm_radius_from_width_mm(&phys, 85.f);
	const float wrist_r = ul_map_wrist_radius_from_width_mm(&phys, 60.f);
	CHECK(palm_r == Catch::Approx(0.01275f).margin(1e-6f));
	CHECK(wrist_r == Catch::Approx(0.018f).margin(1e-6f));

	// Physically sensible: 1-2 cm, and well under the old width/2 (42.5 / 30 mm).
	CHECK(palm_r > 0.010f);
	CHECK(palm_r < 0.015f);
	CHECK(wrist_r > 0.010f);
	CHECK(wrist_r < 0.020f);
	CHECK(palm_r < 0.5f * ul_map_radius_from_width_mm(&phys, 85.f));
	CHECK(wrist_r < 0.7f * ul_map_radius_from_width_mm(&phys, 60.f));

	// The palm is not thicker than the wrist; both are thicker than a finger (~16 mm wide -> 8 mm).
	CHECK(palm_r < wrist_r);
	CHECK(palm_r > ul_map_radius_from_width_mm(&phys, 16.f));

	// A missing width (0 / negative) uses the typical hand, never a zero or negative radius.
	CHECK(ul_map_palm_radius_from_width_mm(&phys, 0.f) == Catch::Approx(palm_r).margin(1e-7f));
	CHECK(ul_map_wrist_radius_from_width_mm(&phys, -1.f) == Catch::Approx(wrist_r).margin(1e-7f));

	// Relative mapping: scaled by the same gain as every other radius and joint offset.
	const ul_map rel = resolve_defaults(kH156);
	CHECK(ul_map_palm_radius_from_width_mm(&rel, 85.f) == Catch::Approx(palm_r * rel.scale).margin(1e-7f));
	CHECK(ul_map_wrist_radius_from_width_mm(&rel, 60.f) == Catch::Approx(wrist_r * rel.scale).margin(1e-7f));
}

TEST_CASE("relative mapping: the gain is one uniform scale, so hand contact is preserved", "[ultraleap_map]")
{
	// Hand-internal distances, distances between two hands and radii all scale by the
	// same factor: two fingertips touching in Leap space (centres 2 tip radii apart)
	// stay exactly touching after the mapping, at any anchor. Gaining only the
	// placement while keeping fingers metric would make them overlap (gain < 1)
	// or separate (gain > 1).
	for (float h : {kH156, kH32}) {
		const ul_map m = resolve_defaults(h);
		const float tip_width_mm = 16.f; // a typical index distal bone
		const xrt_vec3 a = map_pos(m, {-8.f, 230.f, 10.f});
		const xrt_vec3 b = map_pos(m, {8.f, 230.f, 10.f}); // centres 16 mm apart = 2 radii
		const float r = ul_map_radius_from_width_mm(&m, tip_width_mm);
		const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
		const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
		CHECK(d == Catch::Approx(0.016f * m.scale).margin(1e-6f));
		CHECK(d == Catch::Approx(2.f * r).margin(1e-6f));
	}
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


/*
 *
 * Hand frame: joints are relative to hand_pose, which the state tracker
 * composes on (oxr_session.c, T_xdev_hand). A field run on a Windows 3D panel
 * found the palm applied twice: two touching index tips came back 7.7 cm apart.
 *
 */

namespace {

constexpr enum xrt_space_relation_flags kValid = (enum xrt_space_relation_flags)(
    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
    XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

xrt_quat
axis_angle(xrt_vec3 axis, float rad)
{
	const float n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
	const xrt_vec3 unit = {axis.x / n, axis.y / n, axis.z / n};
	xrt_quat q;
	math_quat_from_angle_vector(rad, &unit, &q);
	return q;
}

//! A display-plane hand the way ul_process_hand fills it: palm somewhere off the
//! display centre and turned, every joint at its own display-plane pose.
xrt_hand_joint_set
display_plane_hand(xrt_vec3 palm_pos, xrt_quat palm_rot)
{
	xrt_hand_joint_set set = {};
	for (int i = 0; i < XRT_HAND_JOINT_COUNT; i++) {
		xrt_hand_joint_value &j = set.values.hand_joint_set_default[i];
		// Hand-local layout: joints spread along -Z (fingers) and X (across the hand).
		const xrt_pose local = {axis_angle({1.f, 0.f, 0.f}, 0.05f * (float)i),
		                        {0.004f * (float)(i % 5) - 0.008f, 0.001f * (float)i, -0.004f * (float)i}};
		const xrt_pose palm = {palm_rot, palm_pos};
		math_pose_transform(&palm, &local, &j.relation.pose);
		j.relation.relation_flags = kValid;
		j.radius = 0.002f + 0.0001f * (float)i;
	}
	xrt_hand_joint_value &p = set.values.hand_joint_set_default[XRT_HAND_JOINT_PALM];
	p.relation.pose = {palm_rot, palm_pos};
	return set;
}

//! What the state tracker does per joint: hand_pose then the joint (device = identity here).
xrt_pose
compose(const xrt_hand_joint_set &set, int joint)
{
	xrt_space_relation out;
	xrt_relation_chain chain = {};
	m_relation_chain_push_relation(&chain, &set.values.hand_joint_set_default[joint].relation);
	m_relation_chain_push_relation(&chain, &set.hand_pose);
	m_relation_chain_resolve(&chain, &out);
	return out.pose;
}

float
dist(xrt_vec3 a, xrt_vec3 b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

} // namespace

TEST_CASE("hand frame: palm becomes the root, composing it back reproduces the display-plane joints", "[ultraleap_map]")
{
	const xrt_vec3 palm_pos = {0.06f, -0.02f, 0.05f};
	const xrt_quat palm_rot = axis_angle({0.3f, 1.f, 0.2f}, 0.9f);
	const xrt_hand_joint_set before = display_plane_hand(palm_pos, palm_rot);

	xrt_hand_joint_set set = before;
	ul_map_joints_to_hand_frame(&set);

	// hand_pose = the palm; the palm joint = exactly the identity.
	CHECK(set.hand_pose.relation_flags == kValid);
	CHECK(set.hand_pose.pose.position.x == palm_pos.x);
	CHECK(set.hand_pose.pose.position.y == palm_pos.y);
	CHECK(set.hand_pose.pose.position.z == palm_pos.z);
	CHECK(set.hand_pose.pose.orientation.w == palm_rot.w);
	const xrt_pose &pj = set.values.hand_joint_set_default[XRT_HAND_JOINT_PALM].relation.pose;
	CHECK(pj.position.x == 0.f);
	CHECK(pj.position.y == 0.f);
	CHECK(pj.position.z == 0.f);
	CHECK(pj.orientation.w == 1.f);

	for (int i = 0; i < XRT_HAND_JOINT_COUNT; i++) {
		INFO("joint " << i);
		const xrt_hand_joint_value &b = before.values.hand_joint_set_default[i];
		const xrt_hand_joint_value &a = set.values.hand_joint_set_default[i];
		// Flags and radii untouched.
		CHECK(a.relation.relation_flags == b.relation.relation_flags);
		CHECK(a.radius == b.radius);
		// hand_pose ∘ joint == the display-plane joint (the palm applied once).
		const xrt_pose c = compose(set, i);
		CHECK(c.position.x == Catch::Approx(b.relation.pose.position.x).margin(kEps));
		CHECK(c.position.y == Catch::Approx(b.relation.pose.position.y).margin(kEps));
		CHECK(c.position.z == Catch::Approx(b.relation.pose.position.z).margin(kEps));
		// Same rotation up to sign.
		const float dot =
		    c.orientation.x * b.relation.pose.orientation.x + c.orientation.y * b.relation.pose.orientation.y +
		    c.orientation.z * b.relation.pose.orientation.z + c.orientation.w * b.relation.pose.orientation.w;
		CHECK(std::fabs(dot) == Catch::Approx(1.f).margin(kEps));
	}
}

TEST_CASE("hand frame: two touching fingertips stay touching (the palm-applied-twice regression)", "[ultraleap_map]")
{
	// Left and right hands either side of the display centre, index tips meeting at one point.
	const xrt_vec3 meet = {0.0f, 0.01f, 0.04f};
	xrt_hand_joint_set left = display_plane_hand({-0.05f, 0.f, 0.03f}, axis_angle({0.f, 0.f, 1.f}, -0.4f));
	xrt_hand_joint_set right = display_plane_hand({0.05f, 0.f, 0.03f}, axis_angle({0.f, 0.f, 1.f}, 0.4f));
	left.values.hand_joint_set_default[XRT_HAND_JOINT_INDEX_TIP].relation.pose.position = meet;
	right.values.hand_joint_set_default[XRT_HAND_JOINT_INDEX_TIP].relation.pose.position = meet;

	// The bug: display-plane joints with hand_pose = palm compose the palm twice.
	xrt_hand_joint_set bad_l = left;
	xrt_hand_joint_set bad_r = right;
	bad_l.hand_pose = bad_l.values.hand_joint_set_default[XRT_HAND_JOINT_PALM].relation;
	bad_r.hand_pose = bad_r.values.hand_joint_set_default[XRT_HAND_JOINT_PALM].relation;
	CHECK(dist(compose(bad_l, XRT_HAND_JOINT_INDEX_TIP).position,
	           compose(bad_r, XRT_HAND_JOINT_INDEX_TIP).position) > 0.05f);

	ul_map_joints_to_hand_frame(&left);
	ul_map_joints_to_hand_frame(&right);
	const xrt_vec3 tl = compose(left, XRT_HAND_JOINT_INDEX_TIP).position;
	const xrt_vec3 tr = compose(right, XRT_HAND_JOINT_INDEX_TIP).position;
	CHECK(dist(tl, tr) == Catch::Approx(0.f).margin(kEps));
	CHECK(tl.x == Catch::Approx(meet.x).margin(kEps));
	CHECK(tl.y == Catch::Approx(meet.y).margin(kEps));
	CHECK(tl.z == Catch::Approx(meet.z).margin(kEps));
}

TEST_CASE("hand frame: an empty (never-tracked) set is left alone", "[ultraleap_map]")
{
	xrt_hand_joint_set set = {};
	ul_map_joints_to_hand_frame(&set);
	CHECK(set.hand_pose.relation_flags == 0);
	for (int i = 0; i < XRT_HAND_JOINT_COUNT; i++) {
		const xrt_pose &p = set.values.hand_joint_set_default[i].relation.pose;
		CHECK(p.orientation.w == 0.f); // untouched: not inverted into NaN
		CHECK(!std::isnan(p.position.x));
	}
	ul_map_joints_to_hand_frame(nullptr); // no crash
}
