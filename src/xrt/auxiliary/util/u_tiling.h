// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header-only tiling utilities for multiview atlas layout.
 * @author David Fattal
 * @ingroup aux_util
 *
 * Given N views at per-view scale (fraction of display), computes
 * a near-square tile layout and atlas dimensions for swapchain creation.
 */

#pragma once

#include "xrt/xrt_device.h"

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Compute near-square tile layout for N views of size V_w x V_h.
 *
 * @param view_count     Number of views (N).
 * @param view_w         Per-view width in pixels.
 * @param view_h         Per-view height in pixels.
 * @param[out] out_cols  Tile columns (C).
 * @param[out] out_rows  Tile rows (R).
 */
static inline void
u_tiling_compute_layout(uint32_t view_count,
                        uint32_t view_w,
                        uint32_t view_h,
                        uint32_t *out_cols,
                        uint32_t *out_rows)
{
	if (view_count <= 1 || view_w == 0 || view_h == 0) {
		*out_cols = 1;
		*out_rows = 1;
		return;
	}

	// C = round(sqrt(N * V_h / V_w)) — nearest-square optimization
	double ratio = (double)view_count * (double)view_h / (double)view_w;
	uint32_t cols = (uint32_t)(sqrt(ratio) + 0.5);
	if (cols < 1)
		cols = 1;
	if (cols > view_count)
		cols = view_count;

	uint32_t rows = (view_count + cols - 1) / cols; // ceil(N / C)

	*out_cols = cols;
	*out_rows = rows;
}

/*!
 * Compute tiling fields for a single rendering mode.
 *
 * Fills the runtime-computed fields: tile_columns, tile_rows,
 * view_width_pixels, view_height_pixels, atlas_width_pixels, atlas_height_pixels.
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
	uint32_t vw = (uint32_t)(display_w * mode->view_scale_x);
	uint32_t vh = (uint32_t)(display_h * mode->view_scale_y);
	if (vw == 0)
		vw = display_w;
	if (vh == 0)
		vh = display_h;

	mode->view_width_pixels = vw;
	mode->view_height_pixels = vh;

	uint32_t cols, rows;
	u_tiling_compute_layout(mode->view_count, vw, vh, &cols, &rows);

	mode->tile_columns = cols;
	mode->tile_rows = rows;
	mode->atlas_width_pixels = cols * vw;
	mode->atlas_height_pixels = rows * vh;
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

#ifdef __cplusplus
}
#endif
