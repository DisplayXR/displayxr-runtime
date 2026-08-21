// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Transport policy for the #918 bridge planes — pure, and therefore
 *         testable.
 * @ingroup comp_d3d11
 *
 * The plane transport's correctness is not in the D3D12 calls; it is in four
 * decisions taken around them, and every one of the #918 review's plane findings
 * was one of those decisions being made inline, differently, at more than one
 * site:
 *
 *   - **F3** stamped a slot VALID because it held some pixels, when what the
 *     consume half needs to know is whether it holds THESE pixels.
 *   - **F4** accumulated a dirty box that only ever covered where content IS,
 *     never where it WAS, so a shrink left ghosts nothing would refresh.
 *   - **F5/F6** clamped a copy box to the panel at one site and to the source at
 *     another, when the answer is min(source, allocation) at both — and an
 *     over-range box is silently DROPPED by the copy queue rather than failing.
 *
 * So the decisions live here, named, free of every graphics type, and covered by
 * `tests/tests_comp_d3d11_plane_policy.cpp`. The bridge calls them; it does not
 * restate them.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A rectangle in plane-source pixels. `w == 0 || h == 0` means "empty".
struct xb_plane_box
{
	int32_t x, y;
	uint32_t w, h;
};

//! What a frame's submit should do with one plane.
enum xb_plane_action
{
	//! Record both copy legs for the box @ref xb_plane_decide resolved.
	XB_PLANE_COPY,
	//! The slot already holds exactly this content generation and owes nothing.
	//! Not an optimisation — a full-window RGBA plane at 4K60 is 1.99 GB/s.
	XB_PLANE_SKIP_UNCHANGED,
	//! The bandwidth gate has this plane on half rate and this is the off seq.
	XB_PLANE_SKIP_HALF_RATE,
	//! Nothing transportable: the box came out empty against the extents.
	XB_PLANE_SKIP_EMPTY,
};

//! True when @p b covers no pixels.
static inline bool
xb_plane_box_empty(const struct xb_plane_box *b)
{
	return b == 0 || b->w == 0 || b->h == 0;
}

/*!
 * Fold @p add into @p acc (bounding-box union). An empty @p add changes nothing;
 * an empty @p acc becomes @p add.
 */
static inline void
xb_plane_box_union(struct xb_plane_box *acc, const struct xb_plane_box *add)
{
	if (xb_plane_box_empty(add)) {
		return;
	}
	if (xb_plane_box_empty(acc)) {
		*acc = *add;
		return;
	}
	const int32_t x0 = acc->x < add->x ? acc->x : add->x;
	const int32_t y0 = acc->y < add->y ? acc->y : add->y;
	const int32_t ax1 = acc->x + (int32_t)acc->w;
	const int32_t ay1 = acc->y + (int32_t)acc->h;
	const int32_t bx1 = add->x + (int32_t)add->w;
	const int32_t by1 = add->y + (int32_t)add->h;
	const int32_t x1 = ax1 > bx1 ? ax1 : bx1;
	const int32_t y1 = ay1 > by1 ? ay1 : by1;
	acc->x = x0;
	acc->y = y0;
	acc->w = (uint32_t)(x1 - x0);
	acc->h = (uint32_t)(y1 - y0);
}

//! Clip @p b into [0, @p lim_w) x [0, @p lim_h). Returns an empty box when the
//! intersection is empty.
static inline struct xb_plane_box
xb_plane_box_clamp(const struct xb_plane_box *b, uint32_t lim_w, uint32_t lim_h)
{
	struct xb_plane_box out = {0, 0, 0, 0};
	if (xb_plane_box_empty(b)) {
		return out;
	}
	int32_t x0 = b->x < 0 ? 0 : b->x;
	int32_t y0 = b->y < 0 ? 0 : b->y;
	int32_t x1 = b->x + (int32_t)b->w;
	int32_t y1 = b->y + (int32_t)b->h;
	if (x1 > (int32_t)lim_w) {
		x1 = (int32_t)lim_w;
	}
	if (y1 > (int32_t)lim_h) {
		y1 = (int32_t)lim_h;
	}
	if (x1 <= x0 || y1 <= y0) {
		return out;
	}
	out.x = x0;
	out.y = y0;
	out.w = (uint32_t)(x1 - x0);
	out.h = (uint32_t)(y1 - y0);
	return out;
}

/*!
 * Grow @p b out to a 4-pixel grid, then clamp into [0, @p lim_w) x [0, @p lim_h).
 *
 * The snap is deliberate: `CopyTextureRegion` on a COPY queue is only documented
 * as offset-free for uncompressed formats, and a mis-copied plane shows up as a
 * subtly torn 2D band rather than a hard failure — the kind of defect that costs
 * a day. Four pixels is free (the box is already hundreds wide) and covers every
 * block size a copy queue could impose.
 *
 * The clamp AFTER it is what #918 review F5/F6 is about: the snap can push the
 * box past a source that is not a multiple of 4 (an authored mask is created at
 * the app's own dims), and an over-range box is not an error the caller sees —
 * the copy is silently DROPPED, leaving whatever the destination held.
 */
static inline struct xb_plane_box
xb_plane_box_snap_clamp(const struct xb_plane_box *b, uint32_t lim_w, uint32_t lim_h)
{
	struct xb_plane_box snapped = {0, 0, 0, 0};
	if (xb_plane_box_empty(b)) {
		return snapped;
	}
	const int32_t x0 = b->x & ~3;
	const int32_t y0 = b->y & ~3;
	const int32_t x1 = (b->x + (int32_t)b->w + 3) & ~3;
	const int32_t y1 = (b->y + (int32_t)b->h + 3) & ~3;
	snapped.x = x0;
	snapped.y = y0;
	snapped.w = (uint32_t)(x1 - x0);
	snapped.h = (uint32_t)(y1 - y0);
	return xb_plane_box_clamp(&snapped, lim_w, lim_h);
}

/*!
 * The transportable extent of a plane: min(source, allocation) per axis, with a
 * source extent of 0 meaning "not known yet, use the allocation".
 *
 * One expression, called from both the stage clamp and the record clamp. They
 * used to disagree — one clamped to the panel, one to min(source, panel) — which
 * is how #918 review F5's uninitialised band survived a fix aimed at F6.
 */
static inline void
xb_plane_limits(uint32_t src_w, uint32_t src_h, uint32_t alloc_w, uint32_t alloc_h, uint32_t *out_w, uint32_t *out_h)
{
	*out_w = (src_w != 0 && src_w < alloc_w) ? src_w : alloc_w;
	*out_h = (src_h != 0 && src_h < alloc_h) ? src_h : alloc_h;
}

/*!
 * What this frame's submit should do with a plane whose write slot holds
 * generation @p slot_seq and still OWES @p owed, given that the frame staged
 * generation @p stage_seq over @p staged.
 *
 * @p owed is what the slot accumulated across the frames it was not the write
 * slot; @p staged is this frame's own dirty box. They are separate parameters
 * because the ORDER is load-bearing — the change-skip asks whether the slot owes
 * anything BEFORE this frame's box is folded in. Fold first and every slot that
 * already holds the current generation looks dirty, which turns the skip into a
 * copy on every frame and inverts the entire bandwidth argument.
 *
 * @param half_rate,parity The bandwidth gate's latch: a latched plane transports
 *        on every other seq.
 */
static inline enum xb_plane_action
xb_plane_decide(uint64_t slot_seq,
                uint64_t stage_seq,
                const struct xb_plane_box *owed,
                const struct xb_plane_box *staged,
                bool half_rate,
                uint64_t seq,
                uint64_t parity)
{
	/*
	 * The slot already carries exactly this generation AND owes no accumulated
	 * box, so there is nothing to transport. The consume half keeps sampling the
	 * pixels already there — which is why the recipe carries plane_seq: a
	 * change-skipped plane is provably the SAME content, never merely an old one.
	 */
	if (slot_seq == stage_seq && xb_plane_box_empty(owed)) {
		return XB_PLANE_SKIP_UNCHANGED;
	}
	if (half_rate && (seq & 1ull) != parity) {
		return XB_PLANE_SKIP_HALF_RATE;
	}
	if (xb_plane_box_empty(owed) && xb_plane_box_empty(staged)) {
		return XB_PLANE_SKIP_EMPTY;
	}
	return XB_PLANE_COPY;
}

/*!
 * #918 review F3 — whether a slot may be stamped VALID in the composite recipe.
 *
 * The test is "these pixels ARE the generation the frame staged", not "this slot
 * has some pixels". A change-skip satisfies it by construction, which is exactly
 * what a change-skip proves. A half-rate or empty-box skip does not: those leave
 * the slot on an older generation, and stamping them valid is what let a stale 2D
 * band composite under a current frame with nothing anywhere able to tell.
 */
static inline bool
xb_plane_slot_is_valid(bool live, bool staged, uint64_t slot_seq, uint64_t stage_seq)
{
	return live && staged && slot_seq != 0 && slot_seq == stage_seq;
}

#ifdef __cplusplus
}
#endif
