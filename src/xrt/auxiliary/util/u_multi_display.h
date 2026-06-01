// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header-only multi-display slice geometry for cross-monitor weaving.
 * @author David Fattal
 * @ingroup aux_util
 *
 * A window's client area maps 1:1 to ONE multiview atlas content region. When
 * that window spans several physical monitors, each monitor's display processor
 * must weave only the sub-region of the atlas that lands on it, and needs the
 * slice's monitor-relative screen position as its lenticular-phase input.
 *
 * This file splits a window rect against a list of monitor rects into
 * per-monitor slices. Pure integer geometry, no OS headers — testable headless
 * on any platform (this is issue #69 Phase 3b's highest-risk math, proven here
 * before a second display is ever attached; no compositor wiring yet).
 *
 * DENSITY ASSUMPTION (Phase 3a): uniform effective pixel density across all
 * monitors. One window-client pixel == one screen pixel == one atlas pixel
 * everywhere, so this is a pure rect clip and `backbuffer_offset` equals the
 * atlas offset. Per-monitor DPI / fractional scaling is Phase 3b — at which
 * point `backbuffer_offset_*` diverges from the atlas offset (it is kept as a
 * separate field here precisely so that change needs no ABI break).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Max slices a single window split can produce. */
#define U_MD_MAX_SLICES 8

/*!
 * A half-open screen-space rectangle covering [left,right) x [top,bottom).
 * Signed because monitors legitimately sit at negative virtual-desktop
 * coordinates (a monitor placed left of / above the primary). Translate a
 * Win32 RECT directly into these fields at the call site.
 */
struct u_md_rect
{
	int32_t left;   //!< Inclusive left edge, screen-space pixels.
	int32_t top;    //!< Inclusive top edge, screen-space pixels.
	int32_t right;  //!< Exclusive right edge, screen-space pixels.
	int32_t bottom; //!< Exclusive bottom edge, screen-space pixels.
};

/*!
 * One per-monitor slice of the window/atlas.
 */
struct u_md_slice
{
	int32_t atlas_x;  //!< Slice origin X relative to window/atlas (>= 0).
	int32_t atlas_y;  //!< Slice origin Y relative to window/atlas (>= 0).
	uint32_t atlas_w; //!< Slice width in atlas pixels (> 0).
	uint32_t atlas_h; //!< Slice height in atlas pixels (> 0).

	int32_t canvas_offset_x; //!< Slice top-left X within this monitor's panel.
	int32_t canvas_offset_y; //!< Slice top-left Y within this monitor's panel.
	                         //!< Monitor-relative screen pos = DP phase input.

	int32_t backbuffer_offset_x; //!< Where the slice lands in the window backbuffer.
	int32_t backbuffer_offset_y; //!< 3a: == atlas_x/atlas_y (1:1). 3b: diverges.

	int32_t monitor_index; //!< Index into the caller's monitor_rects[].
};

/*! @private */
static inline int32_t
u_md_max32(int32_t a, int32_t b)
{
	return a > b ? a : b;
}

/*! @private */
static inline int32_t
u_md_min32(int32_t a, int32_t b)
{
	return a < b ? a : b;
}

/*!
 * Split a window rect into per-monitor atlas slices.
 *
 * For each monitor, intersect (window ∩ monitor) in half-open screen space; if
 * non-empty, emit one slice. Window pixels that fall on no monitor are silently
 * dropped (a partially-off-screen window weaves only its on-screen portion).
 *
 * Half-open edges mean a monitor seam is counted exactly once: a window flush
 * to the shared edge emits a slice on the left monitor only, never both.
 *
 * On `out_capacity` overflow, extra intersections are silently dropped — the
 * return count communicates how many were written.
 *
 * @param window         Window client area in screen-space pixels.
 * @param monitor_rects  Array of monitor rects in screen-space pixels.
 * @param monitor_count  Number of monitors (0 → no slices).
 * @param[out] out_slices  Caller buffer for emitted slices.
 * @param out_capacity   Size of out_slices (typically @ref U_MD_MAX_SLICES).
 * @return Number of slices written. 0 if the window is off all monitors, the
 *         monitor list is empty, or the window has zero area.
 */
static inline uint32_t
u_multi_display_compute_slices(struct u_md_rect window,
                               const struct u_md_rect *monitor_rects,
                               uint32_t monitor_count,
                               struct u_md_slice *out_slices,
                               uint32_t out_capacity)
{
	// Degenerate window → nothing to weave.
	if (window.right <= window.left || window.bottom <= window.top) {
		return 0;
	}
	if (monitor_rects == NULL || monitor_count == 0 || out_slices == NULL || out_capacity == 0) {
		return 0;
	}

	uint32_t n = 0;
	for (uint32_t i = 0; i < monitor_count; i++) {
		const struct u_md_rect m = monitor_rects[i];

		int32_t l = u_md_max32(window.left, m.left);
		int32_t t = u_md_max32(window.top, m.top);
		int32_t r = u_md_min32(window.right, m.right);
		int32_t b = u_md_min32(window.bottom, m.bottom);

		// Empty intersection (half-open) → skip; also kills seam double-count.
		if (r <= l || b <= t) {
			continue;
		}

		// Out of room → silently drop the rest (return count signals it).
		if (n >= out_capacity) {
			break;
		}

		struct u_md_slice *s = &out_slices[n++];
		s->atlas_x = l - window.left;
		s->atlas_y = t - window.top;
		s->atlas_w = (uint32_t)(r - l);
		s->atlas_h = (uint32_t)(b - t);
		s->canvas_offset_x = l - m.left;
		s->canvas_offset_y = t - m.top;
		s->backbuffer_offset_x = s->atlas_x; // 3a: 1:1 with atlas (uniform density)
		s->backbuffer_offset_y = s->atlas_y; // 3a: 1:1 with atlas (uniform density)
		s->monitor_index = (int32_t)i;
	}

	return n;
}

#ifdef __cplusplus
}
#endif
