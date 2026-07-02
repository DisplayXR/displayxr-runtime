// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan native swapchain implementation.
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_compositor.h"

// Forward declarations
struct comp_vk_native_compositor;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a Vulkan native swapchain.
 *
 * Creates VkImages that the application can render to directly.
 * No multi-compositor involvement.
 *
 * @param c The Vulkan native compositor (opaque).
 * @param info Swapchain creation info.
 * @param out_xsc Pointer to receive the created swapchain.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_swapchain_create(struct comp_vk_native_compositor *c,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_swapchain **out_xsc);

/*!
 * Get the VkImageView for a swapchain image (for sampling).
 *
 * @param xsc The swapchain.
 * @param index Image index.
 * @return The VkImageView as uint64_t, or 0 if not available.
 *
 * @ingroup comp_vk_native
 */
uint64_t
comp_vk_native_swapchain_get_image_view(struct xrt_swapchain *xsc, uint32_t index);

/*!
 * Get the VkImage for a swapchain image.
 *
 * @param xsc The swapchain.
 * @param index Image index.
 * @return The VkImage as uint64_t, or 0 if not available.
 *
 * @ingroup comp_vk_native
 */
uint64_t
comp_vk_native_swapchain_get_image(struct xrt_swapchain *xsc, uint32_t index);

/*!
 * Get the dimensions of a swapchain.
 *
 * @param xsc The swapchain.
 * @param[out] out_w Width in pixels.
 * @param[out] out_h Height in pixels.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_swapchain_get_dimensions(struct xrt_swapchain *xsc, uint32_t *out_w, uint32_t *out_h);

/*!
 * Get the array size of a swapchain (1 for plain 2D swapchains).
 *
 * The zone alpha-over draw path samples through the swapchain's whole-image
 * view, which is a 2D view only when array_size == 1 — layered swapchains
 * fall back to the blit path.
 *
 * @ingroup comp_vk_native
 */
uint32_t
comp_vk_native_swapchain_get_array_size(struct xrt_swapchain *xsc);

#ifdef __cplusplus
}
#endif
