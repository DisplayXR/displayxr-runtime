// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Direct-scanout window backend for the VK native compositor on Linux.
 *
 * Alternative to comp_vk_native_window_xcb: instead of presenting through a
 * desktop VK_KHR_xcb_surface window (which Xorg + the desktop compositor then
 * composite — ~44% of the GPU at 4K, see ST-5539), this backend takes exclusive
 * ownership of the 3D-display connector and scans out to it directly, bypassing
 * the compositor entirely.
 *
 * Acquisition path (Xorg): steal the RandR output from the running X server via
 * vkGetRandROutputDisplayEXT (RROutput -> VkDisplayKHR) + vkAcquireXlibDisplayEXT
 * (VK_EXT_acquire_xlib_display / VK_EXT_direct_mode_display), then build a
 * display-plane VkSurfaceKHR (vkCreateDisplayPlaneSurfaceKHR) the target
 * consumes exactly like any other surface. The connector goes dark for the
 * desktop while we own it — this is a FULLSCREEN-ONLY, opt-in path
 * (DXR_LINUX_DIRECT_SCANOUT=1). The Wayland / DRM-lease acquisition
 * (VK_EXT_acquire_drm_display) is a documented future path, not built here.
 *
 * Unlike the XCB backend — which defers surface creation to the target — this
 * backend OWNS the VkSurfaceKHR (only it has the display/mode/plane context to
 * build it) and hands the finished surface to the target; the target must not
 * destroy an externally-provided surface (see comp_vk_native_target's
 * external-surface handling). See docs/roadmap/linux-support.md and
 * ST-5539 (sw-runtime-linux).
 *
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_results.h"
#include "vk/vk_helpers.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_vk_native_window_direct;

/*!
 * Acquire the 3D-display connector and build a direct-scanout surface.
 *
 * Enumerates the physical device's displays, selects the target connector
 * (see @p screen_left / @p screen_top and EDID matching below), acquires it
 * from the X server, picks its native display mode + a compatible plane, and
 * creates the display-plane VkSurfaceKHR.
 *
 * All of the required instance extensions (VK_KHR_display,
 * VK_EXT_direct_mode_display, VK_EXT_acquire_xlib_display) are OPTIONAL at
 * bundle-build time, so this call fails cleanly (leaving @p out_win NULL) when
 * any is missing — the caller then falls back to the XCB path. Failure here is
 * expected on non-direct-capable boxes and must not be fatal.
 *
 * @param vk            The compositor's already-created Vulkan bundle (needs a
 *                      valid instance + physical_device; the display/acquire
 *                      function pointers are loaded into it when the exts are
 *                      enabled).
 * @param screen_left   Target connector left edge in root-window pixels — the
 *                      3D-panel origin the vendor plug-in reports via
 *                      xsysc->info.display_screen_left. Used to disambiguate
 *                      the RandR output when EDID matching is inconclusive.
 * @param screen_top    Target connector top edge in root-window pixels.
 * @param out_win       Receives the created backend on success, NULL on failure.
 *
 * @return XRT_SUCCESS on success; an error code (and NULL @p out_win) on any
 *         failure — extension missing, no matching connector, acquire denied.
 */
xrt_result_t
comp_vk_native_window_direct_create(struct vk_bundle *vk,
                                    int32_t screen_left,
                                    int32_t screen_top,
                                    struct comp_vk_native_window_direct **out_win);

/*!
 * Get the finished display-plane surface for swapchain creation. The backend
 * owns this surface; the target borrows it and must NOT destroy it.
 */
VkSurfaceKHR
comp_vk_native_window_direct_get_surface(struct comp_vk_native_window_direct *win);

/*!
 * Get the acquired connector's native mode dimensions in pixels. These are the
 * scanout dimensions the swapchain must match (a direct-mode surface has no
 * "resize" — the mode is fixed for the session).
 */
void
comp_vk_native_window_direct_get_dimensions(struct comp_vk_native_window_direct *win,
                                            uint32_t *out_width,
                                            uint32_t *out_height);

/*!
 * Whether the backend still owns a valid acquired display. Direct scanout has
 * no user-close event, so this only turns false on teardown; kept for
 * signature parity with the XCB backend's per-frame validity check.
 */
bool
comp_vk_native_window_direct_is_valid(struct comp_vk_native_window_direct *win);

/*!
 * Destroy the display-plane surface, release the display back to the X server
 * (vkReleaseDisplayEXT), close the X connection, and free the backend
 * (sets *win_ptr to NULL). Call AFTER the target's swapchain is torn down.
 */
void
comp_vk_native_window_direct_destroy(struct comp_vk_native_window_direct **win_ptr);

#ifdef __cplusplus
}
#endif
