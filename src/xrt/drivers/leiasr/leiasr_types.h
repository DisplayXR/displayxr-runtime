// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common type definitions for Leia SR SDK integration.
 *         This header can be included without Vulkan or D3D11 dependencies.
 * @author David Fattal
 * @ingroup drv_leiasr
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Eye position in meters (converted from SR's millimeters).
 * Position is relative to the display center.
 */
struct leiasr_eye_position
{
	float x;  //!< Horizontal position (positive = right)
	float y;  //!< Vertical position (positive = up)
	float z;  //!< Depth position (positive = toward viewer)
};

/*!
 * Eye pair containing both left and right eye positions.
 */
struct leiasr_eye_pair
{
	struct leiasr_eye_position left;   //!< Left eye position in meters
	struct leiasr_eye_position right;  //!< Right eye position in meters
	int64_t timestamp_ns;              //!< Monotonic timestamp when the eye positions were sampled
	bool valid;                        //!< True if the eye positions are valid
};

/*!
 * Display dimensions in meters for Kooima FOV calculation.
 */
struct leiasr_display_dimensions
{
	float width_m;   //!< Screen width in meters
	float height_m;  //!< Screen height in meters
	bool valid;      //!< True if the dimensions are valid
};

#ifdef __cplusplus
}
#endif
