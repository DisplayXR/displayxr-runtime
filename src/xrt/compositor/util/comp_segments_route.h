// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-segment view routing and segment-table publishing — the pure
 *         halves a compositor needs to carry multi-screen M3 (ADR-047 D3).
 * @ingroup comp_util
 *
 * Shared by every compositor that segments a window and routes per-segment
 * views into its atlas. Written for the D3D11 service compositor (the
 * service / IPC path, where the segment table crosses the process boundary
 * and the routing comes back over IPC), deliberately backend-agnostic and
 * header-only so it is host-tested (`tests/tests_segments_ipc.cpp`):
 *
 *   - @ref comp_segments_route_place — where one submitted view of a routed
 *     projection layer lands in the atlas (which tile, which rect in it).
 *   - @ref comp_segments_publish — the change-only generation bump of the
 *     published segment table.
 *
 * The rect mapping is @ref comp_segments_tile_rect — the one the per-segment
 * crop (M2) reads with — so a routed view always lands exactly where its
 * segment's crop picks it up. Design: docs/architecture/comp-segments.md.
 */
#pragma once

#include "xrt/xrt_display_metrics.h"
#include "util/comp_segments.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Where a routed view goes.
 */
struct comp_segments_route_slot
{
	uint32_t segment;          //!< Which segment of the routing the view belongs to
	uint32_t tile;             //!< The atlas tile it is painted into (its LOCAL view index)
	struct comp_seg_rect rect; //!< The segment's rect inside that tile, tile px
};

/*!
 * Place submitted view @p view of a projection layer under @p r.
 *
 * Segment k's local view j (layer view `r->first_view[k] + j`) is painted into
 * tile j at segment k's rect inside the tile — the mosaic: the atlas keeps the
 * grid of the active mode, each tile holds every segment's j-th view side by
 * side. The routing is honoured only while it is consistent with the frame:
 * a segment's range must lie inside what the layer submitted, its tiles must
 * exist (@p tile_count, the active mode's view count — it can change between
 * the locate and the commit across a 2D/3D toggle, in which case only the
 * views the tiles can hold are routed) and every rect must be non-degenerate.
 *
 * @param r                  The routing the client's last splitting locate handed out.
 * @param layer_view_count   The views the projection layer submitted.
 * @param tile_count         The atlas tiles of the active mode (cols x rows).
 * @param tile_w, tile_h     One view's CONTENT size in the atlas (canvas x mode scale).
 * @param view               The submitted view to place.
 * @param[out] out           Filled when the view is routed.
 * @return false when the frame is unrouted (count 0, or any inconsistency —
 *         the caller then places every view as it always did), or when this
 *         view is past every segment's routed range.
 */
static inline bool
comp_segments_route_place(const struct xrt_segment_view_routing *r,
                          uint32_t layer_view_count,
                          uint32_t tile_count,
                          uint32_t tile_w,
                          uint32_t tile_h,
                          uint32_t view,
                          struct comp_segments_route_slot *out)
{
	if (r == NULL || out == NULL || r->count == 0 || r->count > XRT_MAX_SEGMENTS || tile_count == 0 ||
	    tile_w == 0 || tile_h == 0) {
		return false;
	}
	const struct comp_seg_rect canvas = {
	    r->canvas.offset.w,
	    r->canvas.offset.h,
	    (uint32_t)(r->canvas.extent.w > 0 ? r->canvas.extent.w : 0),
	    (uint32_t)(r->canvas.extent.h > 0 ? r->canvas.extent.h : 0),
	};
	bool found = false;
	struct comp_segments_route_slot hit;
	memset(&hit, 0, sizeof(hit));
	// Validate EVERY segment, not only the one holding the view: a frame is
	// routed all-or-nothing, exactly as the in-process renderer does it.
	for (uint32_t k = 0; k < r->count; k++) {
		const uint32_t n = r->view_count[k] < tile_count ? r->view_count[k] : tile_count;
		if (n == 0 || r->first_view[k] + n > layer_view_count) {
			return false;
		}
		const struct comp_seg_rect seg = {
		    r->rect[k].offset.w,
		    r->rect[k].offset.h,
		    (uint32_t)(r->rect[k].extent.w > 0 ? r->rect[k].extent.w : 0),
		    (uint32_t)(r->rect[k].extent.h > 0 ? r->rect[k].extent.h : 0),
		};
		struct comp_seg_rect tr;
		if (!comp_segments_tile_rect(&seg, &canvas, tile_w, tile_h, &tr)) {
			return false;
		}
		if (!found && view >= r->first_view[k] && view < r->first_view[k] + n) {
			hit.segment = k;
			hit.tile = view - r->first_view[k];
			hit.rect = tr;
			found = true;
		}
	}
	if (found) {
		*out = hit;
	}
	return found;
}

/*!
 * Is any view of @p r routed at all (and so must the unrouted placement of
 * views past the routed ranges be suppressed)? True exactly when
 * @ref comp_segments_route_place would accept the frame.
 */
static inline bool
comp_segments_route_active(const struct xrt_segment_view_routing *r,
                           uint32_t layer_view_count,
                           uint32_t tile_count,
                           uint32_t tile_w,
                           uint32_t tile_h)
{
	if (r == NULL || r->count == 0) {
		return false;
	}
	struct comp_segments_route_slot s;
	return comp_segments_route_place(r, layer_view_count, tile_count, tile_w, tile_h, r->first_view[0], &s);
}

/*!
 * Publish @p next as the segment table readers frame views from, bumping the
 * generation only when the table (generation aside) changed — so a reader can
 * tell "the same table again" from "a new one" without comparing payloads.
 *
 * @return true when the published table changed.
 */
static inline bool
comp_segments_publish(struct xrt_segment_metrics *pub, const struct xrt_segment_metrics *next)
{
	struct xrt_segment_metrics m = *next;
	m.generation = pub->generation;
	const bool changed = memcmp(pub, &m, sizeof(m)) != 0;
	if (changed) {
		m.generation++;
	}
	*pub = m;
	return changed;
}

#ifdef __cplusplus
}
#endif
