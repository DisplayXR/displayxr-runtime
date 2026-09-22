// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Wayland window-geometry provider (compositor side channel), #817.
 *
 * Wayland never exposes a surface's absolute position to its client, but
 * windowed weaving anchors the interlacing phase to exactly that. This
 * provider consumes the geometry the compositor publishes over the session
 * D-Bus — GNOME Shell extension `window-geometry@displayxr.org` (see
 * contrib/gnome-shell/), service `org.displayxr.WindowGeometry` — and hands
 * the runtime a window rect in the same units X11 gets from
 * xcb_translate_coordinates. The rest of the chain (get_window_metrics →
 * vk_update_present_origin → DP set_present_origin) is source-agnostic and
 * unchanged.
 *
 * ## This file is the logical → device boundary (#1596)
 *
 * The wire payload is LOGICAL (Mutter stage) pixels. Everything past this
 * file's public API is DEVICE pixels, because that is the only space the
 * weaver has — the vendor SDK guarantees the same thing from its side, so the
 * conversion is ours to perform and it happens here, once. Prior to #1596 this
 * file instead REFUSED any rect from a non-1.0 monitor (#1557), which is why a
 * fractionally-scaled desktop produced no present origin at all rather than a
 * converted one.
 *
 * Degradation: extension absent / bus unreachable / no matching window / the
 * window is on no monitor → comp_vk_native_wl_geom_get_window_rect returns
 * false and the compositor stays display-scoped, exactly the pre-#817 Wayland
 * behavior.
 *
 * Only built when XRT_HAVE_WAYLAND && XRT_HAVE_DBUS.
 *
 * @ingroup comp_vk_native
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_vk_native_wl_geom;

/*!
 * Connect to the session bus and take an initial GetWindows snapshot.
 * Subscribes to WindowsChanged so later queries are served from cache.
 *
 * Never fails hard: returns a provider even when the extension is missing
 * (queries then return false; a WARN is logged once). Returns NULL only on
 * out-of-memory / no session bus at all.
 */
struct comp_vk_native_wl_geom *
comp_vk_native_wl_geom_create(void);

/*!
 * The calling process's window, in DEVICE pixels, as this provider resolves it.
 *
 * Every length here is device pixels. The ORIGIN is deliberately relative to
 * the window's own monitor rather than global: a mixed-scale Wayland layout has
 * no single global device grid (two outputs at different scales cannot tile
 * one), but a displacement inside one output is exact, and it is precisely what
 * the weave phase needs. The caller adds the runtime's own resolved panel
 * origin back on — see `comp_vk_native_compositor_get_window_metrics`.
 */
struct comp_vk_native_wl_window_rect
{
	/*!
	 * The window's CONTENT — the bound surface, where the runtime's pixels
	 * land — top-left in DEVICE px, RELATIVE to its monitor's top-left.
	 *
	 * Mutter's buffer rect when the publisher reports it, else the frame
	 * (#1654): with client-side decorations the frame includes a title bar
	 * that is not part of the surface. Undecorated, the two are equal.
	 */
	int32_t left_px, top_px;
	//! Content size in DEVICE px (same source as the origin).
	uint32_t width_px, height_px;
	//! The monitor's own size in DEVICE px. The caller compares this against
	//! the panel's native size to decide whether the window is on the 3D panel
	//! at all — the check that would have caught the 2026-09-20 session, where
	//! the surface fullscreened on the laptop and wove anyway.
	uint32_t monitor_width_px, monitor_height_px;
	//! The factor applied. 1.0 means the wire values were already device px.
	float scale;
	//! The committed SURFACE's size in DEVICE px (the compositor's buffer rect
	//! for the window), or 0 when the publisher does not report it. Differs
	//! from width_px/height_px when the client's buffer is not mapped to its
	//! configured size — the surface is then painted at this size, not that one.
	uint32_t surface_width_px, surface_height_px;
	/*!
	 * False when the committed surface spills PAST the window frame — a
	 * buffer not mapped to its configured size (#1653). True when it lies
	 * inside it (undecorated, or client-side decorations around it), and
	 * when nothing is known.
	 */
	bool surface_within_frame;
	//! Content top minus frame top, DEVICE px: the client-side title bar's
	//! height, 0 without one. Diagnostic only.
	int32_t frame_inset_top_px;

	/*!
	 * The window FRAME's top-left in the publisher's LOGICAL stage
	 * coordinates — the space @ref comp_vk_native_wl_geom_move_window speaks,
	 * and the only field here that is not device pixels (its name says so).
	 */
	int32_t frame_logical_x, frame_logical_y;

	/*!
	 * An interactive grab (move / resize) is in progress on this window: the
	 * user is still dragging it, so nothing may reposition it (#1609).
	 * Requires publisher version 3; @ref have_moving is false against an older
	 * one, and the caller must then settle on geometry alone.
	 */
	bool moving;
	bool have_moving;
};

/*!
 * Current rect of the calling process's window, converted to DEVICE pixels.
 *
 * Pumps pending D-Bus messages (non-blocking), then picks the best window
 * owned by this PID: focused first, else the largest. Reports the buffer rect
 * (the bound surface) when published, else the frame (#1654; #817 carries the
 * original frame-vs-buffer validation note).
 *
 * Requires the payload's `monitor` object: without it there is no scale and no
 * monitor rect, so neither the conversion nor the on-the-panel check can be
 * made, and this refuses rather than serving a rect whose space it cannot name.
 * Mutter omits it only for a window on no monitor.
 *
 * @return true when a matching window with a live, convertible rect was found.
 */
bool
comp_vk_native_wl_geom_get_window_rect(struct comp_vk_native_wl_geom *g,
                                       struct comp_vk_native_wl_window_rect *out_rect);

/*!
 * Ask the compositor to move this process's window so its FRAME's top-left
 * lands on (@p frame_logical_x, @p frame_logical_y) — the publisher's logical
 * stage coordinates, i.e. the space @ref comp_vk_native_wl_window_rect
 * ::frame_logical_x reports.
 *
 * This is the one thing a Wayland client cannot do for itself and the
 * lenticular phase needs after a drag (#1609). The publisher only ever moves
 * windows of the CALLING process, and refuses while the user is still
 * dragging.
 *
 * @return true when the compositor reported the window moved. False covers
 *         "publisher too old to have the method" as well as a refusal, and is
 *         never fatal: the window simply stays where the user dropped it.
 */
bool
comp_vk_native_wl_geom_move_window(struct comp_vk_native_wl_geom *g, int32_t frame_logical_x, int32_t frame_logical_y);

void
comp_vk_native_wl_geom_destroy(struct comp_vk_native_wl_geom **g_ptr);

#ifdef __cplusplus
}
#endif
