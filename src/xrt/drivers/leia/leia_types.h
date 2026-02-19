// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common type definitions for Leia SR SDK integration.
 *         This header can be included without Vulkan or D3D11 dependencies.
 *
 * Eye position, eye pair, and window metrics types are now vendor-neutral
 * aliases defined in xrt/xrt_display_metrics.h. The leiasr_* names are
 * kept for backward compatibility with existing Leia driver code.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_display_metrics.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Backward-compatible aliases — Leia driver code uses leiasr_* names,
 * which now resolve to the vendor-neutral xrt_* types.
 */
typedef struct xrt_eye_position leiasr_eye_position;
typedef struct xrt_eye_pair leiasr_eye_pair;
typedef struct xrt_window_metrics leiasr_window_metrics;

/*!
 * Display dimensions in meters for Kooima FOV calculation.
 *
 * This remains Leia-specific because it includes nominal viewer position
 * data that is only populated by the SR SDK init code.
 */
struct leiasr_display_dimensions
{
	float width_m;      //!< Screen width in meters
	float height_m;     //!< Screen height in meters
	float nominal_x_m;  //!< Nominal viewer X in meters (display space)
	float nominal_y_m;  //!< Nominal viewer Y in meters (display space)
	float nominal_z_m;  //!< Nominal viewer Z in meters (display space)
	bool valid;         //!< True if the dimensions are valid
};

#ifdef __cplusplus
}
#endif
