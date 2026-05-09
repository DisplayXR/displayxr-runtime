// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan presentation target (Win32 surface + VkSwapchainKHR).
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_vulkan_includes.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
struct comp_vk_native_target;
struct comp_vk_native_compositor;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a Vulkan presentation target (Win32 surface + swapchain).
 *
 * @param c The Vulkan native compositor.
 * @param hwnd The window handle to present to.
 * @param width Preferred width.
 * @param height Preferred height.
 * @param transparent_background If true, request VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
 *        on the swapchain (falls back to INHERIT, then OPAQUE if neither is supported).
 * @param out_target Pointer to receive the created target.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_target_create(struct comp_vk_native_compositor *c,
                              void *hwnd,
                              uint32_t width,
                              uint32_t height,
                              bool transparent_background,
                              struct comp_vk_native_target **out_target);

/*!
 * Destroy a Vulkan presentation target.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_target_destroy(struct comp_vk_native_target **target_ptr);

/*!
 * Acquire the next swapchain image for rendering.
 *
 * @param target The target.
 * @param out_index Index of the acquired image.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_target_acquire(struct comp_vk_native_target *target, uint32_t *out_index);

/*!
 * Present the rendered image.
 *
 * @param target The target.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_target_present(struct comp_vk_native_target *target);

/*!
 * Get target dimensions.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_target_get_dimensions(struct comp_vk_native_target *target,
                                      uint32_t *out_width,
                                      uint32_t *out_height);

/*!
 * Get the current swapchain image and image view for direct rendering.
 *
 * @param target The target.
 * @param out_image VkImage of the current swapchain image (as uint64_t).
 * @param out_view VkImageView of the current swapchain image (as uint64_t).
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_target_get_current_image(struct comp_vk_native_target *target,
                                         uint64_t *out_image,
                                         uint64_t *out_view);

/*!
 * Get the target swapchain image format.
 *
 * @ingroup comp_vk_native
 */
VkFormat
comp_vk_native_target_get_format(struct comp_vk_native_target *target);

/*!
 * Resize the target swapchain.
 *
 * @param target The target.
 * @param width New width.
 * @param height New height.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_target_resize(struct comp_vk_native_target *target,
                               uint32_t width,
                               uint32_t height);

#ifdef __cplusplus
}
#endif
