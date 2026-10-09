// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The selftest rule "every VERIFIED claim has physical metres".
 *
 * Display dashboard phase 0 (ADR-051, warning code `NO_PHYSICAL_SIZE`): a
 * screen that resolves to 0 m makes the per-segment view layout refuse it, so
 * a window spanning it gets one view set for the whole window. The self-test
 * warns about it (never fails); what is pinned here is which screens count.
 */

#include "catch_amalgamated.hpp"

#include "cli_metres_check.h"

#include <cstring>

static struct xrt_screen
screen(uint32_t confidence, float w_m, float h_m)
{
	struct xrt_screen s;
	std::memset(&s, 0, sizeof(s));
	s.id = 0x8c413a2f61152ce7ull;
	s.confidence = confidence;
	s.info.width_m = w_m;
	s.info.height_m = h_m;
	return s;
}

TEST_CASE("cli_screen_verified_without_metres")
{
	SECTION("a VERIFIED screen with metres is fine")
	{
		struct xrt_screen s = screen(XRT_DISPLAY_CLAIM_VERIFIED, 0.344f, 0.193f);
		CHECK_FALSE(cli_screen_verified_without_metres(&s));
	}

	SECTION("a VERIFIED screen at 0 m is the trap")
	{
		struct xrt_screen s = screen(XRT_DISPLAY_CLAIM_VERIFIED, 0.0f, 0.0f);
		CHECK(cli_screen_verified_without_metres(&s));
	}

	SECTION("one missing axis is enough")
	{
		struct xrt_screen w = screen(XRT_DISPLAY_CLAIM_VERIFIED, 0.344f, 0.0f);
		struct xrt_screen h = screen(XRT_DISPLAY_CLAIM_VERIFIED, 0.0f, 0.193f);
		CHECK(cli_screen_verified_without_metres(&w));
		CHECK(cli_screen_verified_without_metres(&h));
	}

	SECTION("a negative size counts as missing")
	{
		struct xrt_screen n = screen(XRT_DISPLAY_CLAIM_VERIFIED, -0.1f, 0.193f);
		CHECK(cli_screen_verified_without_metres(&n));
	}

	SECTION("EDID, FALLBACK and synthesized screens are out of scope")
	{
		struct xrt_screen e = screen(XRT_DISPLAY_CLAIM_EDID, 0.0f, 0.0f);
		struct xrt_screen f = screen(XRT_DISPLAY_CLAIM_FALLBACK, 0.0f, 0.0f);
		struct xrt_screen z = screen(0, 0.0f, 0.0f);
		CHECK_FALSE(cli_screen_verified_without_metres(&e));
		CHECK_FALSE(cli_screen_verified_without_metres(&f));
		CHECK_FALSE(cli_screen_verified_without_metres(&z));
	}

	SECTION("NULL is not a warning")
	{
		CHECK_FALSE(cli_screen_verified_without_metres(nullptr));
	}
}
