// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  THE logical-pixel -> device-pixel conversion at the Wayland boundary
 *         (#1595 / #1596).
 * @ingroup aux_util
 *
 * ## The rule
 *
 * **Wayland reports geometry in LOGICAL coordinates. Everything the weaver
 * consumes is DEVICE pixels. The conversion is ours to perform, and it happens
 * exactly once, where the geometry crosses into the runtime.**
 *
 * That is not a style preference. A lenticular weave is a ~1-pixel-period
 * interlace: the phase is a function of the surface's position in *physical
 * panel pixels*, and a scale error does not cancel anywhere downstream. Unlike
 * a translation error — which cancels the moment two quantities in the same
 * wrong frame are subtracted — a halved displacement lands on a plausible-looking
 * but wrong lattice. It fails silently rather than visibly, which is why the
 * measured session on 2026-09-20 produced three separate faults from one cause:
 * `wl_output.geometry` put the 3D panel at logical x=1728 while the runtime's panel
 * rect had it at device x=3456, so the output match could never succeed, the
 * surface fullscreened on the laptop, and the weave ran anyway.
 *
 * The vendor SDK ships the same guarantee from its side — every geometry it
 * reports or accepts is device pixels, unconditionally — so there is exactly one
 * place in the stack where the two spaces meet, and it is here.
 *
 * ## Why `wl_output.scale` is not the conversion factor
 *
 * `wl_output.scale` is an **integer** by protocol. On the measured box the
 * laptop runs at a fractional 1.6667 and still advertises `scale = 2`. Using it
 * would convert a 1728-logical origin to 3456 device px on an output whose real
 * device origin is 2880 — a 20% error that looks entirely plausible. The two
 * honest sources are:
 *
 *   - `wl_output.mode` (device px) divided by `xdg_output.logical_size`, or
 *   - a compositor that publishes the fractional scale directly
 *     (`wp_fractional_scale_v1`; GNOME's `Meta.Display.get_monitor_scale()`,
 *     which the `window-geometry@displayxr.org` extension already forwards).
 *
 * @ref u_wl_monitor accepts either and @ref u_wl_monitor_scale prefers the
 * explicit one.
 *
 * ## Naming
 *
 * Every field in this header carries its space in its name (`logical_*` /
 * `*_px`). The failures this header exists to prevent all came from two
 * coordinate spaces sharing one variable name; callers are expected to keep
 * that discipline on their own locals too.
 *
 * Pure arithmetic, no Wayland dependency, no platform guard — the rule is worth
 * strictly more when a host test can pin it than when it compiles only on the
 * one platform it runs on. See `tests/tests_aux_wayland_geom.cpp`.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * A rectangle in DEVICE pixels. Never logical — the whole point of this header
 * is that the two never share a type.
 *
 * @ingroup aux_util
 */
struct u_wl_rect_px
{
	int32_t x, y;
	int32_t w, h;
};

/*!
 * A rectangle in LOGICAL pixels (a compositor's global stage coordinates, as
 * the `window-geometry@displayxr.org` payload carries them). Its own type so
 * it can never be passed where @ref u_wl_rect_px is expected.
 *
 * @ingroup aux_util
 */
struct u_wl_rect_logical
{
	int32_t logical_x, logical_y;
	int32_t logical_w, logical_h;
};

/*!
 * One Wayland output / Mutter monitor, exactly as the two publishers describe
 * it — **all fields logical except `mode_*`**.
 *
 * Two publishers, one struct:
 *
 *   - the app's `wl_output` + `zxdg_output_v1` pair: `logical_*` from
 *     `xdg_output.logical_position` / `logical_size`, `mode_*` from
 *     `wl_output.mode`, `scale` left 0 (derived);
 *   - the `window-geometry@displayxr.org` GNOME Shell extension's
 *     `"monitor": {x, y, w, h, scale}`: `logical_*` from the rect, `scale`
 *     explicit and fractional, `mode_*` left 0 (derived).
 *
 * Whichever is populated, @ref u_wl_monitor_scale resolves one factor and every
 * other function in this header goes through it.
 *
 * @ingroup aux_util
 */
struct u_wl_monitor
{
	//! Position of this monitor's top-left in the compositor's global LOGICAL
	//! layout.
	int32_t logical_x, logical_y;
	//! Size of this monitor in LOGICAL pixels.
	int32_t logical_w, logical_h;

	/*!
	 * Fractional scale, when the publisher states it (GNOME's
	 * `get_monitor_scale`, `wp_fractional_scale_v1`). 0 means "not stated —
	 * derive it from @ref mode_w / @ref logical_w".
	 *
	 * NEVER `wl_output.scale`: that is an integer and is wrong on any
	 * fractionally-scaled output. See the file comment.
	 */
	double scale;

	//! Current mode in DEVICE pixels (`wl_output.mode`). 0 means "not stated
	//! — derive the device size from @ref scale".
	int32_t mode_w, mode_h;
};

/*!
 * Round a logical length to device pixels. Half-away-from-zero, so a 1080
 * logical height at 1.6667 lands on 1800 and not 1799.
 *
 * @ingroup aux_util
 */
static inline int32_t
u_wl_logical_to_px(int32_t logical, double scale)
{
	const double v = (double)logical * scale;
	return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

/*!
 * The one scale factor for this monitor, or 0.0 when it cannot be resolved.
 *
 * Prefers an explicitly published fractional scale; falls back to
 * `mode_w / logical_w`. Never uses an integer `wl_output.scale` — a caller that
 * has only that has no usable factor and should say so rather than guess.
 *
 * @ingroup aux_util
 */
static inline double
u_wl_monitor_scale(const struct u_wl_monitor *m)
{
	if (m == NULL) {
		return 0.0;
	}
	if (m->scale > 0.0) {
		return m->scale;
	}
	if (m->mode_w > 0 && m->logical_w > 0) {
		return (double)m->mode_w / (double)m->logical_w;
	}
	return 0.0;
}

/*!
 * This monitor's size in DEVICE pixels.
 *
 * The mode size when the publisher gave one (authoritative — it IS the device
 * extent), else `logical_size * scale`.
 *
 * @return false when neither is resolvable; the outputs are then untouched.
 *
 * @ingroup aux_util
 */
static inline bool
u_wl_monitor_size_px(const struct u_wl_monitor *m, int32_t *out_w_px, int32_t *out_h_px)
{
	if (m == NULL) {
		return false;
	}
	if (m->mode_w > 0 && m->mode_h > 0) {
		if (out_w_px != NULL) {
			*out_w_px = m->mode_w;
		}
		if (out_h_px != NULL) {
			*out_h_px = m->mode_h;
		}
		return true;
	}
	const double s = u_wl_monitor_scale(m);
	if (s <= 0.0 || m->logical_w <= 0 || m->logical_h <= 0) {
		return false;
	}
	if (out_w_px != NULL) {
		*out_w_px = u_wl_logical_to_px(m->logical_w, s);
	}
	if (out_h_px != NULL) {
		*out_h_px = u_wl_logical_to_px(m->logical_h, s);
	}
	return true;
}

/*!
 * This monitor's rect in DEVICE pixels — origin AND size.
 *
 * The origin is `logical_origin * this monitor's own scale`. On a mixed-scale
 * layout that is *a* device space rather than *the* device space (there is no
 * single global one: two outputs at different scales cannot both tile a common
 * device grid), but it is the same space the runtime's own panel rect is
 * expressed in, which is the only space the comparison has to agree with. Use
 * @ref u_wl_monitor_is_panel rather than comparing origins by hand — it knows
 * that the SIZE is the reliable half of the match and the origin the
 * corroborating half.
 *
 * @ingroup aux_util
 */
static inline bool
u_wl_monitor_rect_px(const struct u_wl_monitor *m, struct u_wl_rect_px *out_px)
{
	if (m == NULL || out_px == NULL) {
		return false;
	}
	const double s = u_wl_monitor_scale(m);
	if (s <= 0.0) {
		return false;
	}
	int32_t w_px = 0, h_px = 0;
	if (!u_wl_monitor_size_px(m, &w_px, &h_px)) {
		return false;
	}
	out_px->x = u_wl_logical_to_px(m->logical_x, s);
	out_px->y = u_wl_logical_to_px(m->logical_y, s);
	out_px->w = w_px;
	out_px->h = h_px;
	return true;
}

/*!
 * A window's logical rect on this monitor, converted to DEVICE pixels and
 * expressed **relative to the monitor's top-left**.
 *
 * Monitor-relative on purpose. The absolute device origin of a monitor in a
 * mixed-scale layout is a fiction (see @ref u_wl_monitor_rect_px), but
 * `window - monitor` is not: it is a displacement within one output, at one
 * scale, and it is exactly the quantity the weave phase needs
 * (`srWeaverSetPresentOrigin` takes the surface's offset from the panel's
 * top-left). A caller that needs an absolute rect adds the runtime's own
 * resolved panel origin back on, which keeps the subtraction downstream exact
 * no matter which space that origin lives in.
 *
 * @param m                 the monitor the window is on
 * @param win_logical_x/y   window origin in the global LOGICAL layout
 * @param win_logical_w/h   window size in LOGICAL pixels
 *
 * @ingroup aux_util
 */
static inline bool
u_wl_window_rect_px_on_monitor(const struct u_wl_monitor *m,
                               int32_t win_logical_x,
                               int32_t win_logical_y,
                               int32_t win_logical_w,
                               int32_t win_logical_h,
                               struct u_wl_rect_px *out_px)
{
	if (m == NULL || out_px == NULL || win_logical_w <= 0 || win_logical_h <= 0) {
		return false;
	}
	const double s = u_wl_monitor_scale(m);
	if (s <= 0.0) {
		return false;
	}
	out_px->x = u_wl_logical_to_px(win_logical_x - m->logical_x, s);
	out_px->y = u_wl_logical_to_px(win_logical_y - m->logical_y, s);
	out_px->w = u_wl_logical_to_px(win_logical_w, s);
	out_px->h = u_wl_logical_to_px(win_logical_h, s);
	return true;
}

/*!
 * Is this output the 3D panel?
 *
 * The comparison the app's fullscreen-on-the-right-monitor match needs, and the
 * one that failed on 2026-09-20 because it was made in two different spaces.
 *
 * SIZE is the reliable half: an output's mode is device pixels by protocol and
 * the panel's `displayPixelWidth/Height` is device pixels by contract, so they
 * are directly comparable with no conversion at all. ORIGIN is the corroborating
 * half and needs the conversion, which is why it is reported separately rather
 * than folded into the verdict — a caller with exactly one size candidate should
 * take it and note the origin disagreement, while a caller with several needs
 * the origin to break the tie. That mirrors the runtime's own desktop resolver,
 * whose `OS_DISPLAY_DESKTOP_RULE_PIXEL_MATCH` exists for the same reason.
 *
 * @param panel_left/top    panel origin as the runtime reports it
 *                          (`XrDisplayDesktopPositionDXR`), DEVICE pixels
 * @param panel_w/h         panel native size (`XrDisplayInfoDXR`), DEVICE pixels
 * @param out_origin_agrees set when the converted device origin also matches
 *                          (may be NULL)
 * @return true when this output's DEVICE size is the panel's native size.
 *
 * @ingroup aux_util
 */
static inline bool
u_wl_monitor_is_panel(const struct u_wl_monitor *m,
                      int32_t panel_left,
                      int32_t panel_top,
                      uint32_t panel_w,
                      uint32_t panel_h,
                      bool *out_origin_agrees)
{
	if (out_origin_agrees != NULL) {
		*out_origin_agrees = false;
	}
	if (m == NULL || panel_w == 0 || panel_h == 0) {
		return false;
	}
	struct u_wl_rect_px r = {0, 0, 0, 0};
	if (!u_wl_monitor_rect_px(m, &r)) {
		return false;
	}
	if (r.w != (int32_t)panel_w || r.h != (int32_t)panel_h) {
		return false;
	}
	if (out_origin_agrees != NULL) {
		*out_origin_agrees = (r.x == panel_left && r.y == panel_top);
	}
	return true;
}

/*!
 * Can what we are about to present reach the glass **unresampled**?
 *
 * The refuse-rather-than-resample decision (#1595), reduced to the one
 * comparison that decides it: the buffer we hand the compositor against the
 * region the compositor will paint it into, both in DEVICE pixels.
 *
 * The destination is the caller's to name, because only the caller knows the
 * surface's role:
 *
 *   - fullscreen on the 3D panel  -> the panel's native size;
 *   - windowed                    -> the window's DEVICE extent
 *                                    (@ref u_wl_window_rect_px_on_monitor).
 *
 * Equality is exact and deliberately has no tolerance. A one-pixel difference
 * is still a resample, and a resample of a ~1-pixel-period interlace is a
 * uniform double image across the whole surface with no vantage point where it
 * resolves — strictly worse than honest flat 2D, which is at least correct
 * content in the wrong dimensionality and recovers the instant the session
 * becomes 1:1.
 *
 * Answers false on a zero extent: "we do not know yet" is not "it is fine", but
 * it is also not a reason to degrade — callers must check for the unknown
 * separately and keep the state they are in, never degrade on ignorance.
 *
 * @ingroup aux_util
 */
static inline bool
u_wl_present_is_1to1(uint32_t buffer_w_px, uint32_t buffer_h_px, uint32_t dest_w_px, uint32_t dest_h_px)
{
	if (buffer_w_px == 0 || buffer_h_px == 0 || dest_w_px == 0 || dest_h_px == 0) {
		return false;
	}
	return buffer_w_px == dest_w_px && buffer_h_px == dest_h_px;
}

/*!
 * Where a window's PIXELS are, given the two rects Mutter publishes for it
 * (#1654): the rect to use for the present origin, the Kooima canvas and the
 * 1:1 check is the one the client's surface occupies, not the one the user
 * sees as "the window".
 *
 *   - `frame`  is the window geometry (`Meta.Window.get_frame_rect`). With
 *              client-side decorations it includes the title bar, which the
 *              client draws in a subsurface ABOVE the bound surface.
 *   - `buffer` is the main surface (`Meta.Window.get_buffer_rect`) — the very
 *              surface bound through XR_DXR_wayland_surface_binding, whose
 *              buffer is the runtime's swapchain.
 *
 * Without decorations the two are equal, so preferring the buffer changes
 * nothing there; with a title bar the frame would put the weave phase, the
 * canvas and the 1:1 comparison a bar-height off.
 *
 * @param buffer  NULL, or a rect with a non-positive size, when the publisher
 *                did not report one — the frame is then the only answer.
 * @return true when the buffer rect was chosen.
 *
 * @ingroup aux_util
 */
static inline bool
u_wl_window_content_rect(const struct u_wl_rect_logical *frame,
                         const struct u_wl_rect_logical *buffer,
                         struct u_wl_rect_logical *out)
{
	if (buffer != NULL && buffer->logical_w > 0 && buffer->logical_h > 0) {
		*out = *buffer;
		return true;
	}
	*out = *frame;
	return false;
}

/*!
 * Whether the committed surface lies inside the window frame (±1 logical px
 * of rounding). True for an undecorated window (equal rects) and for one with
 * a client-side title bar (the surface is the frame minus the bar). False is
 * the signature of a buffer not mapped to its configured size — e.g. a
 * device-pixel buffer with no wp_viewport destination at 200 %, which makes the
 * surface twice the frame and spills it onto the next output.
 *
 * @ingroup aux_util
 */
static inline bool
u_wl_surface_within_frame(const struct u_wl_rect_logical *frame, const struct u_wl_rect_logical *buffer)
{
	const int32_t tol = 1;
	return buffer->logical_x >= frame->logical_x - tol && buffer->logical_y >= frame->logical_y - tol &&
	       buffer->logical_x + buffer->logical_w <= frame->logical_x + frame->logical_w + tol &&
	       buffer->logical_y + buffer->logical_h <= frame->logical_y + frame->logical_h + tol;
}

/*!
 * The placement quantum of a Wayland monitor, in DEVICE pixels — how far apart
 * the positions a window can actually be placed at are (#1609).
 *
 * A compositor positions windows in integer LOGICAL pixels, so on a monitor at
 * scale `s` only every `s`-th device pixel is reachable. That is a lattice
 * only when `s` is an integer: at 1.6667 the reachable device positions are
 * `round(k * 1.6667)` — 0, 2, 3, 5, 7, 8 … — which has no period, so no
 * phase-correct position can be named and a caller must not pretend otherwise.
 *
 * @param[out] out_q  the quantum, when true is returned (1 at scale 1.0).
 * @return false when @p scale is not a positive integer within @p tol.
 *
 * @ingroup aux_util
 */
static inline bool
u_wl_placement_quantum(double scale, double tol, uint32_t *out_q)
{
	if (out_q == NULL || !(scale >= 1.0)) {
		return false;
	}
	const double nearest = (double)(int32_t)(scale + 0.5);
	if (nearest < 1.0 || (scale > nearest ? scale - nearest : nearest - scale) > tol) {
		return false;
	}
	*out_q = (uint32_t)nearest;
	return true;
}

/*!
 * The part of a window that is NOT on the 3D panel, as up to four bands
 * (#1654).
 *
 * A weave is only correct where the panel's lens is. A window dragged half
 * onto an ordinary monitor is still ONE surface and one weave, so the half off
 * the panel would show the lenticular pattern as a double image. The runtime
 * paints those bands flat instead. (On X11 the weaver is window-bound and
 * clips itself; the Wayland weaver is windowless, so the runtime does it.)
 *
 * Every length is DEVICE pixels. @p ox / @p oy is the window's top-left
 * relative to the panel's, i.e. exactly the present origin the weaver is fed,
 * so the bands and the weave phase can never disagree.
 *
 * The bands tile the off-panel area without overlapping: the full-width strips
 * above and below the visible rows first, then the left and right strips
 * between them.
 *
 * @param[out] out  up to four rects in WINDOW-local pixels
 * @return how many were written; 0 when the window is entirely on the panel,
 *         and 1 (the whole window) when it is entirely off it.
 *
 * @ingroup aux_util
 */
static inline uint32_t
u_wl_offpanel_bands(int32_t ox,
                    int32_t oy,
                    uint32_t panel_w,
                    uint32_t panel_h,
                    uint32_t win_w,
                    uint32_t win_h,
                    struct u_wl_rect_px out[4])
{
	if (out == NULL || win_w == 0 || win_h == 0 || panel_w == 0 || panel_h == 0) {
		return 0;
	}
	// The visible (on-panel) rect, in window-local coordinates.
	int32_t vx0 = -ox, vy0 = -oy;
	int32_t vx1 = (int32_t)panel_w - ox, vy1 = (int32_t)panel_h - oy;
	if (vx0 < 0) {
		vx0 = 0;
	}
	if (vy0 < 0) {
		vy0 = 0;
	}
	if (vx1 > (int32_t)win_w) {
		vx1 = (int32_t)win_w;
	}
	if (vy1 > (int32_t)win_h) {
		vy1 = (int32_t)win_h;
	}
	if (vx0 >= vx1 || vy0 >= vy1) {
		out[0].x = 0;
		out[0].y = 0;
		out[0].w = (int32_t)win_w;
		out[0].h = (int32_t)win_h;
		return 1;
	}
	uint32_t n = 0;
	if (vy0 > 0) {
		out[n].x = 0;
		out[n].y = 0;
		out[n].w = (int32_t)win_w;
		out[n].h = vy0;
		n++;
	}
	if (vy1 < (int32_t)win_h) {
		out[n].x = 0;
		out[n].y = vy1;
		out[n].w = (int32_t)win_w;
		out[n].h = (int32_t)win_h - vy1;
		n++;
	}
	if (vx0 > 0) {
		out[n].x = 0;
		out[n].y = vy0;
		out[n].w = vx0;
		out[n].h = vy1 - vy0;
		n++;
	}
	if (vx1 < (int32_t)win_w) {
		out[n].x = vx1;
		out[n].y = vy0;
		out[n].w = (int32_t)win_w - vx1;
		out[n].h = vy1 - vy0;
		n++;
	}
	return n;
}

#ifdef __cplusplus
}
#endif
