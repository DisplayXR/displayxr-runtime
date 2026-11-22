// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `conv run`: the downscale rule and the intrinsics that follow it.
 *
 * A focal length given with `--focal-px` is in the input file's pixels; when
 * the input is downscaled to the capability's max input edge, the provider
 * must receive it in the downscaled image's pixels, or every metric it
 * derives from the focal length (depth, field of view) is off by the factor.
 */

#include "catch_amalgamated.hpp"

#include "cli_conv_run_math.h"

TEST_CASE("cli_conv_downscale_dims: the long side is bounded, the aspect kept")
{
	int w = 0, h = 0;
	SECTION("no limit or already small: untouched")
	{
		CHECK_FALSE(cli_conv_downscale_dims(6016, 4000, 0, 1, &w, &h));
		CHECK((w == 6016 && h == 4000));
		CHECK_FALSE(cli_conv_downscale_dims(1536, 1024, 1536, 1, &w, &h));
		CHECK((w == 1536 && h == 1024));
	}
	SECTION("a landscape photo above the edge")
	{
		REQUIRE(cli_conv_downscale_dims(6016, 4000, 1536, 1, &w, &h));
		CHECK(w == 1536);
		CHECK(h == 1021);
	}
	SECTION("a side-by-side pair: per-eye long side, width stays even")
	{
		REQUIRE(cli_conv_downscale_dims(3840, 1080, 960, 2, &w, &h));
		CHECK(w == 1920);
		CHECK(h == 540);
		CHECK(w % 2 == 0);
	}
}

TEST_CASE("cli_conv_scale_intrinsic: a given focal length follows the image")
{
	int w = 0, h = 0;
	REQUIRE(cli_conv_downscale_dims(6016, 4000, 1536, 1, &w, &h));
	const float fx = cli_conv_scale_intrinsic(5009.2346f, 6016, w);
	const float fy = cli_conv_scale_intrinsic(5009.2346f, 4000, h);
	CHECK(fx == Catch::Approx(5009.2346 * 1536.0 / 6016.0));
	CHECK(fy == Catch::Approx(5009.2346 * 1021.0 / 4000.0));
	// The field of view is preserved: f / W is invariant under the downscale.
	CHECK(fx / (float)w == Catch::Approx(5009.2346 / 6016.0));

	SECTION("unknown stays unknown, untouched stays untouched")
	{
		CHECK(cli_conv_scale_intrinsic(0.0f, 6016, 1536) == 0.0f);
		CHECK(cli_conv_scale_intrinsic(-1.0f, 6016, 1536) == -1.0f);
		CHECK(cli_conv_scale_intrinsic(1234.5f, 6016, 6016) == 1234.5f);
		CHECK(cli_conv_scale_intrinsic(1234.5f, 0, 1536) == 1234.5f);
	}
}
