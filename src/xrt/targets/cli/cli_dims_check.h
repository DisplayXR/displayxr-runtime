// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Does the plug-in's reported panel size match the real display mode?
 *
 * Issue #1201: `selftest` used to PRINT the plug-in's `display_pixel_*` and
 * call that a PASS. On a scaled display it therefore emitted
 * `PASS: display_dims — 2560x1440 px` for a 4K panel, laundering a wrong value
 * into a green result. A check that passes on a wrong number is worse than no
 * check, so the value is now compared against the authoritative display mode
 * (`EnumDisplaySettingsW`, which returns true pixels regardless of the calling
 * process's DPI awareness) and a mismatch FAILS.
 *
 * The decision itself is pure integer arithmetic with no Windows types, so it
 * lives in this header and is unit-tested (`tests/tests_cli_dims_check.cpp`)
 * on every platform — the probe that feeds it is Windows-only, the rule is not.
 *
 * @author David Fattal
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Verdict of comparing reported panel pixels against the native display mode.
 */
enum cli_dims_verdict
{
	//! No authoritative mode to compare against — never a failure (the
	//! probe is Windows-only, and a monitor can fail to resolve).
	CLI_DIMS_NOT_PROBED = 0,
	//! Reported dimensions are the panel's real mode.
	CLI_DIMS_MATCH = 1,
	//! Reported dimensions are NOT the panel's real mode. This fails.
	CLI_DIMS_MISMATCH = 2,
};

/*!
 * Compare the plug-in's reported panel pixels against the native display mode.
 *
 * Absence never fails, matching the house rule the zone-caps and
 * input-provider checks already follow: @p native_probed false, or either
 * native dimension zero, yields @ref CLI_DIMS_NOT_PROBED.
 */
static inline enum cli_dims_verdict
cli_dims_compare(bool native_probed, uint32_t reported_w, uint32_t reported_h, uint32_t native_w, uint32_t native_h)
{
	if (!native_probed || native_w == 0 || native_h == 0) {
		return CLI_DIMS_NOT_PROBED;
	}
	if (reported_w == native_w && reported_h == native_h) {
		return CLI_DIMS_MATCH;
	}
	return CLI_DIMS_MISMATCH;
}

/*!
 * If @p reported is @p native divided by a uniform factor, return that factor
 * as a percentage (150 for a 4K panel reported as 2560x1440), else 0.
 *
 * This is how the failure message names the CAUSE instead of just the symptom:
 * a mismatch that resolves to a round Windows scaling step is a DPI-awareness
 * regression, while an arbitrary ratio is something else (a stale override, a
 * plug-in reporting one monitor's size for another).
 *
 * Only downscaling is reported — a reported size LARGER than the panel is not
 * DPI virtualisation, so it returns 0 and the caller reports the raw numbers.
 */
static inline uint32_t
cli_dims_scaling_percent(uint32_t reported_w, uint32_t reported_h, uint32_t native_w, uint32_t native_h)
{
	if (reported_w == 0 || reported_h == 0 || native_w <= reported_w || native_h <= reported_h) {
		return 0;
	}

	// Round to the nearest percent. Windows' logical size is the physical
	// size divided by the scale factor and truncated, so 175% on 2160 rows
	// gives 1234 -> the two axes can disagree by a percent or so; anything
	// wider than that is not one uniform scale factor.
	uint32_t pct_w = (native_w * 200u / reported_w + 1u) / 2u;
	uint32_t pct_h = (native_h * 200u / reported_h + 1u) / 2u;
	uint32_t spread = pct_w > pct_h ? pct_w - pct_h : pct_h - pct_w;
	if (spread > 2u || pct_w < 110u) {
		return 0;
	}
	return pct_w;
}

#ifdef __cplusplus
}
#endif
