// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Texture target binding (ADR-060, XR_DXR_win32_window_binding v9):
 *         a shared-texture session names WHERE its texture is drawn as a
 *         reference window plus a rect in that window's client area.
 *
 * The runtime is the placement authority (ADR-033). Every weave it resolves
 * the pair ONCE to the target's rect on the desktop, and derives the three
 * outputs from that one rect:
 *
 *  - phase:          set_present_origin(target - panel origin) on the primary DP;
 *  - window metrics: window_screen_left/top = target origin, window px = rect size;
 *  - segments:       seg_window = the target's desktop rect.
 *
 * This header is the math, platform-neutral and C-compatible so the unit tests
 * run everywhere. The Win32 read of the reference window lives in
 * comp_texture_target_win32.h. Both are DATA in, DATA out: the native D3D11
 * compositor calls them today and the ADR-059 hub calls them unchanged when it
 * takes texture mode (client-presents presenter strategy).
 *
 * @ingroup comp_util
 */

#pragma once

#include "util/comp_segments.h" // struct comp_seg_rect

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * What the app named: the binding (create time) updated by every frame that
 * carries a target. Rect in physical device px: client-area px of @ref ref,
 * or desktop px when @ref ref is NULL.
 */
struct comp_texture_target
{
	bool bound;    //!< the session was created with a target binding
	void *ref;     //!< reference window (HWND), NULL = the rect is in desktop px
	int32_t x;     //!< target origin in the reference's client area (or desktop)
	int32_t y;     //!<
	uint32_t w;    //!< target size, device px (> 0 once validated)
	uint32_t h;    //!<
	uint64_t flags; //!< XrWin32TextureTargetFrameFlagsDXR (advisory, forwarded only)
};

/*!
 * One resolve of the pair, for one weave (or one locate).
 */
struct comp_texture_target_resolved
{
	bool valid;
	//! The rect was larger than the shared texture and was clamped to it.
	bool clamped;
	//! The target on the desktop: origin in desktop device px, size = the canvas.
	struct comp_seg_rect screen;
};

/*!
 * Compose a resolve from the reference window's client origin on the desktop
 * (0,0 for a NULL reference) and clamp the size to the shared texture
 * (@p tex_w / @p tex_h, 0 = unknown, no clamp). The canvas in the shared
 * texture is always (0, 0, screen.w, screen.h).
 */
static inline bool
comp_texture_target_compose(const struct comp_texture_target *tt,
                            int32_t ref_client_left,
                            int32_t ref_client_top,
                            uint32_t tex_w,
                            uint32_t tex_h,
                            struct comp_texture_target_resolved *out)
{
	out->valid = false;
	out->clamped = false;
	out->screen.x = 0;
	out->screen.y = 0;
	out->screen.w = 0;
	out->screen.h = 0;
	if (tt == NULL || !tt->bound || tt->w == 0 || tt->h == 0) {
		return false;
	}
	uint32_t w = tt->w;
	uint32_t h = tt->h;
	if (tex_w > 0 && w > tex_w) {
		w = tex_w;
		out->clamped = true;
	}
	if (tex_h > 0 && h > tex_h) {
		h = tex_h;
		out->clamped = true;
	}
	out->screen.x = ref_client_left + tt->x;
	out->screen.y = ref_client_top + tt->y;
	out->screen.w = w;
	out->screen.h = h;
	out->valid = true;
	return true;
}

/*!
 * The present origin a windowless DP phases from: the target's origin
 * relative to the origin of the screen that DP weaves (its panel).
 */
static inline void
comp_texture_target_present_origin(const struct comp_texture_target_resolved *r,
                                   int32_t panel_left,
                                   int32_t panel_top,
                                   int32_t *out_x,
                                   int32_t *out_y)
{
	*out_x = r->screen.x - panel_left;
	*out_y = r->screen.y - panel_top;
}

/*!
 * Anchor fallback (no windowless DP): a windowed DP phases render-target
 * pixel p as the anchor window's client pixel p, but the app draws the
 * canvas, which is texel (0,0) of the shared texture, at the target's origin.
 * Returns that origin relative to the anchor's client origin: weaving the
 * canvas at this offset into a render target of at least (x + w) x (y + h)
 * phases it where the app draws it. False when there is nothing to correct
 * (offset 0,0) or it cannot be corrected: the target starts left of or above
 * the anchor's client area, or the render target would exceed @p max_dim.
 */
static inline bool
comp_texture_target_anchor_offset(const struct comp_texture_target_resolved *r,
                                  int32_t anchor_left,
                                  int32_t anchor_top,
                                  uint32_t max_dim,
                                  int32_t *out_x,
                                  int32_t *out_y)
{
	*out_x = 0;
	*out_y = 0;
	if (r == NULL || !r->valid) {
		return false;
	}
	const int64_t x = (int64_t)r->screen.x - anchor_left;
	const int64_t y = (int64_t)r->screen.y - anchor_top;
	if (x < 0 || y < 0 || (x == 0 && y == 0)) {
		return false;
	}
	if (x + r->screen.w > (int64_t)max_dim || y + r->screen.h > (int64_t)max_dim) {
		return false;
	}
	*out_x = (int32_t)x;
	*out_y = (int32_t)y;
	return true;
}

#ifdef __cplusplus
}
#endif
