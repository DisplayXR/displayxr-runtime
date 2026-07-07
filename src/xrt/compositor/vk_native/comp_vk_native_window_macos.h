// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS window helper for the VK native compositor.
 *
 * Provides NSWindow/NSView management for Vulkan presentation on macOS
 * via VK_EXT_metal_surface. Modeled on the Metal compositor's
 * CompMetalView pattern.
 *
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_vk_native_window_macos;

/*!
 * Create a self-owned NSWindow with a CAMetalLayer-backed view.
 *
 * @param width                 Requested window width in points.
 * @param height                Requested window height in points.
 * @param screen_left           Window left edge in top-down global coordinates
 *                              (origin = primary screen top-left) — the 3D
 *                              panel position the vendor plug-in reports via
 *                              xsysc->info.display_screen_left. (0, 0) means
 *                              primary screen. Positions outside every
 *                              NSScreen fall back to the primary. #715.
 * @param screen_top            Window top edge, same space as @p screen_left.
 * @param transparent_background When true, the NSWindow + CAMetalLayer are
 *                              configured non-opaque (clear background) so the
 *                              desktop shows through alpha < 1 regions.
 * @param out_win Pointer to receive the created window handle.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 */
xrt_result_t
comp_vk_native_window_macos_create(uint32_t width,
                                    uint32_t height,
                                    int32_t screen_left,
                                    int32_t screen_top,
                                    bool transparent_background,
                                    struct comp_vk_native_window_macos **out_win);

/*!
 * Set up presentation on an app-provided NSView.
 *
 * If the view's layer is already a CAMetalLayer, it is used directly.
 * Otherwise, a CAMetalLayer is added as a sublayer.
 *
 * @param ns_view  The app's NSView (as void*).
 * @param transparent_background When true, force the CAMetalLayer non-opaque
 *                              so the desktop shows through alpha < 1 regions.
 *                              Enforced runtime-side (not relying on the app's
 *                              layer.opaque, which AppKit may reset on a
 *                              layer-backed view) right before VkSurface creation.
 * @param out_win  Pointer to receive the window handle.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 */
xrt_result_t
comp_vk_native_window_macos_setup_external(void *ns_view,
                                            bool transparent_background,
                                            struct comp_vk_native_window_macos **out_win);

/*!
 * Get the CAMetalLayer for VkSurfaceKHR creation.
 *
 * @param win The window handle.
 *
 * @return The CAMetalLayer as void*, or NULL.
 */
void *
comp_vk_native_window_macos_get_layer(struct comp_vk_native_window_macos *win);

/*!
 * Get the current backing pixel dimensions.
 *
 * Computed live from the view's bounds (so it tracks window resize, #524);
 * falls back to the CAMetalLayer drawableSize when no view is available.
 *
 * @param win        The window handle.
 * @param out_width  Pointer to receive width in pixels.
 * @param out_height Pointer to receive height in pixels.
 */
void
comp_vk_native_window_macos_get_dimensions(struct comp_vk_native_window_macos *win,
                                            uint32_t *out_width,
                                            uint32_t *out_height);

/*!
 * Sync the CAMetalLayer drawableSize to the live view backing size.
 *
 * MoltenVK derives the VkSurface currentExtent from the layer's
 * drawableSize, so this must be called before recreating the swapchain
 * on a window resize (#524).
 *
 * @param win The window handle.
 */
void
comp_vk_native_window_macos_sync_drawable_size(struct comp_vk_native_window_macos *win);

/*!
 * Get the view's on-screen position in top-down backing pixels,
 * relative to the origin of the screen the window is on.
 *
 * @param win          The window handle.
 * @param out_left_px  Pointer to receive the view's left edge in pixels.
 * @param out_top_px   Pointer to receive the view's top edge in pixels.
 *
 * @return true if the position could be determined.
 */
bool
comp_vk_native_window_macos_get_screen_position(struct comp_vk_native_window_macos *win,
                                                 int32_t *out_left_px,
                                                 int32_t *out_top_px);

/*!
 * Check if the window is still valid (not closed by user).
 *
 * @param win The window handle.
 *
 * @return true if the window is still open.
 */
bool
comp_vk_native_window_macos_is_valid(struct comp_vk_native_window_macos *win);

/*!
 * Destroy the window helper and release resources.
 *
 * @param win_ptr Pointer to window handle (set to NULL after destruction).
 */
void
comp_vk_native_window_macos_destroy(struct comp_vk_native_window_macos **win_ptr);

#ifdef __cplusplus
}
#endif
