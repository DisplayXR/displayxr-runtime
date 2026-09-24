// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Off-panel 2D on the service weave path: mapping the off-panel bands
 *         onto the woven output (#1654 / #1699).
 * @ingroup aux_util
 *
 * Runtime-only, and deliberately NOT in u_wayland_geom.h: that header is shared
 * byte-for-byte with displayxr-common's Linux app window
 * (test_apps/common/dxr_linux_window_aux_guard.cmake), and this arithmetic is
 * the service engine's alone. The bands themselves still come from
 * u_wl_offpanel_bands() there. Pinned by tests_aux_weave_span2d.
 */

#pragma once

#include "util/u_wayland_geom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Map off-panel bands from WINDOW pixels onto a buffer that shows the window
 * (#1654 on the service weave path, comp_multi_weave_linux.c).
 *
 * The service's woven output is not always the window: for the batch / legacy
 * layouts it is the window client area from its top-left at 1:1 (possibly
 * smaller — a legacy output is only as big as its element's rect), for the v6
 * N-view layout it is one content view, i.e. the window scaled by the mode's
 * view scale. @p scale selects the second case (x by out_w / win_w, y by
 * out_h / win_h); otherwise the bands are clipped to the output unscaled. Each
 * edge is rounded to the nearest pixel, so adjacent bands stay adjacent.
 *
 * @param[out] out        up to four rects in OUTPUT pixels; empty ones dropped
 * @param[out] out_whole  true when the one remaining band is the whole output
 *                        (the window is entirely off the panel), may be NULL
 * @return how many were written.
 *
 * @ingroup aux_util
 */
static inline uint32_t
u_wl_offpanel_bands_to_output(const struct u_wl_rect_px *bands,
                              uint32_t count,
                              uint32_t win_w,
                              uint32_t win_h,
                              uint32_t out_w,
                              uint32_t out_h,
                              bool scale,
                              struct u_wl_rect_px out[4],
                              bool *out_whole)
{
	if (out_whole != NULL) {
		*out_whole = false;
	}
	if (bands == NULL || out == NULL || win_w == 0 || win_h == 0 || out_w == 0 || out_h == 0) {
		return 0;
	}
	const double sx = scale ? (double)out_w / (double)win_w : 1.0;
	const double sy = scale ? (double)out_h / (double)win_h : 1.0;
	uint32_t n = 0;
	for (uint32_t i = 0; i < count && i < 4; i++) {
		int32_t x0 = (int32_t)((double)bands[i].x * sx + 0.5);
		int32_t y0 = (int32_t)((double)bands[i].y * sy + 0.5);
		int32_t x1 = (int32_t)((double)(bands[i].x + bands[i].w) * sx + 0.5);
		int32_t y1 = (int32_t)((double)(bands[i].y + bands[i].h) * sy + 0.5);
		if (x0 < 0) {
			x0 = 0;
		}
		if (y0 < 0) {
			y0 = 0;
		}
		if (x1 > (int32_t)out_w) {
			x1 = (int32_t)out_w;
		}
		if (y1 > (int32_t)out_h) {
			y1 = (int32_t)out_h;
		}
		if (x1 <= x0 || y1 <= y0) {
			continue;
		}
		out[n].x = x0;
		out[n].y = y0;
		out[n].w = x1 - x0;
		out[n].h = y1 - y0;
		n++;
	}
	if (out_whole != NULL) {
		*out_whole = n == 1 && out[0].x == 0 && out[0].y == 0 && out[0].w == (int32_t)out_w &&
		             out[0].h == (int32_t)out_h;
	}
	return n;
}

#ifdef __cplusplus
}
#endif
