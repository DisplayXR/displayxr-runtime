// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-segment views: the pure geometry behind xrLocateViews for a
 *         window that spans screens (multi-screen M3, ADR-047 D3).
 * @ingroup oxr_main
 *
 * A window woven per screen (see docs/architecture/comp-segments.md) gets,
 * under `PRIMARY_MULTIVIEW_DXR`, one view set PER SEGMENT: each located from
 * that screen's eyes, with that segment (relative to its own screen, in its
 * own screen's metres) as the Kooima canvas. Everything here is the arithmetic
 * that turns a compositor segment table into those inputs — no session, no
 * device, so it is unit-tested headless (`tests/tests_oxr_segment_views.cpp`):
 *
 *   - @ref oxr_segment_views_multiview_count — the advertised view count.
 *   - @ref oxr_segment_views_majority — the segment holding most of the window.
 *   - @ref oxr_segment_views_layout — every segment placed in ONE frame (the
 *     majority screen's display space), continuous across the seam.
 *   - @ref oxr_segment_views_window_metrics — a segment as the window metrics
 *     the existing Kooima block consumes.
 *   - @ref oxr_segment_views_assign — the contiguous view ranges.
 */

#pragma once

#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_limits.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The view count `PRIMARY_MULTIVIEW_DXR` reports: the device max per segment,
 * times @ref XRT_MAX_SEGMENTS, capped at @ref XRT_MAX_VIEWS. A session with one
 * segment locates exactly what it did before and aliases the larger tail.
 */
static inline uint32_t
oxr_segment_views_multiview_count(uint32_t device_max_views)
{
	if (device_max_views == 0) {
		return 0;
	}
	uint32_t n = device_max_views * XRT_MAX_SEGMENTS;
	return n > XRT_MAX_VIEWS ? XRT_MAX_VIEWS : n;
}

/*!
 * How many views ONE segment of a session can carry: the per-segment share of
 * the reported count. This is what the #1499 mode floor asks "can this session
 * fill that mode?" against — a multiview session fills any mode on every
 * segment, a stereo session (which never splits) fills 2-view modes.
 */
static inline uint32_t
oxr_segment_views_per_segment_capacity(uint32_t reported_view_count, bool is_multiview, uint32_t device_max_views)
{
	if (!is_multiview || reported_view_count <= device_max_views) {
		return reported_view_count;
	}
	return device_max_views;
}

/*!
 * The segment holding the largest share of the window (area of its window
 * rect). Ties go to the primary screen, then to the leftmost segment.
 * Returns 0 for an empty table.
 */
uint32_t
oxr_segment_views_majority(const struct xrt_segment_metrics *m);

/*!
 * One segment placed in the reference frame.
 *
 * Frames are display spaces: origin at a screen's centre, +X right, +Y up,
 * metres. "own" is the segment's own screen, "ref" the majority screen's.
 */
struct oxr_segment_place
{
	float own_cx, own_cy; //!< Segment centre in its own screen's display space
	float ref_cx, ref_cy; //!< Segment centre in the reference (majority) display space
	float w_m, h_m;       //!< Segment physical size, its own screen's metres
};

/*!
 * Every segment of a table, placed in one frame.
 */
struct oxr_segment_layout
{
	uint32_t count;
	uint32_t majority;
	struct oxr_segment_place seg[XRT_MAX_SEGMENTS];
	//! Centre of the union of all segments in the reference frame — where the
	//! whole window's centre sits, so a window-centred rig (XR_DXR_view_rig's
	//! display rig) can be offset per segment.
	float window_ref_cx, window_ref_cy;
};

/*!
 * Place every segment of @p m in the majority screen's display space.
 *
 * The majority segment keeps its true position. Every other segment is put
 * where the window's pixels continue across the seam: along each axis, the
 * distance from the majority centre is measured in the majority screen's pixel
 * pitch up to the majority segment's edge and in the segment's OWN pitch beyond
 * it, so two segments that share a seam in window pixels also share it in
 * metres (no gap, no overlap) even when the two panels' pitches differ.
 *
 * @return false when any segment lacks a physical size or a desktop rect (the
 *         caller then keeps the single view set).
 */
bool
oxr_segment_views_layout(const struct xrt_segment_metrics *m, struct oxr_segment_layout *out);

/*!
 * Segment @p i as the window metrics the session's Kooima block consumes:
 * canvas = the segment in its own screen's metres, its centre at
 * oxr_segment_place::ref_cx/ref_cy (the window offset of the reference frame),
 * display = segment @p i's own screen (so XrViewDisplayRawDXR::canvasRectPx
 * reports the segment relative to its own screen).
 */
void
oxr_segment_views_window_metrics(const struct xrt_segment_metrics *m,
                                 const struct oxr_segment_layout *l,
                                 uint32_t i,
                                 struct xrt_window_metrics *out);

/*!
 * The WHOLE window as window metrics relative to segment @p i's screen — what a
 * `PRIMARY_STEREO` session spanning screens locates against: its two views stay
 * one set for the whole window (the compositor crops them per segment, M2),
 * framed from the majority screen.
 */
void
oxr_segment_views_whole_window_metrics(const struct xrt_segment_metrics *m, uint32_t i, struct xrt_window_metrics *out);

/*!
 * The translation that carries a point in segment @p i's own display space into
 * the reference frame (x, y; z is shared).
 */
static inline void
oxr_segment_views_own_to_ref(const struct oxr_segment_layout *l, uint32_t i, float *out_dx, float *out_dy)
{
	*out_dx = l->seg[i].ref_cx - l->seg[i].own_cx;
	*out_dy = l->seg[i].ref_cy - l->seg[i].own_cy;
}

/*!
 * Contiguous view ranges: segment k gets `per_segment` views starting at
 * `k * per_segment`.
 *
 * @return the total (= the session's activeViewCount), or 0 when it does not
 *         fit in @p reported_view_count (the caller keeps one view set).
 */
uint32_t
oxr_segment_views_assign(uint32_t segment_count,
                         uint32_t per_segment,
                         uint32_t reported_view_count,
                         uint32_t out_first[XRT_MAX_SEGMENTS],
                         uint32_t out_count[XRT_MAX_SEGMENTS]);

/*!
 * Which located view view @p i carries: itself while active, view 0 for the
 * inactive tail `[active_view_count, reported)` — the ADR-041 alias, the same
 * with one view set or one per segment.
 */
static inline uint32_t
oxr_segment_views_alias_source(uint32_t i, uint32_t active_view_count)
{
	return i < active_view_count ? i : 0;
}

#ifdef __cplusplus
}
#endif
