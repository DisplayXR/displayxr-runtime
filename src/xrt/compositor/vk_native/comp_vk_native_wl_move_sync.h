// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Move-synchronised re-weave on native Wayland — the frame tag (#1748 spike).
 *
 * PROTOTYPE, off by default (DXR_WL_MOVE_SYNC=1). See
 * docs/specs/runtime/wayland-window-geometry.md §9.
 *
 * The idea: during a compositor-driven move, the GNOME extension keeps the
 * window's ACTOR where the frame on screen was woven for, and moves it on only
 * when a frame woven for the new position has been committed. For that it has
 * to know, per committed buffer, which position the buffer was woven for.
 * Mutter exposes no per-commit identifier to an extension, so the runtime
 * attaches one to the commit itself: a 1x1 transparent SYNCHRONISED subsurface
 * of the bound surface, placed at (x mod 256, y mod 256) of the logical content
 * position the frame was woven for. A synchronised subsurface's position is
 * parent state (wl_subsurface.set_position), applied atomically with the
 * parent's next wl_surface.commit — the one the WSI makes inside
 * vkQueuePresentKHR — so the tag and the buffer reach the compositor as one
 * unit and can never disagree.
 *
 * The extension reads the tag from the subsurface actor's position and resolves
 * it against the positions it has recently given the window.
 *
 * Wayland only (core protocol: wl_compositor, wl_subcompositor, wl_shm); no
 * D-Bus here — the registration with the extension is in comp_vk_native_wl_geom.
 *
 * @ingroup comp_vk_native
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_vk_native_wl_move_sync;

/*!
 * Create the tag subsurface on @p wl_surface (the surface the runtime presents
 * to), using the app's @p wl_display on a private event queue.
 *
 * @return NULL when a required global is missing or any request fails; the
 *         caller then simply runs without move sync.
 */
struct comp_vk_native_wl_move_sync *
comp_vk_native_wl_move_sync_create(void *wl_display, void *wl_surface);

/*!
 * Tag the NEXT commit of the bound surface as woven for the content at LOGICAL
 * (@p content_logical_x, @p content_logical_y). Call it after the weave's
 * origin is fixed and before the present that commits the frame.
 */
void
comp_vk_native_wl_move_sync_tag(struct comp_vk_native_wl_move_sync *ms,
                                int32_t content_logical_x,
                                int32_t content_logical_y);

void
comp_vk_native_wl_move_sync_destroy(struct comp_vk_native_wl_move_sync **ms_ptr);

#ifdef __cplusplus
}
#endif
