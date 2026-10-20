// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Screen-capture pixel helpers shared by the common layer and the
 *         backends (XR_DXR_desktop_control spec §10, ADR-056 Amendment 1).
 *
 * Pure C, no OS calls, compiled on every OS (and unit tested there): rectangle
 * arithmetic, the desktop → image mapping, masking withheld areas, composing a
 * monitor's frame into a desktop image, and the area-filter downscale.
 *
 * Images are struct u_desktop_image (u_desktop.h): RGBA8, straight alpha, top
 * row first, stride w * 4, withheld pixels (0,0,0,0). Nothing here ever writes
 * a pixel anywhere but the caller's buffers.
 */

#pragma once

#include "u_desktop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Largest maxWidth / maxHeight (spec §10.3).
#define U_DESKTOP_CAPTURE_MAX_DIM 8192u
//! Largest native image a backend may produce (either side, device px).
#define U_DESKTOP_CAPTURE_MAX_NATIVE 32768u

//! Intersection of @p a and @p b; false (and *out zeroed) when empty.
bool
u_desktop_rect_intersect(const struct u_desktop_rect *a, const struct u_desktop_rect *b, struct u_desktop_rect *out);

//! Bounding box of @p n rects (zeroed when n == 0).
struct u_desktop_rect
u_desktop_rect_bounds(const struct u_desktop_rect *r, uint32_t n);

/*!
 * The output size for a @p src_w x @p src_h image that must fit @p max_w x
 * @p max_h: scaled DOWN keeping the aspect ratio (each side at least 1), never up.
 */
void
u_desktop_capture_fit(uint32_t src_w, uint32_t src_h, uint32_t max_w, uint32_t max_h, uint32_t *out_w, uint32_t *out_h);

/*!
 * Withhold (0,0,0,0) the part of @p img that shows desktop rect @p r, where @p img
 * covers desktop rect @p src. Every image pixel whose area overlaps @p r at all is
 * withheld (rounds outwards: never leaves a sliver of an excluded window).
 */
void
u_desktop_image_withhold(struct u_desktop_image *img, const struct u_desktop_rect *src, const struct u_desktop_rect *r);

//! Pixel layouts a capture source hands over.
enum u_desktop_pixfmt
{
	U_DESKTOP_PIXFMT_BGRX = 0, //!< B,G,R,x bytes (x ignored)
	U_DESKTOP_PIXFMT_BGRA,     //!< B,G,R,A bytes (A ignored: screen pixels are opaque)
	U_DESKTOP_PIXFMT_RGBX,
	U_DESKTOP_PIXFMT_RGBA,
};

/*!
 * Draw one monitor's frame into a desktop image. @p img covers desktop rect
 * @p src; the frame (@p fw x @p fh px, @p stride bytes per row, layout @p fmt)
 * shows desktop rect @p mon. Each image pixel inside @p mon takes the frame pixel
 * under its centre (nearest), with alpha 255; others are untouched. Exact 1:1
 * copy when both have the same density.
 */
void
u_desktop_image_draw_frame(struct u_desktop_image *img,
                           const struct u_desktop_rect *src,
                           const struct u_desktop_rect *mon,
                           const uint8_t *frame,
                           uint32_t fw,
                           uint32_t fh,
                           uint32_t stride,
                           enum u_desktop_pixfmt fmt);

/*!
 * Area-filter downscale of @p in into a new @p out_w x @p out_h image (alpha-
 * weighted, so withheld pixels never darken real ones; a pixel whose whole
 * footprint was withheld stays (0,0,0,0)). @p out_w / @p out_h must not exceed
 * the input size. When they are equal it is a copy. @p out is allocated.
 */
bool
u_desktop_image_downscale(const struct u_desktop_image *in, uint32_t out_w, uint32_t out_h, struct u_desktop_image *out);

#ifdef __cplusplus
}
#endif
