// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Does every VERIFIED screen have physical metres? (display dashboard phase 0)
 *
 * A screen that resolves to 0 m makes the per-segment view layout refuse it,
 * so a window spanning it gets ONE view set for the whole window instead of
 * one per screen (ADR-047, the "0 m screen" trap; ADR-051 code
 * `NO_PHYSICAL_SIZE`). The self-test reports it as a warning, never a failure.
 *
 * The decision is pure (no OS types) so it lives in this header and is
 * unit-tested on every platform (`tests/tests_cli_metres_check.cpp`).
 *
 * @author David Fattal
 */

#pragma once

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_screen.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * True when @p s is claimed at VERIFIED confidence but its resolved screen
 * info (plug-in `get_display_info_for_monitor`, else EDID mm — the
 * `u_screen_info_resolve` rules) carries no physical size.
 */
static inline bool
cli_screen_verified_without_metres(const struct xrt_screen *s)
{
	if (s == NULL || s->confidence < (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED) {
		return false;
	}
	return !(s->info.width_m > 0.0f) || !(s->info.height_m > 0.0f);
}

#ifdef __cplusplus
}
#endif
