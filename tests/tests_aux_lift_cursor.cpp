// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-046 Amendment 1: the depth-aware cursor on lifted content
 *         (u_lift_cursor) — relief decode, disparity, per-view projection.
 */

#include "util/u_lift_cursor.h"

#include "catch_amalgamated.hpp"

#include <cmath>

using Catch::Approx;

namespace {

constexpr float kB = 0.063f; // eye baseline
constexpr float kZ = 0.60f;  // viewing distance

// SBS pair, rect-relative, viewer 4 cm right and 2 cm up of the rect centre.
const float kEyes[6] = {0.04f - kB / 2, 0.02f, kZ, 0.04f + kB / 2, 0.02f, kZ};

} // namespace

TEST_CASE("lift cursor: relief decode follows the encoding", "[lift_cursor]")
{
	float z = 0.0f;
	// INVERSE, 0..255 relative samples: decoded = s / 255 = 1 / depth.
	REQUIRE(u_lift_cursor_relief_z(255.0f, true, 1.0f / 255.0f, 0.0f, 0.08f, -0.04f, &z));
	CHECK(z == Approx(0.04f)); // nearest plane: 40 mm in front
	REQUIRE(u_lift_cursor_relief_z(0.5f * 255.0f, true, 1.0f / 255.0f, 0.0f, 0.08f, -0.04f, &z));
	CHECK(z == Approx(0.0f).margin(1e-6f)); // mid-depth on the glass
	// LINEAR metres: 1 / depth.
	REQUIRE(u_lift_cursor_relief_z(2.0f, false, 1.0f, 0.0f, 0.1f, -0.03f, &z));
	CHECK(z == Approx(0.02f));
	// No depth: zero, negative, NaN.
	CHECK_FALSE(u_lift_cursor_relief_z(0.0f, true, 1.0f / 255.0f, 0.0f, 0.08f, -0.04f, &z));
	CHECK_FALSE(u_lift_cursor_relief_z(-1.0f, false, 1.0f, 0.0f, 0.1f, 0.0f, &z));
	CHECK_FALSE(u_lift_cursor_relief_z(NAN, false, 1.0f, 0.0f, 0.1f, 0.0f, &z));
	// value_scale 0 reads as 1 (the DP contract's rule).
	REQUIRE(u_lift_cursor_relief_z(2.0f, false, 0.0f, 0.0f, 0.1f, 0.0f, &z));
	CHECK(z == Approx(0.05f));
}

TEST_CASE("lift cursor: the nearest sample is the largest z", "[lift_cursor]")
{
	const float s[5] = {10.0f, 0.0f, 200.0f, NAN, 120.0f};
	float z = 0.0f;
	REQUIRE(u_lift_cursor_nearest_z(s, 5, true, 1.0f / 255.0f, 0.0f, 0.08f, -0.04f, &z));
	CHECK(z == Approx(0.08f * 200.0f / 255.0f - 0.04f));
	const float none[2] = {0.0f, NAN};
	CHECK_FALSE(u_lift_cursor_nearest_z(none, 2, true, 1.0f / 255.0f, 0.0f, 0.08f, -0.04f, &z));
}

TEST_CASE("lift cursor: disparity of a height in front of the screen", "[lift_cursor]")
{
	float e[3];
	REQUIRE(u_lift_cursor_eye_midpoint(kEyes, 2, e));
	CHECK(e[0] == Approx(0.04f));
	CHECK(e[2] == Approx(kZ));
	float d = 1.0f;
	REQUIRE(u_lift_cursor_disparity_of_z(e, 0.0f, &d));
	CHECK(d == Approx(0.0f).margin(1e-7f));
	REQUIRE(u_lift_cursor_disparity_of_z(e, 0.06f, &d)); // t = 0.9
	CHECK(d == Approx(1.0f - 1.0f / 0.9f));
	REQUIRE(u_lift_cursor_disparity_of_z(e, -0.06f, &d)); // behind: uncrossed
	CHECK(d > 0.0f);
	CHECK_FALSE(u_lift_cursor_disparity_of_z(e, kZ, &d)); // at the eye
}

TEST_CASE("lift cursor: SBS projections straddle the cursor by baseline x disparity", "[lift_cursor]")
{
	float e[3];
	REQUIRE(u_lift_cursor_eye_midpoint(kEyes, 2, e));
	const float sx = 0.02f, sy = -0.01f;
	float xy[4];

	// On the screen plane both views draw it at S.
	REQUIRE(u_lift_cursor_project(e, sx, sy, 0.0f, kEyes, 2, xy));
	CHECK(xy[0] == Approx(sx));
	CHECK(xy[2] == Approx(sx));
	CHECK(xy[1] == Approx(sy));

	// In front (crossed): the left view draws it right of S, the right view
	// left of S, each by baseline/2 * |d|; same row in both.
	const float d = -0.1f;
	REQUIRE(u_lift_cursor_project(e, sx, sy, d, kEyes, 2, xy));
	CHECK(xy[0] - sx == Approx(-kB / 2 * d));
	CHECK(xy[2] - sx == Approx(+kB / 2 * d));
	CHECK(xy[1] == Approx(sy));
	CHECK(xy[3] == Approx(sy));
}

TEST_CASE("lift cursor: the image on the glass does not swim with the head", "[lift_cursor]")
{
	// ADR-046 Consequences: screen anchoring. The same cursor point and
	// disparity give the same per-eye glass positions from any head pose.
	const float sx = -0.05f, sy = 0.03f, d = -0.08f;
	const float poses[3][6] = {
	    {-kB / 2, 0.0f, kZ, kB / 2, 0.0f, kZ},
	    {0.07f - kB / 2, -0.03f, 0.55f, 0.07f + kB / 2, -0.03f, 0.55f},
	    {-0.09f - kB / 2, 0.05f, 0.70f, -0.09f + kB / 2, 0.05f, 0.70f},
	};
	for (const auto &eyes : poses) {
		float e[3], xy[4];
		REQUIRE(u_lift_cursor_eye_midpoint(eyes, 2, e));
		REQUIRE(u_lift_cursor_project(e, sx, sy, d, eyes, 2, xy));
		CHECK(xy[0] == Approx(sx - kB / 2 * d));
		CHECK(xy[2] == Approx(sx + kB / 2 * d));
		CHECK(xy[1] == Approx(sy));
	}
}

TEST_CASE("lift cursor: N views spread in order, centred on the cursor", "[lift_cursor]")
{
	// Four views on a line, 21 mm apart, centred on the viewer.
	float vps[12];
	for (int i = 0; i < 4; i++) {
		vps[3 * i + 0] = (-1.5f + (float)i) * 0.021f;
		vps[3 * i + 1] = 0.0f;
		vps[3 * i + 2] = kZ;
	}
	float e[3], xy[8];
	REQUIRE(u_lift_cursor_eye_midpoint(vps, 4, e));
	REQUIRE(u_lift_cursor_project(e, 0.0f, 0.0f, -0.1f, vps, 4, xy));
	// Crossed: the leftmost view draws it rightmost, evenly spaced.
	for (int i = 0; i < 3; i++) {
		CHECK(xy[2 * i] > xy[2 * (i + 1)]);
		CHECK(xy[2 * i] - xy[2 * (i + 1)] == Approx(0.021f * 0.1f));
	}
	CHECK((xy[0] + xy[6]) * 0.5f == Approx(0.0f).margin(1e-7f));
}

TEST_CASE("lift cursor: degenerate input draws nothing", "[lift_cursor]")
{
	float e[3] = {0.0f, 0.0f, kZ}, xy[4];
	CHECK_FALSE(u_lift_cursor_project(e, 0.0f, 0.0f, 1.0f, kEyes, 2, xy));  // at the eye
	CHECK_FALSE(u_lift_cursor_project(e, 0.0f, 0.0f, NAN, kEyes, 2, xy));
	CHECK_FALSE(u_lift_cursor_project(e, 0.0f, 0.0f, -0.1f, kEyes, 0, xy)); // no views
	const float behind[6] = {0.0f, 0.0f, -0.1f, 0.0f, 0.0f, -0.1f};
	CHECK_FALSE(u_lift_cursor_eye_midpoint(behind, 2, e));
}
