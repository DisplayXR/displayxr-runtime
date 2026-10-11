// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave v19 (#1884): split each submitted weave rect into its
 *         per-screen parts.
 * @ingroup aux_util
 *
 * A present-owner's bound window can straddle a seam between two 3D screens.
 * The D3D11 service then weaves the window per screen (ADR-047 Amendment 4):
 * each screen's segment (window ∩ screen) by that screen's display processor.
 * The service hands the client its segment table (@ref xrt_segment_metrics, the
 * same table per-segment views use); the client's state tracker cuts every rect
 * it submitted at the table's seams, so the caller knows which pixels of which
 * rect to render from which screen's eyes. The rects never cross the wire twice.
 *
 * Header-only and backend-agnostic, so it is host-tested
 * (`tests/tests_weave_segments.cpp`).
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_display_metrics.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * One part of one weave rect: the pixels of rect @ref rect_index that land on
 * segment @ref segment_index's screen.
 */
struct u_weave_rect_part
{
	uint32_t rect_index;         //!< Index into the submitted rects
	uint32_t segment_index;      //!< Index into xrt_segment_metrics::seg
	struct xrt_rect window_rect; //!< The part, window px
	struct xrt_rect rect_rel;    //!< The part, relative to its rect's top-left
	struct xrt_rect screen_rect; //!< The part, relative to its screen's top-left
};

/*!
 * Intersect two rects (half-open). @return false when they do not overlap.
 */
static inline bool
u_weave_rect_intersect(const struct xrt_rect *a, const struct xrt_rect *b, struct xrt_rect *out)
{
	if (a->extent.w <= 0 || a->extent.h <= 0 || b->extent.w <= 0 || b->extent.h <= 0) {
		return false;
	}
	const int64_t ax1 = (int64_t)a->offset.w + a->extent.w;
	const int64_t ay1 = (int64_t)a->offset.h + a->extent.h;
	const int64_t bx1 = (int64_t)b->offset.w + b->extent.w;
	const int64_t by1 = (int64_t)b->offset.h + b->extent.h;
	const int64_t x0 = a->offset.w > b->offset.w ? a->offset.w : b->offset.w;
	const int64_t y0 = a->offset.h > b->offset.h ? a->offset.h : b->offset.h;
	const int64_t x1 = ax1 < bx1 ? ax1 : bx1;
	const int64_t y1 = ay1 < by1 ? ay1 : by1;
	if (x1 <= x0 || y1 <= y0) {
		return false;
	}
	out->offset.w = (int)x0;
	out->offset.h = (int)y0;
	out->extent.w = (int)(x1 - x0);
	out->extent.h = (int)(y1 - y0);
	return true;
}

/*!
 * Split @p rects at the seams of segment table @p m.
 *
 * Parts are emitted per rect in submission order, and within a rect in the
 * table's order (left to right). A rect wholly on one segment yields one part
 * equal to the rect; a rect on no segment (off every screen) yields none; an
 * empty rect yields none.
 *
 * A table with @c count 0 (the window is woven by one display processor) or
 * a count past XRT_MAX_SEGMENTS yields no parts at all: the caller then keeps
 * one eye set for everything, which is the pre-split contract.
 *
 * @return the number of parts written (excess dropped at @p cap).
 */
static inline uint32_t
u_weave_rect_parts(const struct xrt_segment_metrics *m,
                   const struct xrt_rect *rects,
                   uint32_t rect_count,
                   struct u_weave_rect_part *out,
                   uint32_t cap)
{
	if (m == NULL || rects == NULL || out == NULL || m->count == 0 || m->count > XRT_MAX_SEGMENTS) {
		return 0;
	}
	uint32_t n = 0;
	for (uint32_t r = 0; r < rect_count; r++) {
		for (uint32_t k = 0; k < m->count; k++) {
			const struct xrt_segment_metric *s = &m->seg[k];
			struct xrt_rect isect;
			if (!u_weave_rect_intersect(&rects[r], &s->window_rect, &isect)) {
				continue;
			}
			if (n >= cap) {
				return n;
			}
			struct u_weave_rect_part *p = &out[n++];
			p->rect_index = r;
			p->segment_index = k;
			p->window_rect = isect;
			p->rect_rel.offset.w = isect.offset.w - rects[r].offset.w;
			p->rect_rel.offset.h = isect.offset.h - rects[r].offset.h;
			p->rect_rel.extent = isect.extent;
			// The segment's window rect and screen rect are the same pixels:
			// shift by the part's offset inside the segment.
			p->screen_rect.offset.w = s->screen_rect.offset.w + (isect.offset.w - s->window_rect.offset.w);
			p->screen_rect.offset.h = s->screen_rect.offset.h + (isect.offset.h - s->window_rect.offset.h);
			p->screen_rect.extent = isect.extent;
		}
	}
	return n;
}

/*!
 * Must the state tracker fetch the per-screen table after a submit
 * (`weave_get_segments`)? Only for a batch / v6 submit (@p rect_count > 0; the
 * legacy single-rect submit is never segmented) whose weave the service reports
 * as per screen: @p submit_segment_count is the count `weave_submit`'s reply
 * carries (the table that very submit published). A window on one screen — and
 * every single-screen box, which never enables segments — reports 0, so a v19
 * caller chaining XrWeaveOutputRectPartsDXR pays no extra round trip there.
 */
static inline bool
u_weave_segments_fetch_needed(uint32_t rect_count, uint32_t submit_segment_count)
{
	return rect_count > 0 && submit_segment_count > 0 && submit_segment_count <= XRT_MAX_SEGMENTS;
}

/*!
 * The segment of table @p m whose screen holds ALL of @p rect (window px), or
 * -1 when the rect lands on no segment or on more than one (it straddles a
 * seam). The parts rule of @ref u_weave_rect_parts: a rect for which this
 * answers k is exactly the one part of segment k.
 */
static inline int32_t
u_weave_rect_single_segment(const struct xrt_segment_metrics *m, const struct xrt_rect *rect)
{
	if (m == NULL || rect == NULL || m->count == 0 || m->count > XRT_MAX_SEGMENTS) {
		return -1;
	}
	int32_t found = -1;
	for (uint32_t k = 0; k < m->count; k++) {
		struct xrt_rect isect;
		if (!u_weave_rect_intersect(rect, &m->seg[k].window_rect, &isect)) {
			continue;
		}
		if (found >= 0) {
			return -1; // a second screen: the rect straddles a seam
		}
		found = (int32_t)k;
	}
	return found;
}

/*!
 * Which screen frames a zone-scoped locate (an inline-3D element) of a
 * present-owner whose bound window is woven per screen (ADR-047 Amendment 4,
 * addendum): the segment index whose screen's eyes and geometry the element's
 * views must come from, or -1 to keep the session's own (the primary screen's)
 * framing, which is the pre-addendum answer.
 *
 * Re-framed only when every one of these holds:
 *  - @p m is a per-screen table (count 1..XRT_MAX_SEGMENTS; count 1 is a
 *    window wholly on a non-primary screen);
 *  - it was cut from a window of the live size @p live_w x @p live_h (a resize
 *    in flight keeps the old framing for that locate);
 *  - the element lies wholly on ONE segment (@ref u_weave_rect_single_segment)
 *    — an element that straddles a seam keeps the primary's views, because
 *    rendering it per part needs a second view set the page cannot take yet
 *    (displayxr-browser-pvt#272);
 *  - that segment is not the primary screen's (the session's own framing
 *    already is that screen);
 *  - its screen's physical and desktop sizes are known (the Kooima needs them).
 */
static inline int32_t
u_weave_zone_screen_segment(const struct xrt_segment_metrics *m,
                            const struct xrt_rect *zone,
                            uint32_t live_w,
                            uint32_t live_h)
{
	if (m == NULL || zone == NULL || m->count == 0 || m->count > XRT_MAX_SEGMENTS) {
		return -1;
	}
	if (m->window_pixel_width != live_w || m->window_pixel_height != live_h) {
		return -1;
	}
	const int32_t k = u_weave_rect_single_segment(m, zone);
	if (k < 0) {
		return -1;
	}
	const struct xrt_segment_metric *s = &m->seg[k];
	if (s->is_primary || s->screen_width_m <= 0.0f || s->screen_height_m <= 0.0f || s->screen_desktop_width == 0 ||
	    s->screen_desktop_height == 0) {
		return -1;
	}
	return k;
}

/*!
 * The WHOLE window of table @p m as window metrics relative to segment @p k's
 * screen: that screen's display rect and physical size, the window's client
 * area in desktop px, the window's metres at that screen's pixel pitch and its
 * centre in that screen's display space. A zone rebase
 * (u_canvas_apply_to_metrics) then places an element on that screen exactly as
 * it places one on the primary from the session's own window metrics.
 *
 * The same arithmetic as oxr_segment_views_whole_window_metrics (cross-checked
 * in tests/tests_weave_segments.cpp); here because the IPC server, which frames
 * a present-owner's locates, cannot link the state tracker.
 *
 * @return false (and @p out zeroed) when the table or the screen lacks a size.
 */
static inline bool
u_weave_segment_window_metrics(const struct xrt_segment_metrics *m, uint32_t k, struct xrt_window_metrics *out)
{
	memset(out, 0, sizeof(*out));
	if (m == NULL || k >= m->count || k >= XRT_MAX_SEGMENTS || m->window_pixel_width == 0 ||
	    m->window_pixel_height == 0) {
		return false;
	}
	const struct xrt_segment_metric *s = &m->seg[k];
	if (s->screen_desktop_width == 0 || s->screen_desktop_height == 0 || s->screen_width_m <= 0.0f ||
	    s->screen_height_m <= 0.0f) {
		return false;
	}
	const float px = s->screen_width_m / (float)s->screen_desktop_width;
	const float py = s->screen_height_m / (float)s->screen_desktop_height;
	out->display_width_m = s->screen_width_m;
	out->display_height_m = s->screen_height_m;
	out->display_pixel_width = s->screen_desktop_width;
	out->display_pixel_height = s->screen_desktop_height;
	out->display_screen_left = s->screen_desktop_left;
	out->display_screen_top = s->screen_desktop_top;
	out->window_orientation.x = 0.0f;
	out->window_orientation.y = 0.0f;
	out->window_orientation.z = 0.0f;
	out->window_orientation.w = 1.0f;
	out->window_pixel_width = m->window_pixel_width;
	out->window_pixel_height = m->window_pixel_height;
	out->window_screen_left = m->window_screen_left;
	out->window_screen_top = m->window_screen_top;
	out->window_width_m = (float)m->window_pixel_width * px;
	out->window_height_m = (float)m->window_pixel_height * py;
	const float wcx = (float)(m->window_screen_left - s->screen_desktop_left) + (float)m->window_pixel_width * 0.5f;
	const float wcy = (float)(m->window_screen_top - s->screen_desktop_top) + (float)m->window_pixel_height * 0.5f;
	out->window_center_offset_x_m = (wcx - (float)s->screen_desktop_width * 0.5f) * px;
	out->window_center_offset_y_m = -(wcy - (float)s->screen_desktop_height * 0.5f) * py;
	out->valid = true;
	return true;
}

#ifdef __cplusplus
}
#endif
