// Copyright 2025, The DisplayXR Project
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
 * For _ext apps: canvas = window client area.
 * For _shared apps: canvas = wherever the app places the shared texture.
 * For _rt apps: canvas = window (runtime-owned).
 *
 * Derived from the window/zone geometry. (The legacy app-facing setter
 * xrSetSharedTextureOutputRectDXR was removed in ADR-031 — a sub-rect is now
 * expressed as one 3D zone via XR_DXR_display_zones.)
 */
struct u_canvas_rect
{
	bool valid;     //!< True if a sub-rect canvas is set (else full window)
	int32_t x;      //!< Left edge in window client-area pixels
	int32_t y;      //!< Top edge in window client-area pixels
	uint32_t w;     //!< Canvas width in pixels
	uint32_t h;     //!< Canvas height in pixels
};

/*!
 * Reframe window metrics to a sub-rect canvas.
 *
 * Overrides the "window" fields in xrt_window_metrics with the canvas sub-rect
 * so Kooima FOV/aspect are computed for that rect, not the full client area.
 * No-op if canvas->valid is false.
 *
 * Used by the display-zones locate path (oxr_session / ipc_server_handler) to
 * frame each 3D zone's off-axis projection to the zone's window-px rect.
 *
 * @param metrics   Window metrics to adjust in-place.
 * @param canvas    Sub-rect to reframe to (e.g. a zone rect).
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
