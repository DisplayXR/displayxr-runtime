// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  u_stereo_comfort: the zones match the web SDK's comfortZone().
 */

#include "util/u_stereo_comfort.h"

#include "catch_amalgamated.hpp"

#include <cmath>

using Catch::Approx;

TEST_CASE("stereo comfort: Shibata at 0.6 m matches the web SDK table", "[aux][stereo_comfort]")
{
	u_stereo_comfort c{};
	u_stereo_comfort_compute(U_STEREO_COMFORT_ZONE_SHIBATA, 0.0f, 0.6f, 1.0f, &c);
	CHECK(c.zone == U_STEREO_COMFORT_ZONE_SHIBATA);
	// authoring-inline-3d.md: 0.57 D / 0.63 D, 21.5 mm / 23.9 mm at 0.60 m
	CHECK(c.front_diopters == Approx(0.5687).margin(0.001));
	CHECK(c.behind_diopters == Approx(0.6324).margin(0.001));
	CHECK(c.front_disparity_m * 1000.0f == Approx(21.5).margin(0.1));
	CHECK(c.behind_disparity_m * 1000.0f == Approx(23.9).margin(0.05));
}

TEST_CASE("stereo comfort: Percival is 1/3 D both sides, 12.6 mm at 0.6 m", "[aux][stereo_comfort]")
{
	u_stereo_comfort c{};
	u_stereo_comfort_compute(U_STEREO_COMFORT_ZONE_PERCIVAL, 0.0f, 0.6f, 1.0f, &c);
	CHECK(c.front_diopters == Approx(1.0 / 3.0));
	CHECK(c.behind_diopters == Approx(1.0 / 3.0));
	CHECK(c.front_disparity_m * 1000.0f == Approx(12.6).margin(0.05));
}

TEST_CASE("stereo comfort: Shibata far limit at or below 0 D leaves behind unbounded", "[aux][stereo_comfort]")
{
	// far = Dv/1.129 - 0.442 <= 0 when V >= 1 / (0.442 * 1.129) ~ 2.0 m
	u_stereo_comfort c{};
	u_stereo_comfort_compute(U_STEREO_COMFORT_ZONE_SHIBATA, 0.0f, 3.0f, 1.0f, &c);
	CHECK(std::isinf(c.behind_diopters));
	CHECK(std::isinf(c.behind_disparity_m));
	CHECK(std::isfinite(c.front_diopters));
}

TEST_CASE("stereo comfort: the user scale lowers both bounds and is clamped to (0, 1]", "[aux][stereo_comfort]")
{
	u_stereo_comfort c{};
	u_stereo_comfort_compute(U_STEREO_COMFORT_ZONE_PERCIVAL, 0.0f, 0.6f, 0.5f, &c);
	CHECK(c.front_diopters == Approx(1.0 / 6.0));
	CHECK(c.user_scale == Approx(0.5));
	u_stereo_comfort_compute(U_STEREO_COMFORT_ZONE_PERCIVAL, 0.0f, 0.6f, 4.0f, &c);
	CHECK(c.user_scale == Approx(1.0));
	CHECK(c.front_diopters == Approx(1.0 / 3.0));
	u_stereo_comfort_compute(U_STEREO_COMFORT_ZONE_PERCIVAL, 0.0f, 0.6f, -1.0f, &c);
	CHECK(c.user_scale == Approx(1.0));
}

TEST_CASE("stereo comfort: bad inputs take the defaults", "[aux][stereo_comfort]")
{
	u_stereo_comfort c{};
	u_stereo_comfort_compute(U_STEREO_COMFORT_ZONE_CUSTOM, -2.0f, NAN, 1.0f, &c);
	CHECK(c.zone == U_STEREO_COMFORT_ZONE_SHIBATA);
	CHECK(c.viewer_m == Approx(0.6));
	u_stereo_comfort_compute(U_STEREO_COMFORT_ZONE_CUSTOM, 0.25f, 0.9f, 1.0f, &c);
	CHECK(c.zone == U_STEREO_COMFORT_ZONE_CUSTOM);
	CHECK(c.front_disparity_m == Approx(0.063 * 0.9 * 0.25));
}

TEST_CASE("stereo comfort: zone setting parser", "[aux][stereo_comfort]")
{
	u_stereo_comfort_zone z = U_STEREO_COMFORT_ZONE_SHIBATA;
	float d = 0.0f;
	CHECK(u_stereo_comfort_parse_zone("Percival", &z, &d));
	CHECK(z == U_STEREO_COMFORT_ZONE_PERCIVAL);
	CHECK(u_stereo_comfort_parse_zone("0.2", &z, &d));
	CHECK(z == U_STEREO_COMFORT_ZONE_CUSTOM);
	CHECK(d == Approx(0.2));
	z = U_STEREO_COMFORT_ZONE_SHIBATA;
	CHECK_FALSE(u_stereo_comfort_parse_zone("-1", &z, &d));
	CHECK_FALSE(u_stereo_comfort_parse_zone("0.2x", &z, &d));
	CHECK_FALSE(u_stereo_comfort_parse_zone(nullptr, &z, &d));
	CHECK(z == U_STEREO_COMFORT_ZONE_SHIBATA);
}
