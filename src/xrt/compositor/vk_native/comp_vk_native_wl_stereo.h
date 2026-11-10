// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Full side-by-side declaration of the presented Wayland surface.
 *
 * A display processor whose output is the views side by side, each at the
 * resolution it was rendered at (@ref XRT_DP_STEREO_LAYOUT_SIDE_BY_SIDE_FULL),
 * hands a stereo desktop compositor a picture it shows per eye. The compositor
 * learns that from one declaration on the surface: KDE's
 * `kde_stereo_content_v1` (value 3, side by side, full width per eye, left
 * first — the value of HDMI 1.4b's 3D structure "Side-by-Side (Full)"), which
 * Stereo KWin implements. From version 2 a declared surface is one view wide:
 * the window keeps its size on screen while its buffer holds both views.
 *
 * Generated from the vendored protocol XML (src/external/wayland-protocols); no
 * library beyond libwayland-client. On a compositor without the global (GNOME,
 * an unpatched KWin) nothing is declared and the caller says so.
 *
 * Wayland only. Requests go out on the app's connection through a private
 * queue, like the move-sync tag (comp_vk_native_wl_move_sync): the declaration
 * takes effect with the surface's next commit, the one the WSI makes inside
 * vkQueuePresentKHR, so it reaches the compositor with the first buffer that
 * holds both views.
 *
 * @ingroup comp_vk_native
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_vk_native_wl_stereo;

/*!
 * Prepare the declaration of @p wl_surface, the surface the runtime presents to,
 * on @p wl_display (the app's connection). Sends nothing: the compositor's
 * globals are asked for on the first comp_vk_native_wl_stereo_set_full_sbs(true),
 * so a session that never presents full side by side adds no traffic.
 *
 * @return NULL only without a display or a surface, or out of memory.
 */
struct comp_vk_native_wl_stereo *
comp_vk_native_wl_stereo_create(void *wl_display, void *wl_surface);

/*!
 * Declare the surface full side by side (@p full_sbs true) or nothing. Takes
 * effect with the surface's next commit; call it before the present that
 * carries the first buffer of the new layout.
 *
 * The first call with true binds the stereo registry (through a private
 * event queue, one roundtrip) and makes the surface's declaration object.
 *
 * @param[out] out_reason Set to a static sentence when false is returned: why
 *                        this compositor cannot take the declaration. A
 *                        failed bind is not retried.
 * @return false when nothing could be declared.
 */
bool
comp_vk_native_wl_stereo_set_full_sbs(struct comp_vk_native_wl_stereo *s, bool full_sbs, const char **out_reason);

//! The version of `kde_stereo_content_manager_v1` that was bound (0 before the first declaration).
uint32_t
comp_vk_native_wl_stereo_version(const struct comp_vk_native_wl_stereo *s);

/*!
 * Withdraw the declaration (it resets to none with the surface's next commit)
 * and free everything. Before the surface is destroyed.
 */
void
comp_vk_native_wl_stereo_destroy(struct comp_vk_native_wl_stereo **s_ptr);

#ifdef __cplusplus
}
#endif
