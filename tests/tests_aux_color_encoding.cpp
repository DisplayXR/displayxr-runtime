// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The two pure colour decisions shared by every backend (#1589/#1610).
 *
 * The mechanism these decisions drive is a hardware `_SRGB` render target, so
 * the encode itself is only exercisable on a GPU. Everything that DRIVES it is
 * pure and belongs here:
 *
 *   - the sRGB OETF, asserted against the exact BYTES an atlas capture must
 *     read back (the #1589 acceptance numbers), so the oracle a hardware check
 *     compares against has a definition that cannot drift;
 *   - the single-layer fast-path predicate, as a truth table.
 *
 * Device-free and backend-free on purpose: the same header is consumed by the
 * D3D11, Vulkan, Metal and OpenGL compositors.
 */

#include "util/u_color_encoding.h"

#include "catch_amalgamated.hpp"


TEST_CASE("u_color_srgb_encode_u8 — the atlas-capture oracle")
{
	// These four ARE the acceptance numbers: a linear value stored in a
	// UNORM swapchain must reach the atlas as the byte on the right.
	CHECK(u_color_srgb_encode_u8(0.0f) == 0);
	CHECK(u_color_srgb_encode_u8(0.2f) == 124);
	CHECK(u_color_srgb_encode_u8(0.5f) == 188);
	CHECK(u_color_srgb_encode_u8(1.0f) == 255);

	// The negative control: the value a PASSTHROUGH (pre-#1589) runtime
	// would have produced for linear 0.5 is 128, not 188. If a capture
	// reads 128 the frame did not take the encode.
	CHECK(u_color_srgb_encode_u8(0.5f) != 128);
}

TEST_CASE("u_color_srgb_encode — curve shape")
{
	// Clamped outside [0,1] (a negative or >1 sample must not produce NaN
	// out of powf).
	CHECK(u_color_srgb_encode(-1.0f) == 0.0f);
	CHECK(u_color_srgb_encode(2.0f) == 1.0f);

	// The linear segment below the 0.0031308 knee.
	CHECK(u_color_srgb_encode(0.001f) == Catch::Approx(0.001f * 12.92f).epsilon(1e-6));

	// Monotonic across the knee.
	float prev = -1.0f;
	for (int i = 0; i <= 100; i++) {
		float v = u_color_srgb_encode((float)i / 100.0f);
		CHECK(v >= prev);
		prev = v;
	}

	// sRGB lifts midtones: encoded is always >= linear in [0,1].
	CHECK(u_color_srgb_encode(0.5f) > 0.5f);
	CHECK(u_color_srgb_encode(0.2f) > 0.2f);
}

TEST_CASE("u_color_compose_fast_path — the truth table")
{
	const bool hatch_off = false;
	const bool hatch_on = true;

	SECTION("one _SRGB projection layer takes the raw path")
	{
		// The whole shipping app population: nothing to encode (the app
		// already did) and nothing to blend. Byte-identical atlas.
		CHECK(u_color_compose_fast_path(hatch_off, 1, true, true));
	}

	SECTION("one UNORM projection layer must compose")
	{
		// Its values are LINEAR, so the encode is owed even though
		// nothing blends.
		CHECK_FALSE(u_color_compose_fast_path(hatch_off, 1, true, false));
	}

	SECTION("two or more layers always compose, whatever the formats")
	{
		// #1610: the spec blends in linear, which only the _SRGB-view
		// target can do — so a second layer disqualifies the raw path
		// even when every source is already encoded.
		CHECK_FALSE(u_color_compose_fast_path(hatch_off, 2, true, true));
		CHECK_FALSE(u_color_compose_fast_path(hatch_off, 2, true, false));
		CHECK_FALSE(u_color_compose_fast_path(hatch_off, 7, true, true));
	}

	SECTION("a lone non-projection layer composes")
	{
		// A quad / zone / Local2D layer covers a SUB-RECT and blends
		// over the clear, so it is a compose case even when alone.
		CHECK_FALSE(u_color_compose_fast_path(hatch_off, 1, false, true));
	}

	SECTION("an empty frame is not a fast-path frame")
	{
		CHECK_FALSE(u_color_compose_fast_path(hatch_off, 0, false, true));
		CHECK_FALSE(u_color_compose_fast_path(hatch_off, 0, true, true));
	}

	SECTION("the hatch restores the old behaviour WHOLESALE")
	{
		// Every one of the cases above flips: no private target, no
		// copy, no linear blend, UNORM treated as already encoded.
		CHECK(u_color_compose_fast_path(hatch_on, 1, true, true));
		CHECK(u_color_compose_fast_path(hatch_on, 1, true, false));
		CHECK(u_color_compose_fast_path(hatch_on, 2, true, false));
		CHECK(u_color_compose_fast_path(hatch_on, 1, false, false));
		CHECK(u_color_compose_fast_path(hatch_on, 0, false, false));
	}
}

TEST_CASE("DXR_COLOR_LEGACY_UNORM_ENCODED is OFF by default")
{
	// The flip is the default; the hatch is opt-in. ctest runs with a clean
	// environment, so this pins the shipped default rather than the local
	// shell's. (The variable is read once per process, so the ON case
	// cannot be exercised from the same binary — that is what the explicit
	// `legacy_hatch` argument above is for.)
	CHECK_FALSE(u_color_legacy_unorm_encoded());
}
