// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common type definitions for Leia SR SDK integration.
 *         This header can be included without Vulkan or D3D11 dependencies.
 * @author David Fattal
 * @ingroup drv_leia
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
	float nominal_x_m;  //!< Nominal viewer X in meters (display space)
	float nominal_y_m;  //!< Nominal viewer Y in meters (display space)
	float nominal_z_m;  //!< Nominal viewer Z in meters (display space)
	bool valid;      //!< True if the dimensions are valid
};

/*!
 * Window metrics for adaptive resize.
 * Contains display and window geometry needed to compute
 * window-adaptive FOV and eye position adjustments.
 */
struct leiasr_window_metrics
{
	float display_width_m;              //!< Display physical width (meters)
	float display_height_m;             //!< Display physical height (meters)
	uint32_t display_pixel_width;       //!< Display pixel width
	uint32_t display_pixel_height;      //!< Display pixel height
	int32_t display_screen_left;        //!< Display left edge (screen coords)
	int32_t display_screen_top;         //!< Display top edge (screen coords)

	uint32_t window_pixel_width;        //!< Window client area width (pixels)
	uint32_t window_pixel_height;       //!< Window client area height (pixels)
	int32_t window_screen_left;         //!< Window client area left (screen coords)
	int32_t window_screen_top;          //!< Window client area top (screen coords)

	float window_width_m;               //!< Window physical width (meters)
	float window_height_m;              //!< Window physical height (meters)
	float window_center_offset_x_m;     //!< Window center offset from display center (meters, +right)
	float window_center_offset_y_m;     //!< Window center offset from display center (meters, +up)

	bool valid;                         //!< True if all metrics are valid
};

#ifdef __cplusplus
}
#endif
