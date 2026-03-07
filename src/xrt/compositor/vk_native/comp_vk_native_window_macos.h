// Copyright 2025, Leia Inc.
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
 * @param width   Requested window width in points.
 * @param height  Requested window height in points.
 * @param out_win Pointer to receive the created window handle.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 */
xrt_result_t
comp_vk_native_window_macos_create(uint32_t width,
                                    uint32_t height,
                                    struct comp_vk_native_window_macos **out_win);

/*!
 * Set up presentation on an app-provided NSView.
 *
 * If the view's layer is already a CAMetalLayer, it is used directly.
 * Otherwise, a CAMetalLayer is added as a sublayer.
 *
 * @param ns_view  The app's NSView (as void*).
 * @param out_win  Pointer to receive the window handle.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 */
xrt_result_t
comp_vk_native_window_macos_setup_external(void *ns_view,
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
 * @param win        The window handle.
 * @param out_width  Pointer to receive width in pixels.
 * @param out_height Pointer to receive height in pixels.
 */
void
comp_vk_native_window_macos_get_dimensions(struct comp_vk_native_window_macos *win,
                                            uint32_t *out_width,
                                            uint32_t *out_height);

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
