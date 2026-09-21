// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for u_grip_surface — the `grip` → `grip_surface` transform
 *         (part of #1633).
 *
 * The bounds encoded here are the ones the Khronos CTS checks in
 * `GripSurface-objective` when it locates the grip-surface space IN the grip
 * space (`test_XR_EXT_palm_pose.cpp`):
 *
 *   left  : REQUIRE(x <= 1e-4)   CHECK(x >= -0.10)  CHECK(y >= -0.10)
 *   right : REQUIRE(x >= -1e-4)  CHECK(x <=  0.10)  CHECK(y <=  0.10)
 *   both  : angle(+X_surface, +X_grip) < 10 deg
 *           |angle(+Z_surface, +Z_grip)| < 90 deg
 *
 * They live here so an edit that would fail the CTS — which needs a display,
 * a session and twenty seconds — fails on the host in milliseconds instead.
 * No device, no driver, no clock: the transform is pure geometry.
 */

#include "math/m_api.h"
#include "math/m_mathinclude.h"
#include "util/u_grip_surface.h"

#include "catch_amalgamated.hpp"

#include <cmath>

namespace {

constexpr xrt_pose kIdentity = XRT_POSE_IDENTITY;
constexpr xrt_vec3 kAxisX = XRT_VEC3_UNIT_X;
constexpr xrt_vec3 kAxisY = XRT_VEC3_UNIT_Y;
constexpr xrt_vec3 kAxisZ = XRT_VEC3_UNIT_Z;

//! The CTS's own numbers.
constexpr float kEpsilon = 0.0001f;
constexpr float kMaxOffsetM = 0.10f;

//! Angle between two vectors, in degrees. Mirrors the CTS's angleDeg().
double
angle_deg(const xrt_vec3 &a, const xrt_vec3 &b)
{
	const double dot = (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z;
	const double la = std::sqrt((double)a.x * a.x + (double)a.y * a.y + (double)a.z * a.z);
	const double lb = std::sqrt((double)b.x * b.x + (double)b.y * b.y + (double)b.z * b.z);
	double c = dot / (la * lb);
	c = c > 1.0 ? 1.0 : (c < -1.0 ? -1.0 : c);
	return std::acos(c) * 180.0 / M_PI;
}

xrt_space_relation
relation(const xrt_pose &pose, uint32_t flags)
{
	xrt_space_relation r{};
	r.pose = pose;
	r.relation_flags = (enum xrt_space_relation_flags)flags;
	return r;
}

constexpr uint32_t kTracked =
    (uint32_t)XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | (uint32_t)XRT_SPACE_RELATION_POSITION_VALID_BIT |
    (uint32_t)XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | (uint32_t)XRT_SPACE_RELATION_POSITION_TRACKED_BIT;

//! The surface pose expressed IN the grip frame — what the CTS measures.
xrt_pose
surface_in_grip(bool is_left, const xrt_pose &grip)
{
	const xrt_space_relation in = relation(grip, kTracked);

	xrt_space_relation out{};
	u_grip_surface_from_grip(is_left, &in, &out);

	// out.pose = grip o offset  =>  offset = grip^-1 o out.pose
	xrt_pose inv_grip{};
	math_pose_invert(&grip, &inv_grip);

	xrt_pose in_grip{};
	math_pose_transform(&inv_grip, &out.pose, &in_grip);
	return in_grip;
}

xrt_quat
quat_about(const xrt_vec3 &axis, float rads)
{
	xrt_quat q{};
	math_quat_from_angle_vector(rads, &axis, &q);
	return q;
}

Catch::Approx
near_zero()
{
	return Catch::Approx(0.0f).margin(1e-6);
}

} // namespace


TEST_CASE("u_grip_surface: the offset is on the palm side, per hand")
{
	xrt_pose left{};
	xrt_pose right{};
	u_grip_surface_offset(true, &left);
	u_grip_surface_offset(false, &right);

	// grip +X is the palm normal: away from the palm on the left hand,
	// into it on the right. The surface therefore sits at -X / +X.
	CHECK(left.position.x == Catch::Approx(-U_GRIP_SURFACE_OFFSET_M));
	CHECK(right.position.x == Catch::Approx(U_GRIP_SURFACE_OFFSET_M));

	// Nothing off-axis, and no rotation for a device with no handle.
	CHECK(left.position.y == near_zero());
	CHECK(left.position.z == near_zero());
	CHECK(right.position.y == near_zero());
	CHECK(right.position.z == near_zero());
	CHECK(left.orientation.w == Catch::Approx(1.0f));
	CHECK(right.orientation.w == Catch::Approx(1.0f));

	// The two hands mirror.
	CHECK(left.position.x == Catch::Approx(-right.position.x));

	// Well inside the CTS's 10 cm sanity bound, and not zero — a palm pose
	// that coincided with grip would be a claim, not a derivation.
	CHECK(std::abs(left.position.x) < kMaxOffsetM);
	CHECK(std::abs(left.position.x) > kEpsilon);
}

TEST_CASE("u_grip_surface: identity grip yields the documented offset")
{
	const xrt_space_relation in = relation(kIdentity, kTracked);

	for (bool is_left : {true, false}) {
		xrt_space_relation out{};
		u_grip_surface_from_grip(is_left, &in, &out);

		xrt_pose expected{};
		u_grip_surface_offset(is_left, &expected);

		CHECK(out.pose.position.x == Catch::Approx(expected.position.x));
		CHECK(out.pose.position.y == near_zero());
		CHECK(out.pose.position.z == near_zero());
		CHECK(out.pose.orientation.w == Catch::Approx(1.0f));
	}
}

TEST_CASE("u_grip_surface: CTS position bounds hold for any grip pose")
{
	// A spread of grips, including rotations, so the "in grip space"
	// measurement is exercised rather than a lucky identity.
	xrt_pose grips[4]{};
	grips[0] = kIdentity;
	grips[1].orientation = quat_about(kAxisY, 1.2f);
	grips[1].position = {0.3f, 1.4f, -0.7f};
	grips[2].orientation = quat_about(kAxisX, -0.9f);
	grips[2].position = {-2.0f, 0.1f, 3.5f};
	grips[3].orientation = quat_about(kAxisZ, 2.7f);
	grips[3].position = {0.0f, -1.0f, 0.0f};

	for (const xrt_pose &grip : grips) {
		const xrt_pose l = surface_in_grip(true, grip);
		const xrt_pose r = surface_in_grip(false, grip);

		// Left: on the -X side of grip, within 10 cm in x and y.
		CHECK(l.position.x <= kEpsilon);
		CHECK(l.position.x >= -kMaxOffsetM);
		CHECK(l.position.y >= -kMaxOffsetM);

		// Right: on the +X side of grip, within 10 cm in x and y.
		CHECK(r.position.x >= -kEpsilon);
		CHECK(r.position.x <= kMaxOffsetM);
		CHECK(r.position.y <= kMaxOffsetM);
	}
}

TEST_CASE("u_grip_surface: CTS orientation bounds hold")
{
	xrt_pose grip{};
	grip.orientation = quat_about(kAxisY, 0.6f);
	grip.position = {1.0f, 2.0f, 3.0f};

	for (bool is_left : {true, false}) {
		const xrt_pose in_grip = surface_in_grip(is_left, grip);

		xrt_vec3 surface_x{};
		math_quat_rotate_vec3(&in_grip.orientation, &kAxisX, &surface_x);
		CHECK(angle_deg(kAxisX, surface_x) < 10.0);

		xrt_vec3 surface_z{};
		math_quat_rotate_vec3(&in_grip.orientation, &kAxisZ, &surface_z);
		CHECK(std::abs(angle_deg(kAxisZ, surface_z)) < 90.0);
	}
}

TEST_CASE("u_grip_surface: relation flags are mirrored, never invented")
{
	SECTION("a fully tracked grip yields a fully tracked surface")
	{
		const xrt_space_relation in = relation(kIdentity, kTracked);

		xrt_space_relation out{};
		u_grip_surface_from_grip(true, &in, &out);
		CHECK((uint32_t)out.relation_flags == kTracked);
	}

	SECTION("an untracked grip stays untracked — no locatability is invented")
	{
		const xrt_space_relation in = relation(kIdentity, (uint32_t)XRT_SPACE_RELATION_BITMASK_NONE);

		xrt_space_relation out{};
		u_grip_surface_from_grip(false, &in, &out);
		CHECK((uint32_t)out.relation_flags == (uint32_t)XRT_SPACE_RELATION_BITMASK_NONE);
	}

	SECTION("orientation-only stays orientation-only")
	{
		const xrt_space_relation in = relation(kIdentity, (uint32_t)XRT_SPACE_RELATION_ORIENTATION_VALID_BIT);

		xrt_space_relation out{};
		u_grip_surface_from_grip(false, &in, &out);
		CHECK((uint32_t)out.relation_flags == (uint32_t)XRT_SPACE_RELATION_ORIENTATION_VALID_BIT);
	}
}

TEST_CASE("u_grip_surface: velocity gains the lever arm only when both bits are valid")
{
	const float r = U_GRIP_SURFACE_OFFSET_M;

	SECTION("both valid: v + omega x r")
	{
		xrt_space_relation in =
		    relation(kIdentity, (uint32_t)XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT |
		                            (uint32_t)XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
		in.linear_velocity = {1.0f, 0.0f, 0.0f};
		in.angular_velocity = {0.0f, 0.0f, 2.0f}; // spin about +Z

		xrt_space_relation out{};
		u_grip_surface_from_grip(false, &in, &out); // right hand: r = (+r, 0, 0)

		// omega x r = (0,0,2) x (r,0,0) = (0, 2r, 0)
		CHECK(out.linear_velocity.x == Catch::Approx(1.0f));
		CHECK(out.linear_velocity.y == Catch::Approx(2.0f * r));
		CHECK(out.linear_velocity.z == near_zero());
	}

	SECTION("angular missing: linear velocity passes through untouched")
	{
		xrt_space_relation in = relation(kIdentity, (uint32_t)XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
		in.linear_velocity = {1.0f, 0.0f, 0.0f};
		in.angular_velocity = {0.0f, 0.0f, 2.0f};

		xrt_space_relation out{};
		u_grip_surface_from_grip(false, &in, &out);
		CHECK(out.linear_velocity.x == Catch::Approx(1.0f));
		CHECK(out.linear_velocity.y == near_zero());
		CHECK(out.linear_velocity.z == near_zero());
	}
}

TEST_CASE("u_grip_surface: aliasing and NULL")
{
	SECTION("in and out may be the same relation")
	{
		xrt_space_relation both = relation(kIdentity, (uint32_t)XRT_SPACE_RELATION_POSITION_VALID_BIT);
		both.pose.position = {0.5f, 0.5f, 0.5f};

		u_grip_surface_from_grip(true, &both, &both);
		CHECK(both.pose.position.x == Catch::Approx(0.5f - U_GRIP_SURFACE_OFFSET_M));
		CHECK((uint32_t)both.relation_flags == (uint32_t)XRT_SPACE_RELATION_POSITION_VALID_BIT);
	}

	SECTION("NULL grip yields a zero relation, not garbage")
	{
		xrt_space_relation out = relation(kIdentity, (uint32_t)XRT_SPACE_RELATION_POSITION_VALID_BIT);
		out.pose.position = {9.0f, 9.0f, 9.0f};

		u_grip_surface_from_grip(true, NULL, &out);
		CHECK((uint32_t)out.relation_flags == (uint32_t)XRT_SPACE_RELATION_BITMASK_NONE);
		CHECK(out.pose.position.x == near_zero());
	}

	SECTION("NULL outputs do not crash")
	{
		const xrt_space_relation in = relation(kIdentity, kTracked);
		u_grip_surface_from_grip(true, &in, NULL);
		u_grip_surface_offset(true, NULL);
		SUCCEED();
	}
}
