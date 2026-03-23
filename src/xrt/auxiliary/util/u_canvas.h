// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header-only canvas utilities for shared-texture output rect.
 * @author David Fattal
 * @ingroup aux_util
 *
 * For _shared apps, the 3D canvas (output rect) may be a sub-rect of the
 * app's window. View dimensions and Kooima projection must be based on
 * canvas size, not display size. This file provides shared utilities
 * so all compositors apply canvas logic identically.
 */

#pragma once

#include "xrt/xrt_display_metrics.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Canvas output rect — the sub-rect of the app's window where 3D content appears.
 *
 * For _ext apps: canvas = window client area (no output_rect needed).
 * For _shared apps: canvas = wherever the app places the shared texture.
 * For _rt apps: canvas = window (runtime-owned).
 *
 * Set via xrSetSharedTextureOutputRectEXT (part of XR_EXT_win32_window_binding
 * and XR_EXT_cocoa_window_binding).
 */
struct u_canvas_rect
{
	bool valid;     //!< True if output rect has been set
	int32_t x;      //!< Left edge in window client-area pixels
	int32_t y;      //!< Top edge in window client-area pixels
	uint32_t w;     //!< Canvas width in pixels
	uint32_t h;     //!< Canvas height in pixels
};

/*!
 * Apply canvas output rect to window metrics.
 *
 * When a shared-texture app has set an output rect, the "window" fields in
 * xrt_window_metrics should reflect the canvas (output rect), not the full
 * window client area. This ensures Kooima FOV uses the correct aspect ratio.
 *
 * Call this after populating raw window metrics from the OS, before returning
 * from get_window_metrics(). No-op if canvas->valid is false.
 *
 * @param metrics   Window metrics to adjust in-place.
 * @param canvas    Canvas output rect (from xrSetSharedTextureOutputRectEXT).
 */
static inline void
u_canvas_apply_to_metrics(struct xrt_window_metrics *metrics,
                          const struct u_canvas_rect *canvas)
{
	if (!canvas->valid || canvas->w == 0 || canvas->h == 0) {
		return;
	}
	if (metrics->display_pixel_width == 0 || metrics->display_pixel_height == 0) {
		return;
	}

	float pixel_size_x = metrics->display_width_m / (float)metrics->display_pixel_width;
	float pixel_size_y = metrics->display_height_m / (float)metrics->display_pixel_height;

	// Override window fields with canvas dims
	metrics->window_pixel_width = canvas->w;
	metrics->window_pixel_height = canvas->h;
	metrics->window_screen_left += canvas->x;
	metrics->window_screen_top += canvas->y;

	metrics->window_width_m = (float)canvas->w * pixel_size_x;
	metrics->window_height_m = (float)canvas->h * pixel_size_y;

	// Recompute center offset relative to canvas center
	float canvas_center_px_x = (float)(metrics->window_screen_left - metrics->display_screen_left)
	                           + (float)canvas->w / 2.0f;
	float canvas_center_px_y = (float)(metrics->window_screen_top - metrics->display_screen_top)
	                           + (float)canvas->h / 2.0f;
	float disp_center_px_x = (float)metrics->display_pixel_width / 2.0f;
	float disp_center_px_y = (float)metrics->display_pixel_height / 2.0f;

	metrics->window_center_offset_x_m = (canvas_center_px_x - disp_center_px_x) * pixel_size_x;
	metrics->window_center_offset_y_m = -((canvas_center_px_y - disp_center_px_y) * pixel_size_y);
}

#ifdef __cplusplus
}
#endif
