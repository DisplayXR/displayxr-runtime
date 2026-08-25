// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The self-test rule that turns `display_dims` from a print into a proof.
 *
 * Issue #1201: on a 4K panel at 150% Windows scaling a DPI-unaware
 * `displayxr-cli` read the display as 2560x1440 and its self-test emitted
 * `PASS: display_dims — … 2560x1440 px`. The number was wrong and the check
 * still went green, which is worse than having no check at all.
 *
 * What is pinned here is the decision, not the probe: the comparison must FAIL
 * on any divergence from the authoritative mode, must never fail merely
 * because there was nothing to compare against, and must be able to name DPI
 * scaling as the cause when the ratio is exactly a scaling step.
 */

#include "catch_amalgamated.hpp"

#include "cli_dims_check.h"


TEST_CASE("cli_dims_compare: the reported panel size must equal the real mode")
{
	SECTION("identical dimensions pass")
	{
		CHECK(cli_dims_compare(true, 3840, 2160, 3840, 2160) == CLI_DIMS_MATCH);
	}

	SECTION("the #1201 signature — 4K panel reported at 150% logical — FAILS")
	{
		CHECK(cli_dims_compare(true, 2560, 1440, 3840, 2160) == CLI_DIMS_MISMATCH);
	}

	SECTION("any divergence fails, not just DPI-shaped ones")
	{
		// A single axis is enough; the check is equality, not similarity.
		CHECK(cli_dims_compare(true, 3840, 1440, 3840, 2160) == CLI_DIMS_MISMATCH);
		CHECK(cli_dims_compare(true, 1920, 1080, 3840, 2160) == CLI_DIMS_MISMATCH);
		// Reported LARGER than the panel is still a mismatch.
		CHECK(cli_dims_compare(true, 7680, 4320, 3840, 2160) == CLI_DIMS_MISMATCH);
	}
}

TEST_CASE("cli_dims_compare: absence never fails")
{
	SECTION("no probe (non-Windows, or the monitor did not resolve)")
	{
		CHECK(cli_dims_compare(false, 2560, 1440, 3840, 2160) == CLI_DIMS_NOT_PROBED);
	}

	SECTION("a probe that produced a zero dimension is treated as no probe")
	{
		CHECK(cli_dims_compare(true, 2560, 1440, 0, 2160) == CLI_DIMS_NOT_PROBED);
		CHECK(cli_dims_compare(true, 2560, 1440, 3840, 0) == CLI_DIMS_NOT_PROBED);
	}
}

TEST_CASE("cli_dims_scaling_percent names DPI scaling as the cause")
{
	SECTION("the Windows scaling steps")
	{
		// 150% — the reference box in #1201.
		CHECK(cli_dims_scaling_percent(2560, 1440, 3840, 2160) == 150);
		// 125%.
		CHECK(cli_dims_scaling_percent(3072, 1728, 3840, 2160) == 125);
		// 200%.
		CHECK(cli_dims_scaling_percent(1920, 1080, 3840, 2160) == 200);
		// 300% — what a 8K-class panel is typically shipped at.
		CHECK(cli_dims_scaling_percent(2560, 1440, 7680, 4320) == 300);
	}

	SECTION("175%, where Windows' truncation makes the two axes disagree slightly")
	{
		// 3840/1.75 = 2194.3, 2160/1.75 = 1234.3 — both truncated by Windows.
		CHECK(cli_dims_scaling_percent(2194, 1234, 3840, 2160) == 175);
	}

	SECTION("a non-uniform ratio is NOT scaling, so no factor is claimed")
	{
		// Half the width but full height — no single scale factor explains it.
		CHECK(cli_dims_scaling_percent(1920, 2160, 3840, 2160) == 0);
	}

	SECTION("equal or larger reported dimensions are never scaling")
	{
		CHECK(cli_dims_scaling_percent(3840, 2160, 3840, 2160) == 0);
		CHECK(cli_dims_scaling_percent(7680, 4320, 3840, 2160) == 0);
	}

	SECTION("zero reported dimensions do not divide by zero")
	{
		CHECK(cli_dims_scaling_percent(0, 1440, 3840, 2160) == 0);
		CHECK(cli_dims_scaling_percent(2560, 0, 3840, 2160) == 0);
	}
}
