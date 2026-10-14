// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Put a window back where it was on a screen that was unplugged and
 *         plugged in again (pure geometry).
 * @ingroup aux_util
 *
 * When a screen disappears the OS moves its windows onto a remaining screen,
 * and when the same screen comes back it does not move them back. On macOS
 * AppKit parks an evicted window against the edge of the surviving screen
 * (logged on the Odyssey, arranged right of the laptop: the window's content
 * went to x = 1472 pt, 40 pt onto the 1512 pt-wide laptop, the rest hanging
 * where the panel had been), and the replugged panel finds it there: at the
 * panel's LEFT EDGE, whatever its position on the panel was before. The
 * runtime that owns placement (ADR-050) records
 * where the window sat relative to its screen, keyed by the screen's stable
 * identity, and restores that offset on the same screen's return.
 *
 * Everything here is top-down points (y grows downward): the caller converts
 * from its window system's space. Header-only so the rule is unit-tested on
 * the host (tests_aux_window_replug).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//! A rectangle in top-down points (x, y = top-left).
struct u_window_replug_rect
{
	double x, y, w, h;
};

/*!
 * Where @p frame sits relative to @p screen: its top-left minus the screen's
 * top-left. Stored at the last position the window had on its screen.
 */
static inline void
u_window_replug_offset(const struct u_window_replug_rect *frame,
                       const struct u_window_replug_rect *screen,
                       double *out_dx,
                       double *out_dy)
{
	*out_dx = frame->x - screen->x;
	*out_dy = frame->y - screen->y;
}

/*!
 * The frame to restore on the returning screen: the stored offset applied to
 * that screen's (possibly moved) top-left, keeping the window's size, then
 * clamped only as far as needed for the window to stay reachable:
 *
 *  - its top (the title bar) is not above @p visible's top (the menu bar) and
 *    not below @p visible's bottom minus @p min_visible;
 *  - at least @p min_visible points of it stay inside @p visible horizontally.
 *
 * A window that partly hung off the panel before the unplug is restored the
 * same way (the clamp keeps only the reachable minimum, not the whole window).
 * If the screen came back at another size the offset is kept and the clamp
 * does the rest.
 */
static inline struct u_window_replug_rect
u_window_replug_restore(double dx,
                        double dy,
                        double w,
                        double h,
                        const struct u_window_replug_rect *screen,
                        const struct u_window_replug_rect *visible,
                        double min_visible)
{
	struct u_window_replug_rect r = {screen->x + dx, screen->y + dy, w, h};
	const double min_x = visible->x + min_visible - w;
	const double max_x = visible->x + visible->w - min_visible;
	if (r.x < min_x) {
		r.x = min_x;
	}
	if (r.x > max_x) {
		r.x = max_x;
	}
	const double max_y = visible->y + visible->h - min_visible;
	if (r.y > max_y) {
		r.y = max_y;
	}
	if (r.y < visible->y) {
		r.y = visible->y;
	}
	return r;
}

#ifdef __cplusplus
}
#endif
