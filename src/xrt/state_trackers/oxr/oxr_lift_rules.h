// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift view-count rule (#2005 G2), shared by xrSubmitLiftFrameDXR
 *         (oxr_lift.c) and the weave path's lifted rects (oxr_weave.c).
 *
 * `XrLiftPropertiesDXR::maxViews` "bounds XrLiftOptionsDXR::viewCount for
 * NVIEW" (XR_DXR_lift spec §2), and a bad viewCount is
 * XR_ERROR_VALIDATION_FAILURE (§10). The state tracker used to check only the
 * extension's constant ceiling (XR_LIFT_MAX_VIEWS_DXR, 8), so a module that
 * reported fewer views got requests it could not serve.
 *
 * The bound is the module's reported max_views, as the session last read it
 * from xrGetLiftPropertiesDXR; 0 (never read, or not reported) falls back to
 * the constant, which also caps a module that over-reports.
 *
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_dp_lift.h"
#include "xrt/xrt_lift.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The effective view bound for a module reporting @p module_max_views (0 = unknown).
static inline uint32_t
oxr_lift_view_bound(uint32_t module_max_views)
{
	return module_max_views >= 1 && module_max_views <= XRT_LIFT_MAX_VIEWS ? module_max_views
	                                                                        : (uint32_t)XRT_LIFT_MAX_VIEWS;
}

/*!
 * Whether an app's XrLiftOptionsDXR::viewCount is in contract for a stream of
 * @p mode (one XRT_DP_LIFT_MODE_* bit). The constant ceiling applies to every
 * mode; the module's maxViews to NVIEW, the mode the spec bounds with it.
 */
static inline bool
oxr_lift_view_count_ok(uint32_t mode, uint32_t view_count, uint32_t module_max_views)
{
	if (view_count > XRT_LIFT_MAX_VIEWS) {
		return false;
	}
	if (mode != XRT_DP_LIFT_MODE_NVIEW) {
		return true;
	}
	return view_count <= oxr_lift_view_bound(module_max_views);
}

#ifdef __cplusplus
}
#endif
