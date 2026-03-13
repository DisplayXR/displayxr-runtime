// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header-only tiling utilities for multiview atlas layout.
 * @author David Fattal
 * @ingroup aux_util
 *
 * Drivers specify tile_columns and tile_rows in each rendering mode.
 * This file computes derived pixel dimensions and provides helpers
 * for atlas layout and zero-copy eligibility checking.
 */

#pragma once

#include "xrt/xrt_device.h"
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Compute tiling fields for a single rendering mode.
 *
 * Fills the runtime-computed fields: view_width_pixels, view_height_pixels,
 * atlas_width_pixels, atlas_height_pixels. The driver must have already set
 * tile_columns and tile_rows.
 *
 * @param mode       The rendering mode to fill (in/out).
 * @param display_w  Native display width in pixels.
 * @param display_h  Native display height in pixels.
 */
static inline void
u_tiling_compute_mode(struct xrt_rendering_mode *mode,
                      uint32_t display_w,
                      uint32_t display_h)
{
	assert(mode->tile_columns > 0 && "Driver must set tile_columns");
	assert(mode->tile_rows > 0 && "Driver must set tile_rows");

	uint32_t vw = (uint32_t)(display_w * mode->view_scale_x);
	uint32_t vh = (uint32_t)(display_h * mode->view_scale_y);
	if (vw == 0)
		vw = display_w;
	if (vh == 0)
		vh = display_h;

	mode->view_width_pixels = vw;
	mode->view_height_pixels = vh;
	// tile_columns and tile_rows are already set by the driver
	mode->atlas_width_pixels = mode->tile_columns * vw;
	mode->atlas_height_pixels = mode->tile_rows * vh;
}

/*!
 * Compute system-wide worst-case atlas dimensions across all modes.
 *
 * @param modes          Array of rendering modes (already computed).
 * @param count          Number of modes.
 * @param[out] out_w     Max atlas width.
 * @param[out] out_h     Max atlas height.
 */
static inline void
u_tiling_compute_system_atlas(const struct xrt_rendering_mode *modes,
                              uint32_t count,
                              uint32_t *out_w,
                              uint32_t *out_h)
{
	uint32_t max_w = 0, max_h = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (modes[i].atlas_width_pixels > max_w)
			max_w = modes[i].atlas_width_pixels;
		if (modes[i].atlas_height_pixels > max_h)
			max_h = modes[i].atlas_height_pixels;
	}
	*out_w = max_w;
	*out_h = max_h;
}

/*!
 * Compute the origin of a view within the atlas.
 *
 * @param view_index  Index of the view (0..N-1).
 * @param cols        Tile columns.
 * @param view_w      Per-view width.
 * @param view_h      Per-view height.
 * @param[out] out_x  X origin in pixels.
 * @param[out] out_y  Y origin in pixels.
 */
static inline void
u_tiling_view_origin(uint32_t view_index,
                     uint32_t cols,
                     uint32_t view_w,
                     uint32_t view_h,
                     uint32_t *out_x,
                     uint32_t *out_y)
{
	*out_x = (view_index % cols) * view_w;
	*out_y = (view_index / cols) * view_h;
}

/*!
 * Check whether a view's subImage rect matches the expected tile position
 * for zero-copy passthrough.
 *
 * @param view_index       Index of the view (0..N-1).
 * @param rect_x           subImage.imageRect.offset.x
 * @param rect_y           subImage.imageRect.offset.y
 * @param rect_w           subImage.imageRect.extent.width
 * @param rect_h           subImage.imageRect.extent.height
 * @param mode             Active rendering mode.
 * @return true if the rect matches the expected tile position and size.
 */
static inline bool
u_tiling_view_matches_tile(uint32_t view_index,
                           int32_t rect_x,
                           int32_t rect_y,
                           uint32_t rect_w,
                           uint32_t rect_h,
                           const struct xrt_rendering_mode *mode)
{
	uint32_t expected_x, expected_y;
	u_tiling_view_origin(view_index, mode->tile_columns,
	                     mode->view_width_pixels, mode->view_height_pixels,
	                     &expected_x, &expected_y);

	return (uint32_t)rect_x == expected_x &&
	       (uint32_t)rect_y == expected_y &&
	       rect_w == mode->view_width_pixels &&
	       rect_h == mode->view_height_pixels;
}

/*!
 * Check whether a swapchain can be passed directly to the display processor
 * without atlas copy (zero-copy passthrough).
 *
 * Checks that all views' subImage rects match expected tile positions and
 * that the swapchain dimensions match the atlas dimensions.
 *
 * @param view_count       Number of views.
 * @param rect_xs          Array of subImage.imageRect.offset.x per view.
 * @param rect_ys          Array of subImage.imageRect.offset.y per view.
 * @param rect_ws          Array of subImage.imageRect.extent.width per view.
 * @param rect_hs          Array of subImage.imageRect.extent.height per view.
 * @param swapchain_w      Swapchain width.
 * @param swapchain_h      Swapchain height.
 * @param mode             Active rendering mode.
 * @return true if zero-copy is possible.
 */
static inline bool
u_tiling_can_zero_copy(uint32_t view_count,
                       const int32_t *rect_xs,
                       const int32_t *rect_ys,
                       const uint32_t *rect_ws,
                       const uint32_t *rect_hs,
                       uint32_t swapchain_w,
                       uint32_t swapchain_h,
                       const struct xrt_rendering_mode *mode)
{
	// View count must match mode
	if (view_count != mode->view_count)
		return false;

	// Swapchain must match atlas dimensions exactly
	if (swapchain_w != mode->atlas_width_pixels ||
	    swapchain_h != mode->atlas_height_pixels)
		return false;

	// Each view's rect must match its expected tile position
	for (uint32_t i = 0; i < view_count; i++) {
		if (!u_tiling_view_matches_tile(i, rect_xs[i], rect_ys[i],
		                                rect_ws[i], rect_hs[i], mode))
			return false;
	}

	return true;
}

#ifdef __cplusplus
}
#endif
