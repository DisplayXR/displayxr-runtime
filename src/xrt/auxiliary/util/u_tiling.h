// Copyright 2025, The DisplayXR Project
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
 * Worst-case atlas dimensions across all modes, spanning device orientation for
 * modes flagged @ref XRT_RENDERING_MODE_FLAG_CAN_ROTATE.
 *
 * For each mode the atlas is sized at the given (display_w, display_h); if the mode
 * sets CAN_ROTATE it is ALSO sized at the 90°-swapped (display_h, display_w), and the
 * global per-dimension max is taken. On Android the app's render swapchain is never
 * recreated on rotation, so it must be allocated to this worst case to survive a flip;
 * orientation-locked modes (flag clear) only contribute their native orientation.
 *
 * Computes from view_scale_x/y + tile_columns/rows directly (does NOT require or mutate
 * the modes' runtime-computed fields), so it is independent of @ref u_tiling_compute_mode.
 *
 * @param modes        Array of rendering modes (tile_columns/rows + view_scale_x/y set).
 * @param count        Number of modes.
 * @param display_w    Native (current) display width in pixels.
 * @param display_h    Native (current) display height in pixels.
 * @param[out] out_w   Worst-case atlas width.
 * @param[out] out_h   Worst-case atlas height.
 */
static inline void
u_tiling_compute_system_atlas_oriented(const struct xrt_rendering_mode *modes,
                                       uint32_t count,
                                       uint32_t display_w,
                                       uint32_t display_h,
                                       uint32_t *out_w,
                                       uint32_t *out_h)
{
	uint32_t max_w = 0, max_h = 0;
	for (uint32_t i = 0; i < count; i++) {
		const struct xrt_rendering_mode *m = &modes[i];
		// Native orientation, plus the 90°-swapped one when the mode can rotate.
		uint32_t dims[2][2] = {{display_w, display_h}, {display_h, display_w}};
		uint32_t n = (m->mode_flags & XRT_RENDERING_MODE_FLAG_CAN_ROTATE) ? 2u : 1u;
		for (uint32_t o = 0; o < n; o++) {
			uint32_t dw = dims[o][0], dh = dims[o][1];
			uint32_t vw = (uint32_t)(dw * m->view_scale_x);
			uint32_t vh = (uint32_t)(dh * m->view_scale_y);
			if (vw == 0)
				vw = dw;
			if (vh == 0)
				vh = dh;
			uint32_t aw = m->tile_columns * vw;
			uint32_t ah = m->tile_rows * vh;
			if (aw > max_w)
				max_w = aw;
			if (ah > max_h)
				max_h = ah;
		}
	}
	*out_w = max_w;
	*out_h = max_h;
}

/*!
 * Compute the origin of a view within the atlas, for a TOP-LEFT-origin API.
 *
 * **Tile order (#1625).** View @p view_index occupies the tile at column
 * `view_index % cols`, row `view_index / cols`, counting rows **downward from
 * the TOP edge of the atlas as displayed** — the picture a human sees, not a
 * texel address. **View 0 is the top-left tile on every backend and every
 * platform.** X needs no such statement: no graphics API flips X.
 *
 * This variant returns the Y of the tile's **top** edge, which is what D3D11,
 * D3D12, Metal and Vulkan want (`TopLeftY` / `MTLViewport.originY` /
 * `VkViewport.y` all address texel row 0 at the top).
 *
 * **OpenGL must use @ref u_tiling_view_origin_gl instead** — a GL framebuffer's
 * origin is the BOTTOM-left, so handing `glViewport` the Y this function
 * returns places view 0 in the bottom row and mirrors the whole tile grid
 * against every other backend. That was #1625.
 *
 * @param view_index  Index of the view (0..N-1).
 * @param cols        Tile columns.
 * @param view_w      Per-view width.
 * @param view_h      Per-view height.
 * @param[out] out_x  X origin in pixels.
 * @param[out] out_y  Y origin in pixels, from the TOP edge.
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
 * @ref u_tiling_view_origin for a BOTTOM-LEFT-origin API (OpenGL).
 *
 * Same physical tile, expressed as the `glViewport` Y of its **lower** edge:
 * row `r = view_index / cols` counted from the top becomes row
 * `rows - 1 - r` counted from the bottom. Hence view 0 lands at
 * `(rows - 1) * view_h` — the top tile row as displayed, matching every other
 * backend.
 *
 * A GL display processor must index the atlas the mirrored way to match: tile
 * row `r` occupies `v ∈ [1 - (r+1)/rows, 1 - r/rows]`, i.e. sample view
 * `view_index` at `v = (local_v + (rows - 1 - r)) / rows`. The compositor half
 * and the DP half are one statement and **must change together** — see
 * `sim_display_processor_gl.c`.
 *
 * Identity when `rows == 1`, which is every mode any shipped vendor plug-in
 * publishes.
 *
 * @param view_index  Index of the view (0..N-1).
 * @param cols        Tile columns.
 * @param rows        Tile rows.
 * @param view_w      Per-view width.
 * @param view_h      Per-view height.
 * @param[out] out_x  X origin in pixels.
 * @param[out] out_y  Y origin in pixels, from the BOTTOM edge.
 */
static inline void
u_tiling_view_origin_gl(uint32_t view_index,
                        uint32_t cols,
                        uint32_t rows,
                        uint32_t view_w,
                        uint32_t view_h,
                        uint32_t *out_x,
                        uint32_t *out_y)
{
	uint32_t c = cols > 0 ? cols : 1;
	uint32_t r = rows > 0 ? rows : 1;
	uint32_t row = view_index / c;
	uint32_t flipped = row < r ? (r - 1u - row) : 0u;

	*out_x = (view_index % c) * view_w;
	*out_y = flipped * view_h;
}

/*!
 * Which end of the submitted image the app's `subImage.imageRect.offset.y` is
 * measured from (#1628).
 *
 * The offset is a bare number that the app computes with the SAME expression it
 * feeds its own viewport call, so its meaning follows the app's framebuffer
 * origin — and a D3D/Metal/Vulkan app and an OpenGL app disagree about which
 * row `y = 0` names. #1625 settled the atlas side ("view 0 is the top-left tile
 * as displayed"); this enum is the submission side of the same statement, and
 * it must be passed explicitly because a predicate that silently means
 * different things to different callers is exactly what hid this.
 */
enum u_tiling_origin
{
	//! `offset.y` counts DOWN from the top edge — D3D11, D3D12, Metal, Vulkan.
	U_TILING_ORIGIN_TOP_LEFT = 0,
	//! `offset.y` counts UP from the bottom edge — OpenGL (`glViewport`).
	U_TILING_ORIGIN_BOTTOM_LEFT = 1,
};

/*!
 * Check whether a view's subImage rect matches the expected tile position
 * for zero-copy passthrough.
 *
 * **Origin (#1628).** @p origin says which end of the submitted image
 * `rect_y` is measured from. Under `U_TILING_ORIGIN_BOTTOM_LEFT` the
 * expectation is @ref u_tiling_view_origin_gl's, i.e. row `view_index / cols`
 * counted from the BOTTOM — because a GL app writes its tiles with
 * `glViewport`, whose Y origin is the bottom, so `rect_y == 0` names the
 * geometrically BOTTOM row while the top-origin expectation reads the same 0 as
 * the TOP row. They agree numerically and mean opposite rows. The two collapse
 * to the same test at `tile_rows == 1`, which is every mode any shipped vendor
 * plug-in publishes.
 *
 * The bottom-origin expectation is well defined only because the caller has
 * already required swapchain == atlas (see @ref u_tiling_can_zero_copy): the
 * row count to flip against is the mode's, and it only describes the submitted
 * image when that image IS the atlas.
 *
 * @param view_index       Index of the view (0..N-1).
 * @param rect_x           subImage.imageRect.offset.x
 * @param rect_y           subImage.imageRect.offset.y
 * @param rect_w           subImage.imageRect.extent.width
 * @param rect_h           subImage.imageRect.extent.height
 * @param mode             Active rendering mode.
 * @param origin           Which end of the image @p rect_y is measured from.
 * @return true if the rect matches the expected tile position and size.
 */
static inline bool
u_tiling_view_matches_tile(uint32_t view_index,
                           int32_t rect_x,
                           int32_t rect_y,
                           uint32_t rect_w,
                           uint32_t rect_h,
                           const struct xrt_rendering_mode *mode,
                           enum u_tiling_origin origin)
{
	uint32_t expected_x, expected_y;
	if (origin == U_TILING_ORIGIN_BOTTOM_LEFT) {
		u_tiling_view_origin_gl(view_index, mode->tile_columns, mode->tile_rows,
		                        mode->view_width_pixels, mode->view_height_pixels,
		                        &expected_x, &expected_y);
	} else {
		u_tiling_view_origin(view_index, mode->tile_columns,
		                     mode->view_width_pixels, mode->view_height_pixels,
		                     &expected_x, &expected_y);
	}

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
 * @param origin           Which end of the submitted image the rect Y offsets
 *                         are measured from — the caller's backend decides
 *                         (#1628). Not a second eligibility gate: it changes
 *                         what the SAME question means, not which submissions
 *                         are allowed to answer it.
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
                       const struct xrt_rendering_mode *mode,
                       enum u_tiling_origin origin)
{
	// ADR-041: the submission must COVER the mode, not equal it. The app's view
	// count is fixed by its view configuration (R) while the mode's tile count
	// (A) changes underneath it, so a conformant app submits R views and aliases
	// the inactive tail [A, R) onto view 0's subimage. Those tail views carry no
	// content the runtime reads, so they cannot disqualify the passthrough —
	// only the first mode->view_count rects are checked below. A submission that
	// does NOT cover the mode (R < A, e.g. PRIMARY_STEREO in a quad mode) still
	// fails: there is no atlas to hand over.
	//
	// This is still the SOLE zero-copy gate (ADR-030). Relaxing it is not an
	// optimisation: under ADR-041 Windows Leia's worst-case-filling 2D mode is a
	// 1-view mode that a PRIMARY_STEREO app now submits 2 views into, so an
	// equality test here would silently retire the one shipping zero-copy case.
	if (view_count < mode->view_count)
		return false;

	// Swapchain must match atlas dimensions exactly
	if (swapchain_w != mode->atlas_width_pixels ||
	    swapchain_h != mode->atlas_height_pixels)
		return false;

	// Each ACTIVE view's rect must match its expected tile position, read in
	// the caller's own Y origin (#1628). The swapchain == atlas test above is
	// what makes a bottom-origin expectation meaningful, so it must stay
	// ahead of this loop. The inactive tail is deliberately not inspected —
	// see above.
	for (uint32_t i = 0; i < mode->view_count; i++) {
		if (!u_tiling_view_matches_tile(i, rect_xs[i], rect_ys[i],
		                                rect_ws[i], rect_hs[i], mode, origin))
			return false;
	}

	return true;
}

/*!
 * Compute canvas-adjusted view dimensions for shared-texture apps.
 *
 * When the canvas (output rect) differs from the display, view dimensions
 * should be based on canvas pixels, not display pixels. The mode's
 * view_scale_x/y fractions are applied to canvas dims instead of display dims.
 *
 * @param mode       Rendering mode (for view_scale_x/y).
 * @param canvas_w   Canvas width in pixels.
 * @param canvas_h   Canvas height in pixels.
 * @param[out] out_view_w  Canvas-adjusted view width.
 * @param[out] out_view_h  Canvas-adjusted view height.
 */
static inline void
u_tiling_compute_canvas_view(const struct xrt_rendering_mode *mode,
                             uint32_t canvas_w,
                             uint32_t canvas_h,
                             uint32_t *out_view_w,
                             uint32_t *out_view_h)
{
	*out_view_w = (uint32_t)(canvas_w * mode->view_scale_x);
	*out_view_h = (uint32_t)(canvas_h * mode->view_scale_y);
	if (*out_view_w == 0)
		*out_view_w = canvas_w;
	if (*out_view_h == 0)
		*out_view_h = canvas_h;
}

#ifdef __cplusplus
}
#endif
