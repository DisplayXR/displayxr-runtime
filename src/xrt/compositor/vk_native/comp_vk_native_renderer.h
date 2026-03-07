// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan renderer for layer compositing.
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
struct comp_vk_native_renderer;
struct comp_vk_native_compositor;
struct comp_layer_accum;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a Vulkan renderer for layer compositing.
 *
 * @param c The Vulkan native compositor.
 * @param view_width Width of one view (half of stereo texture width).
 * @param view_height Height of the views.
 * @param target_height Height of the render target (window).
 * @param out_renderer Pointer to receive the created renderer.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_renderer_create(struct comp_vk_native_compositor *c,
                                uint32_t view_width,
                                uint32_t view_height,
                                uint32_t target_height,
                                struct comp_vk_native_renderer **out_renderer);

/*!
 * Destroy the Vulkan renderer.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_renderer_destroy(struct comp_vk_native_renderer **renderer_ptr);

/*!
 * Render all accumulated layers to the side-by-side stereo texture.
 *
 * @param renderer The renderer.
 * @param layers The accumulated layers.
 * @param left_eye Left eye position (NULL for default).
 * @param right_eye Right eye position (NULL for default).
 * @param target_width Width of the render target (window).
 * @param target_height Height of the render target (window).
 * @param force_mono If true, render only 1 view (mono).
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_renderer_draw(struct comp_vk_native_renderer *renderer,
                              struct comp_layer_accum *layers,
                              struct xrt_vec3 *left_eye,
                              struct xrt_vec3 *right_eye,
                              uint32_t target_width,
                              uint32_t target_height,
                              bool force_mono);

/*!
 * Get the stereo texture left and right VkImageViews for the display processor.
 *
 * @param renderer The renderer.
 * @param out_left_view VkImageView for left eye (as uint64_t).
 * @param out_right_view VkImageView for right eye (as uint64_t).
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_renderer_get_stereo_views(struct comp_vk_native_renderer *renderer,
                                          uint64_t *out_left_view,
                                          uint64_t *out_right_view);

/*!
 * Get the stereo texture VkImage.
 *
 * @param renderer The renderer.
 * @return VkImage as uint64_t.
 *
 * @ingroup comp_vk_native
 */
uint64_t
comp_vk_native_renderer_get_stereo_image(struct comp_vk_native_renderer *renderer);

/*!
 * Get stereo texture dimensions.
 *
 * @param renderer The renderer.
 * @param out_view_width Width of one view.
 * @param out_view_height Height of views.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_renderer_get_view_dimensions(struct comp_vk_native_renderer *renderer,
                                             uint32_t *out_view_width,
                                             uint32_t *out_view_height);

/*!
 * Get the stereo texture format.
 *
 * @param renderer The renderer.
 * @return VkFormat as int32_t.
 *
 * @ingroup comp_vk_native
 */
int32_t
comp_vk_native_renderer_get_format(struct comp_vk_native_renderer *renderer);

/*!
 * Resize the renderer's stereo texture.
 *
 * @param renderer The renderer.
 * @param new_view_width New width per view.
 * @param new_view_height New height per view.
 * @param new_target_height New render target height.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_renderer_resize(struct comp_vk_native_renderer *renderer,
                                uint32_t new_view_width,
                                uint32_t new_view_height,
                                uint32_t new_target_height);

/*!
 * Blit the stereo texture to a target image with stretching.
 * Used for mono/2D fallback.
 *
 * @param renderer The renderer.
 * @param cmd Command buffer to record into.
 * @param dst_image Destination VkImage.
 * @param dst_width Destination width.
 * @param dst_height Destination height.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_renderer_blit_to_target(struct comp_vk_native_renderer *renderer,
                                        void *cmd,
                                        uint64_t dst_image,
                                        uint32_t dst_width,
                                        uint32_t dst_height);

/*!
 * Get the command pool for recording commands.
 *
 * @param renderer The renderer.
 * @return VkCommandPool as uint64_t.
 *
 * @ingroup comp_vk_native
 */
uint64_t
comp_vk_native_renderer_get_cmd_pool(struct comp_vk_native_renderer *renderer);

#ifdef __cplusplus
}
#endif
