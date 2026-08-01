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
 * the runtime the same window rect X11 gets from xcb_translate_coordinates.
 * The rest of the chain (get_window_metrics → vk_update_present_origin →
 * DP set_present_origin) is source-agnostic and unchanged.
 *
 * Degradation: extension absent / bus unreachable / no matching window →
 * comp_vk_native_wl_geom_get_window_rect returns false and the compositor
 * stays display-scoped, exactly the pre-#817 Wayland behavior.
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
 * Current global rect of the calling process's window, in desktop pixels
 * (Mutter global coordinates — identical to X11 root coordinates at monitor
 * scale 1.0).
 *
 * Pumps pending D-Bus messages (non-blocking), then picks the best window
 * owned by this PID: focused first, else the largest. Uses the frame rect
 * (see #817 for the frame-vs-buffer-rect validation note).
 *
 * @param out_scale  Monitor scale under the window (1.0 when unknown).
 *                   Weaving requires 1.0; the caller decides how to react.
 * @return true when a matching window with a live rect was found.
 */
bool
comp_vk_native_wl_geom_get_window_rect(struct comp_vk_native_wl_geom *g,
                                       int32_t *out_left_px,
                                       int32_t *out_top_px,
                                       uint32_t *out_width_px,
                                       uint32_t *out_height_px,
                                       float *out_scale);

void
comp_vk_native_wl_geom_destroy(struct comp_vk_native_wl_geom **g_ptr);

#ifdef __cplusplus
}
#endif
