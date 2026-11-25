// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The pure geometry of the Metal segment manager (comp_metal_segments):
 *         cutting a window into segments in POINTS, converting a segment to the
 *         drawable's BACKING px, and the view-size step (ADR-047 Amendment 10).
 * @ingroup comp_metal
 *
 * Header-only, no Metal, no AppKit, no platform guard, so it is unit-tested
 * on every platform (tests_comp_metal_segments_geom) — notably for a window
 * straddling a 2x (Retina) and a 1x display.
 *
 * **The rule this file exists to pin.** On macOS the segment table is cut in
 * points (a seam is exact whatever the scales), but every pixel quantity the
 * compositor and the app share is in the DRAWABLE's backing px: the drawable
 * follows the backing scale of the screen holding most of the window (2x on
 * a Retina panel, 1x on a plain external one), so a window straddling the two
 * has ONE drawable at ONE scale. Hence:
 *   - a segment's canvas (and so its crop out of the atlas) is its point rect
 *     x (drawable px / window points) — never x its own screen's scale;
 *   - the effective table of ADR-047 Amendment 10 is evaluated against the
 *     drawable's size (the clamp to the instance's worst-case atlas compares
 *     `window px x scale x tiles` with an atlas in px) — never the window's
 *     size in points, which on a 2x display would halve what the atlas needs.
 * The Linux Wayland compositor had this bug class (segments cut in the
 * monitor's px while the buffer was at another scale, #2791).
 */

#pragma once

#include "util/comp_segments.h"
#include "util/comp_segments_view_size.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Where the window is this frame (see comp_metal_segments.h, which uses it).
 */
struct comp_metal_seg_window
{
	//! The content view in top-down global POINTS (origin = the main
	//! display's top-left, the CGDisplayBounds space), rounded to whole points.
	//! With @ref px_per_pt set: in that many px per point instead (see there).
	struct comp_seg_rect frame_pt;
	//! The drawable (the target), px.
	uint32_t drawable_w;
	uint32_t drawable_h;
	/*!
	 * 0 (the in-process compositor): @ref frame_pt is in points. > 0 (the
	 * macOS service's weave, #2218 item 6): @ref frame_pt is the window in
	 * global CG BACKING px at this scale (its windowOriginOnScreen and client
	 * size, unrounded to points), and the table is cut in that px frame — each
	 * screen's point rect x this scale, edges rounded like the engine rounds a
	 * display's backing origin — so a present origin is exact even for a
	 * window at a half-point position (util/comp_weave_screens.h,
	 * comp_weave_screens_project_cg, is the same placement).
	 */
	double px_per_pt;
};

//! Round a table-unit edge to drawable px, clamped to [0, max].
static inline int32_t
comp_metal_seg_edge_px(double v_tu, double scale, uint32_t max)
{
	double v = floor(v_tu * scale + 0.5);
	if (v < 0.0) {
		v = 0.0;
	}
	if (v > (double)max) {
		v = (double)max;
	}
	return (int32_t)v;
}

/*!
 * Cut the window into segments, in table units: points (in-process), or the
 * service's px frame when @p win carries px_per_pt (each screen's point rect
 * x that scale, edges rounded independently so neighbours share the seam).
 * @p screens are in points.
 */
static inline void
comp_metal_seg_cut(const struct comp_metal_seg_window *win,
                   const struct comp_segments_screen *screens,
                   uint32_t screen_count,
                   struct comp_segment_table *out)
{
	if (win->px_per_pt > 0.0) {
		struct comp_segments_screen px[COMP_SEGMENTS_MAX_SCREENS];
		const uint32_t n = screen_count < COMP_SEGMENTS_MAX_SCREENS ? screen_count : COMP_SEGMENTS_MAX_SCREENS;
		for (uint32_t i = 0; i < n; i++) {
			const struct comp_seg_rect *d = &screens[i].desktop;
			const int32_t x0 = (int32_t)lround((double)d->x * win->px_per_pt);
			const int32_t y0 = (int32_t)lround((double)d->y * win->px_per_pt);
			const int32_t x1 = (int32_t)lround((double)(d->x + (int32_t)d->w) * win->px_per_pt);
			const int32_t y1 = (int32_t)lround((double)(d->y + (int32_t)d->h) * win->px_per_pt);
			px[i] = screens[i];
			// Field by field: this header is also compiled as C++ (no compound literals).
			px[i].desktop.x = x0;
			px[i].desktop.y = y0;
			px[i].desktop.w = (uint32_t)(x1 - x0);
			px[i].desktop.h = (uint32_t)(y1 - y0);
		}
		comp_segments_compute(&win->frame_pt, NULL, px, n, out);
	} else {
		comp_segments_compute(&win->frame_pt, NULL, screens, screen_count, out);
	}
}

/*!
 * A segment's window rect (table units, relative to the window) in the
 * DRAWABLE's px: x (drawable px / window table units), edges rounded
 * independently (neighbours share the seam), clamped to the drawable.
 */
static inline struct comp_seg_rect
comp_metal_seg_window_rect_px(const struct comp_metal_seg_window *win, const struct comp_seg_rect *r_tu)
{
	struct comp_seg_rect o = {0, 0, 0, 0};
	if (win->frame_pt.w == 0 || win->frame_pt.h == 0) {
		return o;
	}
	const double sx = (double)win->drawable_w / (double)win->frame_pt.w;
	const double sy = (double)win->drawable_h / (double)win->frame_pt.h;
	const int32_t x0 = comp_metal_seg_edge_px((double)r_tu->x, sx, win->drawable_w);
	const int32_t x1 = comp_metal_seg_edge_px((double)r_tu->x + r_tu->w, sx, win->drawable_w);
	const int32_t y0 = comp_metal_seg_edge_px((double)r_tu->y, sy, win->drawable_h);
	const int32_t y1 = comp_metal_seg_edge_px((double)r_tu->y + r_tu->h, sy, win->drawable_h);
	o.x = x0;
	o.y = y0;
	o.w = (uint32_t)(x1 - x0);
	o.h = (uint32_t)(y1 - y0);
	return o;
}

/*!
 * ADR-047 Amendment 10, one step of the view size for the Metal manager:
 * advance @p vs with this frame's table @p t (table units, from
 * @ref comp_metal_seg_cut) and recompute the effective table, evaluated
 * against the DRAWABLE's size (backing px), never the window's points.
 *
 * @return true when the effective table changed.
 */
static inline bool
comp_metal_seg_view_size_step(struct comp_segments_view_size *vs,
                              const struct comp_metal_seg_window *win,
                              const struct comp_segment_table *t,
                              const struct comp_segments_screen *screens,
                              uint32_t screen_count,
                              const struct xrt_rendering_mode *head_modes,
                              uint32_t head_count,
                              uint32_t max_views,
                              uint32_t cap_w,
                              uint32_t cap_h,
                              uint64_t now_ns)
{
	return comp_segments_view_size_update(vs, t, screens, screen_count, head_modes, head_count, max_views, cap_w,
	                                      cap_h, win->drawable_w, win->drawable_h, now_ns);
}

/*!
 * How one segment is cut out of the atlas and handed to its DP: its tile
 * rect (drawable px at view scale, inside one atlas tile) and its grid — the
 * session grid for the primary (or a screen without its own table), else its
 * own screen's entry for the app's slot (comp_segments_view_size_segment_grid).
 *
 * @param window_px  The segment's canvas, drawable px (@ref comp_metal_seg_window_rect_px).
 * @param canvas     The whole drawable.
 * @param entry      Its screen's entry for the app's slot (NULL = the session's).
 * @return false when the segment is thinner than one source pixel.
 */
static inline bool
comp_metal_seg_crop_plan(const struct comp_seg_rect *window_px,
                         const struct comp_seg_rect *canvas,
                         uint32_t view_w,
                         uint32_t view_h,
                         uint32_t session_cols,
                         uint32_t session_rows,
                         const struct xrt_rendering_mode *entry,
                         struct comp_seg_rect *out_tile,
                         struct comp_segments_segment_grid *out_grid)
{
	*out_grid = comp_segments_view_size_segment_grid(entry, session_cols, session_rows);
	return comp_segments_tile_rect(window_px, canvas, view_w, view_h, out_tile);
}

#ifdef __cplusplus
}
#endif
