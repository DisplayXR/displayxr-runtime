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

#ifdef __cplusplus
}
#endif
