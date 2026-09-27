// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Move-synchronised re-weave on native Wayland — the frame tag (#1748).
 *
 * On by default whenever the GNOME extension supports it (EnableMoveSync,
 * extension version 9+); DXR_WL_MOVE_SYNC=0 turns it off. See
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

/*!
 * The tag is the woven position modulo this, in logical px. The extension
 * resolves it against the positions it gave the window in the last second, so
 * it only has to separate positions a few frames apart. Must match the
 * extension's MOVE_SYNC_TAG_MOD.
 */
#define COMP_VK_NATIVE_WL_MOVE_SYNC_TAG_MOD 256

//! One axis of a woven LOGICAL position, as the tag subsurface's offset:
//! v mod 256, non-negative for any v (a window may sit left of / above the
//! stage origin on a multi-monitor layout).
static inline int32_t
comp_vk_native_wl_move_sync_encode(int32_t logical)
{
	const int32_t m = COMP_VK_NATIVE_WL_MOVE_SYNC_TAG_MOD;
	return ((logical % m) + m) % m;
}

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
 * (@p content_logical_x, @p content_logical_y). Call it under the same lock as,
 * and immediately before, the present that commits the frame: any other
 * commit in between would carry this frame's tag.
 */
void
comp_vk_native_wl_move_sync_tag(struct comp_vk_native_wl_move_sync *ms,
                                int32_t content_logical_x,
                                int32_t content_logical_y);

/*!
 * Attach (true) or detach (false) the tag's pixel, with the bound surface's
 * next commit. Detached while the window covers its whole monitor: an extra
 * mapped subsurface can keep mutter from scanning a fullscreen window out
 * directly (the opaque-region path), and a fullscreen window is never dragged
 * anyway.
 */
void
comp_vk_native_wl_move_sync_set_mapped(struct comp_vk_native_wl_move_sync *ms, bool mapped);

void
comp_vk_native_wl_move_sync_destroy(struct comp_vk_native_wl_move_sync **ms_ptr);

#ifdef __cplusplus
}
#endif
