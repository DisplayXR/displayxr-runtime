// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The present-owner's panel-relative DEVICE origin on the desktop-Linux
 *         service weave path — the value fed to the DP phase slot
 *         (`set_present_origin`) and the off-panel band origin.
 * @ingroup aux_util
 *
 * ## Why this exists (fractional scales)
 *
 * The service learns the panel's position from `XR_DXR_display_info`'s
 * resolver (`display_screen_left/top`), which on desktop Linux is in the X11
 * window-placement space. Under XWayland that space is NOT device pixels:
 * Mutter scales the X root by one global factor (the ceiling of the largest
 * monitor scale with xwayland-native-scaling, else 1 — logical px). A
 * 3840x2160 panel at 150 % right of a 1728-logical-wide laptop sits at X11
 * x = 3456, but its device-pixel top-left in the weave geometry convention is
 * 1728 x 1.5 = 2592.
 *
 * A present-owner (the DisplayXR Browser) publishes its window in the weave
 * geometry convention — device px, desktop-absolute, where "desktop-absolute"
 * on a mixed-scale Wayland desktop means the window's monitor's logical
 * origin times that monitor's scale plus the monitor-relative logical offset
 * times the same scale (u_wl_window_rect_px_on_monitor; the browser's
 * StageRectToDevicePx). Subtracting the X11 panel origin from that mixed the
 * two systems: correct only when the X root scale equals the panel scale (a
 * DS1 / Odyssey at 200 %), and a wrong lenticular phase — inverted eyes,
 * position-dependent — at 125 / 150 / 175 %.
 *
 * ## The rule
 *
 * 1. The panel's origin in the weave geometry frame is the X11 origin times
 *    the panel's device-per-X11 ratio (native mode / X11 rect, per axis). That
 *    ratio is exactly `panel scale / X-root scale`, so the product is
 *    `panel logical origin x panel scale` — what the browser computed for the
 *    same monitor. Ratio 1 (pure X11, PHYSICAL layout mode, X root scale ==
 *    panel scale) leaves the origin unchanged, so every box that worked keeps
 *    the identical value.
 * 2. When the present-owner also states its v12 logical (Mutter stage) origin
 *    and the stage->device factor it converted with, the origin is the
 *    panel-relative stage offset converted by the panel's own factor —
 *    exactly what the in-process vk_native compositor feeds (wl_geom's
 *    monitor-relative `left_px`). This also holds for a window whose own
 *    monitor is not the panel (straddling from a laptop at another scale),
 *    where a device-frame difference would mix two scales.
 * 3. Otherwise the device-frame difference `window - panel origin (rule 1)`.
 *
 * Pure arithmetic, no platform guard; pinned by tests_aux_weave_present_origin.
 */

#pragma once

#include "util/u_wayland_geom.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The panel as the service's system-compositor info describes it.
 */
struct u_weave_panel_frame
{
	//! The panel's top-left in the window-placement (X11) space —
	//! `xrt_system_compositor_info::display_screen_left/top`.
	int32_t screen_left, screen_top;
	//! The panel monitor's rect size in that same space (0 = unknown) —
	//! `display_desktop_width/height`.
	uint32_t desktop_w, desktop_h;
	//! The connector's device mode (0 = unknown) —
	//! `display_desktop_native_width/height`.
	uint32_t native_w, native_h;
	//! The compositor's scale for the panel output (0 = unknown) —
	//! `display_desktop_scale`.
	double scale;
};

//! Which rule produced the origin (for the log).
enum u_weave_origin_source
{
	//! Rule 3, ratio 1: the window minus the X11 panel origin (pre-fix value).
	U_WEAVE_ORIGIN_DEVICE_UNSCALED = 0,
	//! Rule 3 with the panel origin converted X11 -> device frame.
	U_WEAVE_ORIGIN_DEVICE_CONVERTED,
	//! Rule 2: from the v12 logical (stage) origin.
	U_WEAVE_ORIGIN_LOGICAL,
};

static inline const char *
u_weave_origin_source_str(enum u_weave_origin_source s)
{
	switch (s) {
	case U_WEAVE_ORIGIN_LOGICAL: return "the present-owner's logical origin";
	case U_WEAVE_ORIGIN_DEVICE_CONVERTED: return "its device origin, panel origin converted X11 -> device";
	default: return "its device origin";
	}
}

//! Round-half-away-from-zero of a double to int32 (the u_wl_logical_to_px rule).
static inline int32_t
u_weave_round(double v)
{
	return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

/*!
 * Rule 1: the panel's top-left in the weave geometry (device) frame.
 *
 * @return true when a conversion was applied (ratio != 1 on some axis).
 */
static inline bool
u_weave_panel_device_origin(const struct u_weave_panel_frame *p, int32_t *out_x, int32_t *out_y)
{
	bool converted = false;
	int32_t x = p->screen_left, y = p->screen_top;
	// Preferred: the X root's own factor. It is an integer by construction
	// (Mutter's global XWayland scale, or 1), whereas a panel's logical extent
	// at a fractional scale is rounded, so the raw native/desktop ratio can be
	// a hair off. The panel's logical origin is then exactly X11 / root.
	if (p->scale > 0.0 && p->native_w > 0 && p->desktop_w > 0) {
		const double root = (double)p->desktop_w * p->scale / (double)p->native_w;
		const double root_i = floor(root + 0.5);
		if (root_i >= 1.0 && fabs(root - root_i) < 0.02) {
			x = u_weave_round((double)p->screen_left / root_i * p->scale);
			y = u_weave_round((double)p->screen_top / root_i * p->scale);
			*out_x = x;
			*out_y = y;
			return x != p->screen_left || y != p->screen_top;
		}
	}
	// Otherwise the per-axis device-per-X11 ratio.
	if (p->native_w > 0 && p->desktop_w > 0 && p->native_w != p->desktop_w) {
		x = u_weave_round((double)p->screen_left * (double)p->native_w / (double)p->desktop_w);
		converted = converted || x != p->screen_left;
	}
	if (p->native_h > 0 && p->desktop_h > 0 && p->native_h != p->desktop_h) {
		y = u_weave_round((double)p->screen_top * (double)p->native_h / (double)p->desktop_h);
		converted = converted || y != p->screen_top;
	}
	*out_x = x;
	*out_y = y;
	return converted;
}

/*!
 * The panel's stage->device factor, given the factor the present-owner
 * converted its own window with (@p window_factor, the v12 `scale`).
 *
 * In Mutter's LOGICAL layout mode a monitor's factor is its scale; in PHYSICAL
 * (integer scales only) every factor is 1 and the X root is device px, so the
 * panel's X11 rect equals its native mode. A window factor of 1 on a panel
 * whose X11 rect is its native mode therefore reads as PHYSICAL; any other
 * factor is LOGICAL, where the panel's own scale applies — even when the
 * window's monitor is another one.
 */
static inline double
u_weave_panel_stage_factor(const struct u_weave_panel_frame *p, double window_factor)
{
	if (!(p->scale > 0.0)) {
		return window_factor;
	}
	const bool rect_is_native = p->native_w > 0 && p->native_w == p->desktop_w;
	if (fabs(window_factor - 1.0) < 1e-3 && rect_is_native) {
		return 1.0;
	}
	return p->scale;
}

/*!
 * The present-owner's panel-relative DEVICE origin (the DP phase slot value).
 *
 * @param p              the panel
 * @param win_x, win_y   the bound `windowOriginOnScreen` (weave geometry frame)
 * @param logical_valid  the bind carried an `XrWeaveWindowLogicalOriginDXR`
 * @param logical_x, logical_y, logical_scale  that chain, verbatim
 */
static inline enum u_weave_origin_source
u_weave_panel_relative_origin(const struct u_weave_panel_frame *p,
                              int32_t win_x,
                              int32_t win_y,
                              bool logical_valid,
                              int32_t logical_x,
                              int32_t logical_y,
                              float logical_scale,
                              int32_t *out_x,
                              int32_t *out_y)
{
	int32_t pdx = 0, pdy = 0;
	const bool converted = u_weave_panel_device_origin(p, &pdx, &pdy);

	if (logical_valid && logical_scale > 0.0f && logical_scale <= 64.0f) {
		const double f = u_weave_panel_stage_factor(p, (double)logical_scale);
		if (f > 0.0) {
			// The panel's top-left in stage px: exact for every Mutter layout
			// (the device origin is stage origin x factor by construction).
			const int32_t psx = u_weave_round((double)pdx / f);
			const int32_t psy = u_weave_round((double)pdy / f);
			*out_x = u_wl_logical_to_px(logical_x - psx, f);
			*out_y = u_wl_logical_to_px(logical_y - psy, f);
			return U_WEAVE_ORIGIN_LOGICAL;
		}
	}
	*out_x = win_x - pdx;
	*out_y = win_y - pdy;
	return converted ? U_WEAVE_ORIGIN_DEVICE_CONVERTED : U_WEAVE_ORIGIN_DEVICE_UNSCALED;
}

#ifdef __cplusplus
}
#endif
