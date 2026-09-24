// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pure rect arithmetic for the desktop-Linux XR_DXR_weave service's
 *         flat regions (spec v8, browser#88) — runtime-only.
 *
 * Kept out of u_wayland_geom.h on purpose: that header is shared byte-for-byte
 * with displayxr-common's Linux app window (dxr_linux_window_aux_guard.cmake),
 * and this arithmetic is the service's alone (like u_weave_span2d.h).
 *
 * @ingroup aux_util
 */

#pragma once

#include "util/u_weave_span2d.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Map the weave caller's FLAT regions (XR_DXR_weave spec v8, browser#88) onto
 * the woven output (desktop-Linux service weave, comp_multi_weave_linux.c).
 *
 * Two sources, composed by union (the output is simply the list of both):
 *  - @p win_rects: the per-submit list, WINDOW-relative device px, y-down;
 *  - @p screen_rects: the sticky latch, ABSOLUTE screen device px, made
 *    window-relative by subtracting (@p win_x, @p win_y).
 * Each is clipped to the window client area [0, @p win_w) x [0, @p win_h)
 * (a sticky rect naming screen area outside the window is clipped away, not
 * an error), then mapped to the output exactly as
 * @ref u_wl_offpanel_bands_to_output maps bands: 1:1 and clipped for the batch
 * / legacy layouts, scaled by out/win when @p scale (the v6 N-view layout,
 * whose output is one content view). Empty results are dropped; at most
 * @p out_cap are written.
 *
 * @return how many rects were written to @p out.
 *
 * @ingroup aux_util
 */
static inline uint32_t
u_wl_flat_rects_to_output(const struct u_wl_rect_px *win_rects,
                          uint32_t win_count,
                          const struct u_wl_rect_px *screen_rects,
                          uint32_t screen_count,
                          int32_t win_x,
                          int32_t win_y,
                          uint32_t win_w,
                          uint32_t win_h,
                          uint32_t out_w,
                          uint32_t out_h,
                          bool scale,
                          struct u_wl_rect_px *out,
                          uint32_t out_cap)
{
	if (out == NULL || out_cap == 0 || win_w == 0 || win_h == 0 || out_w == 0 || out_h == 0) {
		return 0;
	}
	const double sx = scale ? (double)out_w / (double)win_w : 1.0;
	const double sy = scale ? (double)out_h / (double)win_h : 1.0;
	uint32_t n = 0;
	const uint32_t total = win_count + screen_count;
	for (uint32_t i = 0; i < total && n < out_cap; i++) {
		struct u_wl_rect_px r;
		if (i < win_count) {
			if (win_rects == NULL) {
				continue;
			}
			r = win_rects[i];
		} else {
			if (screen_rects == NULL) {
				continue;
			}
			r = screen_rects[i - win_count];
			r.x -= win_x;
			r.y -= win_y;
		}
		if (r.w <= 0 || r.h <= 0) {
			continue;
		}
		// Clip to the window, in 64-bit so a hostile extent cannot wrap.
		int64_t x0 = r.x, y0 = r.y;
		int64_t x1 = (int64_t)r.x + r.w, y1 = (int64_t)r.y + r.h;
		if (x0 < 0) {
			x0 = 0;
		}
		if (y0 < 0) {
			y0 = 0;
		}
		if (x1 > (int64_t)win_w) {
			x1 = (int64_t)win_w;
		}
		if (y1 > (int64_t)win_h) {
			y1 = (int64_t)win_h;
		}
		if (x1 <= x0 || y1 <= y0) {
			continue;
		}
		// Window px -> output px.
		int32_t ox0 = (int32_t)((double)x0 * sx + 0.5);
		int32_t oy0 = (int32_t)((double)y0 * sy + 0.5);
		int32_t ox1 = (int32_t)((double)x1 * sx + 0.5);
		int32_t oy1 = (int32_t)((double)y1 * sy + 0.5);
		if (ox1 > (int32_t)out_w) {
			ox1 = (int32_t)out_w;
		}
		if (oy1 > (int32_t)out_h) {
			oy1 = (int32_t)out_h;
		}
		if (ox1 <= ox0 || oy1 <= oy0) {
			continue;
		}
		out[n].x = ox0;
		out[n].y = oy0;
		out[n].w = ox1 - ox0;
		out[n].h = oy1 - oy0;
		n++;
	}
	return n;
}

#ifdef __cplusplus
}
#endif
