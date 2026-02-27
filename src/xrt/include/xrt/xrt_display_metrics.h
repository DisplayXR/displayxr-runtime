// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vendor-neutral types for display metrics, eye positions, and window geometry.
 *
 * These types are used by the Kooima asymmetric FOV calculation and
 * window-adaptive rendering. They abstract away vendor-specific SDK types
 * so that sim_display (and future vendors) can share the same code paths.
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Eye position in meters relative to the display center.
 *
 * Used by the Kooima asymmetric FOV algorithm. The coordinate system is:
 * - X: horizontal (positive = right)
 * - Y: vertical (positive = up)
 * - Z: depth (positive = toward viewer, away from screen)
 *
 * @ingroup xrt_iface
 */
struct xrt_eye_position
{
	float x; //!< Horizontal position (positive = right)
	float y; //!< Vertical position (positive = up)
	float z; //!< Depth position (positive = toward viewer)
};

/*!
 * Eye pair containing both left and right eye positions.
 *
 * @ingroup xrt_iface
 */
struct xrt_eye_pair
{
	struct xrt_eye_position left;  //!< Left eye position in meters
	struct xrt_eye_position right; //!< Right eye position in meters
	int64_t timestamp_ns;          //!< Monotonic timestamp when the eye positions were sampled
	bool valid;                    //!< True if the eye positions are valid
	bool is_tracking;              //!< True if physical eye tracker has lock on user.
	                               //!< When false, positions are still valid — vendor SDK
	                               //!< provides reasonable fallback (last known, filtered, etc.)
};

/*!
 * Window metrics for adaptive FOV calculation and eye position adjustment.
 *
 * Contains display and window geometry needed to compute window-adaptive
 * Kooima FOV. All physical dimensions are in meters; pixel dimensions are
 * in display pixels; screen coordinates are in the OS virtual screen space.
 *
 * @ingroup xrt_iface
 */
struct xrt_window_metrics
{
	float display_width_m;          //!< Display physical width (meters)
	float display_height_m;         //!< Display physical height (meters)
	uint32_t display_pixel_width;   //!< Display pixel width
	uint32_t display_pixel_height;  //!< Display pixel height
	int32_t display_screen_left;    //!< Display left edge (screen coords)
	int32_t display_screen_top;     //!< Display top edge (screen coords)

	uint32_t window_pixel_width;    //!< Window client area width (pixels)
	uint32_t window_pixel_height;   //!< Window client area height (pixels)
	int32_t window_screen_left;     //!< Window client area left (screen coords)
	int32_t window_screen_top;      //!< Window client area top (screen coords)

	float window_width_m;           //!< Window physical width (meters)
	float window_height_m;          //!< Window physical height (meters)
	float window_center_offset_x_m; //!< Window center offset from display center (meters, +right)
	float window_center_offset_y_m; //!< Window center offset from display center (meters, +up)

	bool valid; //!< True if all metrics are valid
};

#ifdef __cplusplus
}
#endif
