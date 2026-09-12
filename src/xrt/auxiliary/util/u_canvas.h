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
 * Pixels a rect's far edge hangs past an extent (0 when it fits).
 */
static inline int64_t
u_canvas_overhang(int64_t far_edge, uint32_t extent)
{
	return far_edge > (int64_t)extent ? far_edge - (int64_t)extent : 0;
}

/*!
 * Resolve the pixel dims of the canvas a zone rect is expressed in.
 *
 * A zone arrives in the window-client space of the CURRENT orientation, while
 * the metrics source may report a FIXED one — a vendor DP (Leia) does, and the
 * live present-target extent can lag a rotation (#528). The zone is a sub-rect
 * of its canvas in the true orientation, so the reported dims are transposed
 * when — and only when — that reduces how far the zone hangs off the canvas.
 *
 * This never rejects a zone. A rect that hangs off the bottom or right edge in
 * BOTH orientations is a partially visible window — a tile mid-scroll, or a
 * page whose layout viewport has drifted from its visual one — and the
 * projection it needs is still the zone's own. Reframing to the full canvas
 * instead was #1458: a square tile drawn with the whole panel's frustum, ~2.2x
 * too wide on a portrait phone. u_canvas_apply_to_metrics() does not need the
 * rect inside the canvas; windowed weaving never did.
 *
 * @param reported_w  Canvas width the metrics source reports, px.
 * @param reported_h  Canvas height the metrics source reports, px.
 * @param zone        The zone rect, window-client px of the current orientation.
 * @param[out] out_w  Canvas width to use, px (reported or transposed).
 * @param[out] out_h  Canvas height to use, px.
 * @return true when the dims were transposed relative to the reported ones.
 */
static inline bool
u_canvas_zone_canvas_dims(
    uint32_t reported_w, uint32_t reported_h, const struct u_canvas_rect *zone, uint32_t *out_w, uint32_t *out_h)
{
	*out_w = reported_w;
	*out_h = reported_h;
	if (!zone->valid || zone->w == 0 || zone->h == 0 || reported_w == 0 || reported_h == 0) {
		return false;
	}
	const int64_t right = (int64_t)zone->x + (int64_t)zone->w;
	const int64_t bottom = (int64_t)zone->y + (int64_t)zone->h;
	const int64_t over_reported = u_canvas_overhang(right, reported_w) + u_canvas_overhang(bottom, reported_h);
	const int64_t over_transposed = u_canvas_overhang(right, reported_h) + u_canvas_overhang(bottom, reported_w);
	// Strictly less: a tie (square canvas, or a zone that overhangs both the
	// same) keeps what the source reported, so the choice is stable frame to frame.
	if (over_transposed < over_reported) {
		*out_w = reported_h;
		*out_h = reported_w;
		return true;
	}
	return false;
}

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
