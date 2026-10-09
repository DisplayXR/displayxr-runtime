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
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * How many view sets this system can ever need: @ref XRT_MAX_SEGMENTS, but no
 * more than the screens a window could actually be woven on — the DP registry
 * entries that carry a display-processor factory — and only where a compositor
 * segments windows at all (@p can_segment: the in-process Vulkan compositor on
 * desktop Linux, never a service session). Never below 1.
 */
static inline uint32_t
oxr_segment_views_set_capacity(uint32_t screens_with_dp_factory, bool can_segment)
{
	if (!can_segment || screens_with_dp_factory < 2) {
		return 1;
	}
	return screens_with_dp_factory < XRT_MAX_SEGMENTS ? screens_with_dp_factory : XRT_MAX_SEGMENTS;
}

/*!
 * The view count `PRIMARY_MULTIVIEW_DXR` reports: the device max per view set,
 * times the system's set capacity (@ref oxr_segment_views_set_capacity), capped
 * at @ref XRT_MAX_VIEWS. On a system that can never split a window (one screen
 * with a DP, or no segmenting compositor) this is the device max — exactly the
 * pre-M3 count. A session with one segment locates what it did before and
 * aliases any larger tail.
 */
static inline uint32_t
oxr_segment_views_multiview_count(uint32_t device_max_views, uint32_t set_capacity)
{
	if (device_max_views == 0) {
		return 0;
	}
	if (set_capacity == 0) {
		set_capacity = 1;
	}
	uint32_t n = device_max_views * set_capacity;
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
	//! Size of that union, reference metres: the whole window's extent. A
	//! display rig maps the WINDOW's height to its virtual display height, so
	//! each segment gets that height scaled by its share (one m2v for all).
	float window_ref_w, window_ref_h;
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
 * The factor a window-centred display rig's virtual display height takes for
 * segment @p i: its height over the whole window's (`h_seg / union_h`), so
 * `m2v = vH * factor / h_seg = vH / union_h` is the SAME for every segment —
 * stacked screens are not magnified 2x per half, and side-by-side panels of
 * different pitch meet at the seam without a gap or an overlap. 1 when the
 * union is unknown.
 */
static inline float
oxr_segment_views_vdh_scale(const struct oxr_segment_layout *l, uint32_t i)
{
	if (l == NULL || i >= l->count || l->window_ref_h <= 0.0f) {
		return 1.0f;
	}
	return l->seg[i].h_m / l->window_ref_h;
}

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
 * Does a screen's DP eye set count? Exactly the single-screen rule: any VALID
 * set with at least one eye, tracked or not (an untracked DP still reports its
 * nominal-viewer eyes — sim_display does, N of them for a Quad mode). Only a
 * screen with no DP eyes at all falls back to its registry nominal viewer.
 */
static inline bool
oxr_segment_views_accept_eyes(bool have_eyes, bool valid, uint32_t count)
{
	return have_eyes && valid && count > 0;
}

/*!
 * Segment @p s's own DP eyes, carried into the reference frame by (@p dx, @p dy).
 *
 * Used when the screen's DP reported a valid set (tracked or not, like the
 * single-screen path) with at least @p views_needed eyes — one per view the
 * segment carries. A DP that under-reports (a sim_display DP made for a
 * screen while another vendor is active once reported 1 eye: every view of
 * that segment then came from the SAME eye and an anaglyph looked flat) is
 * not used; the caller falls back to that screen's nominal viewer, which
 * fills every view.
 *
 * @return true when @p out holds the segment's DP eyes.
 */
static inline bool
oxr_segment_views_segment_eyes(
    const struct xrt_segment_metric *s, float dx, float dy, uint32_t views_needed, struct xrt_eye_positions *out)
{
	if (s == NULL || out == NULL || !oxr_segment_views_accept_eyes(s->have_eyes, s->eyes.valid, s->eyes.count) ||
	    s->eyes.count < (views_needed > 0 ? views_needed : 1)) {
		return false;
	}
	*out = s->eyes;
	for (uint32_t e = 0; e < out->count && e < XRT_MAX_VIEWS; e++) {
		out->eyes[e].x += dx;
		out->eyes[e].y += dy;
	}
	return true;
}

/*!
 * The untracked eye set: one eye per active view (at least 2), in front of the
 * screen centre at @p z, translated by (@p dx, @p dy) into the caller's frame.
 * The pair is (-ipd/2, +ipd/2); views past 2 repeat it column by column (even
 * views left, odd views right), so a 4-view mode gets a valid frustum for every
 * view instead of zero FOVs. Two views = exactly the pre-M3 nominal pair.
 *
 * @return the number of eyes written (<= XRT_MAX_VIEWS).
 */
static inline uint32_t
oxr_segment_views_nominal_eyes(
    float ipd, float z, uint32_t active_view_count, float dx, float dy, struct xrt_eye_position *out)
{
	uint32_t n = active_view_count < 2 ? 2 : active_view_count;
	if (n > XRT_MAX_VIEWS) {
		n = XRT_MAX_VIEWS;
	}
	for (uint32_t i = 0; i < n; i++) {
		out[i].x = ((i % 2) == 0 ? -ipd / 2.0f : ipd / 2.0f) + dx;
		out[i].y = dy;
		out[i].z = z;
	}
	return n;
}

/*!
 * The per-frame routing record (xrLocateViews -> xrEndFrame). Only a locate
 * that SPLIT the views records; every other locate in the frame (a zone-scoped
 * or camera-rig locate, a locate that kept one view set) leaves the record as
 * it is, so it cannot erase the routing of the projection the app rendered per
 * segment. xrEndFrame takes the record and resets it for the next frame.
 */
static inline void
oxr_segment_views_route_record(struct xrt_segment_view_routing *frame, const struct xrt_segment_view_routing *r)
{
	*frame = *r;
}

static inline void
oxr_segment_views_route_take(struct xrt_segment_view_routing *frame, struct xrt_segment_view_routing *out)
{
	*out = *frame;
	memset(frame, 0, sizeof(*frame));
}

/*!
 * Must the routing @p next be sent to an out-of-process compositor that last
 * received @p sent? (Multi-screen M3 over IPC, ADR-047 Amendment 2: the
 * service keeps the last routing it was given, so the client sends it only on
 * a change — a window that stays split costs no round trip per frame, and the
 * first unrouted frame after a split is sent once, which clears it.)
 *
 * Compares only the fields that mean something for @p next's count: entries
 * past the count are don't-care (the take resets them, but a sender must not
 * re-send because of them).
 */
static inline bool
oxr_segment_views_route_differs(const struct xrt_segment_view_routing *sent,
                                const struct xrt_segment_view_routing *next)
{
	if (sent->count != next->count) {
		return true;
	}
	if (next->count == 0) {
		return false;
	}
	if (memcmp(&sent->canvas, &next->canvas, sizeof(next->canvas)) != 0) {
		return true;
	}
	for (uint32_t k = 0; k < next->count && k < XRT_MAX_SEGMENTS; k++) {
		if (sent->screen_id[k] != next->screen_id[k] || sent->first_view[k] != next->first_view[k] ||
		    sent->view_count[k] != next->view_count[k] ||
		    memcmp(&sent->rect[k], &next->rect[k], sizeof(next->rect[k])) != 0) {
			return true;
		}
	}
	return false;
}

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
