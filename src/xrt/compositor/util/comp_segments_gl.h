// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL coordinate helpers for window segments (multi-screen M6, GL).
 * @ingroup comp_util
 *
 * Everything in @ref comp_segments.h is top-left origin: window px, screen px,
 * and the rect a segment occupies inside a view tile. A GL framebuffer — the
 * window's default framebuffer, the atlas, a crop texture — is BOTTOM-left
 * origin, and the GL atlas additionally flips its tile ROWS (view 0 is the
 * top-left tile as displayed, which GL stores in the top row of `v`; see
 * u_tiling_view_origin_gl, #1625). These three pure functions are the only
 * places the GL segment path converts, so the crop, the per-segment weave
 * viewport, the flat-2D fill and the per-segment view routing all agree, and
 * the conversions are host-tested (tests/tests_comp_segments.cpp).
 */
#pragma once

#include "util/comp_segments.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * A top-left-origin rect in a framebuffer @p height px tall, in GL's
 * bottom-left framebuffer coordinates (what `glViewport` / `glScissor` /
 * `glBlitFramebuffer` take).
 */
static inline struct comp_seg_rect
comp_segments_gl_flip(const struct comp_seg_rect *r, uint32_t height)
{
	struct comp_seg_rect out;
	out.x = r->x;
	out.y = (int32_t)height - r->y - (int32_t)r->h;
	out.w = r->w;
	out.h = r->h;
	return out;
}

/*!
 * Where @p in_tile (a rect inside a view tile, top-left origin — what
 * @ref comp_segments_tile_rect returns) lands in a GL atlas, for the tile of
 * view grid cell (@p col, @p row) — row counted DOWNWARD from the top as
 * displayed — in a grid of @p rows rows of @p tile_w x @p tile_h tiles stored
 * from the atlas origin. Bottom-left coordinates.
 *
 * The crop reads a segment's views from here, the per-segment view routing
 * (M3) paints them here, and the flat-2D fill samples here: one mapping.
 */
static inline struct comp_seg_rect
comp_segments_gl_atlas_rect(
    const struct comp_seg_rect *in_tile, uint32_t col, uint32_t row, uint32_t rows, uint32_t tile_w, uint32_t tile_h)
{
	struct comp_seg_rect out;
	out.x = (int32_t)(col * tile_w) + in_tile->x;
	out.y = (int32_t)((rows - 1u - row) * tile_h) + ((int32_t)tile_h - in_tile->y - (int32_t)in_tile->h);
	out.w = in_tile->w;
	out.h = in_tile->h;
	return out;
}

/*!
 * Where tile (@p col, @p row) of a segment's CROP texture sits: the crop holds
 * `cols x rows` tiles of the segment's tile rect (@p seg_w x @p seg_h), in the
 * same GL row order as the atlas, so a DP reads the crop exactly like a
 * whole-canvas atlas. Bottom-left coordinates.
 */
static inline struct comp_seg_rect
comp_segments_gl_crop_rect(uint32_t col, uint32_t row, uint32_t rows, uint32_t seg_w, uint32_t seg_h)
{
	struct comp_seg_rect out;
	out.x = (int32_t)(col * seg_w);
	out.y = (int32_t)((rows - 1u - row) * seg_h);
	out.w = seg_w;
	out.h = seg_h;
	return out;
}

#ifdef __cplusplus
}
#endif
