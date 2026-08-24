// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared weave-scope test double for the sim_display DP variants.
 *
 * sim_display writes final pixels itself, so its honest scope is
 * @ref XRT_DP_WEAVE_SCOPE_CANVAS — which is also what every plug-in that
 * doesn't implement the slot reports. That would leave the other two scopes
 * with no way to be exercised until real hardware shows up, so this header
 * adds one env knob:
 *
 *  - `SIM_DISPLAY_WEAVE_SCOPE=canvas|region|scanout` — what the DP claims.
 *    Default `canvas` (honest). `region` / `scanout` let a developer — or a
 *    vendor bringing up a display that weaves in an FPGA/ASIC — drive the
 *    runtime's routing and its diagnostics with no hardware at all.
 *
 * Declaring a scope does NOT change what sim_display renders; it changes what
 * it *claims*, which is the whole surface under test.
 *
 * Header-only so all five per-API variants (VK / D3D11 / D3D12 / GL / Metal —
 * C, C++ and Objective-C TUs) share one parser, matching
 * `sim_display_zone_common.h`.
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h> // getenv
#include <string.h>

#include "xrt/xrt_display_scanout.h"
#include "util/u_logging.h"

/*!
 * Parse `SIM_DISPLAY_WEAVE_SCOPE`. Unset or unrecognised ⟹ canvas.
 */
static inline enum xrt_dp_weave_scope
sim_scanout_scope_from_env(void)
{
	const char *v = getenv("SIM_DISPLAY_WEAVE_SCOPE");
	if (v == NULL) {
		return XRT_DP_WEAVE_SCOPE_CANVAS;
	}
	if (strcmp(v, "region") == 0) {
		return XRT_DP_WEAVE_SCOPE_REGION;
	}
	if (strcmp(v, "scanout") == 0) {
		return XRT_DP_WEAVE_SCOPE_SCANOUT;
	}
	if (strcmp(v, "canvas") != 0) {
		U_LOG_W("sim_display: SIM_DISPLAY_WEAVE_SCOPE='%s' unrecognised — using canvas", v);
	}
	return XRT_DP_WEAVE_SCOPE_CANVAS;
}

/*!
 * Body of a sim_display `get_scanout_caps`. Honours the caller's
 * `struct_size` exactly as a real plug-in must.
 *
 * @param out_caps  Caller-provided, `struct_size` pre-set by the runtime.
 * @param variant   Per-API tag for the log line, e.g. "D3D11".
 * @return true when @p out_caps was filled.
 */
static inline bool
sim_scanout_fill_caps(struct xrt_dp_scanout_caps *out_caps, const char *variant)
{
	if (out_caps == NULL || out_caps->struct_size < XRT_DP_SCANOUT_CAPS_SIZE_V1) {
		return false;
	}

	const enum xrt_dp_weave_scope scope = sim_scanout_scope_from_env();
	out_caps->weave_scope = (uint32_t)scope;
	for (size_t i = 0; i < sizeof(out_caps->reserved) / sizeof(out_caps->reserved[0]); i++) {
		out_caps->reserved[i] = 0;
	}

	U_LOG_W("sim_display %s DP: declaring weave scope '%s'", variant, xrt_dp_weave_scope_name(scope));
	return true;
}
