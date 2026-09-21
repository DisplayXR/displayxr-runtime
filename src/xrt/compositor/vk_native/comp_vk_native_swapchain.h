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
 * The sampling view in the format the APP ASKED FOR (#1589/#1610).
 *
 * @ref comp_vk_native_swapchain_get_image_view returns the NON-decoding view
 * (the UNORM sibling of an sRGB swapchain) that the blit path needs. The
 * compose render pass composites in linear light and wants the opposite: the
 * GPU decoding an `_SRGB` source on sample. Identical for a non-sRGB
 * swapchain.
 *
 * @ingroup comp_vk_native
 */
uint64_t
comp_vk_native_swapchain_get_true_image_view(struct xrt_swapchain *xsc, uint32_t index);

/*!
 * Was this swapchain created with an `_SRGB` colour format?
 *
 * Answered from the app's requested format, NEVER from the image's: the image
 * is the UNORM sibling whenever DXR_VK_SWAPCHAIN_TRUE_FORMAT is off, so
 * `comp_vk_native_swapchain_is_true_srgb()` (which asks about the IMAGE) is a
 * different question and the wrong one for a colour decision.
 *
 * @ingroup comp_vk_native
 */
bool
comp_vk_native_swapchain_is_srgb(struct xrt_swapchain *xsc);

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

/*!
 * #1559: true when this swapchain's `VkImage`s really are in the `*_SRGB`
 * format the app asked for, rather than the UNORM sibling the runtime reads.
 *
 * Two callers care: the compose blit (which must stage a raw copy first, see
 * comp_vk_native_swapchain_stage_unorm_copy()) and the zero-copy gate, which
 * must stay off for these so no display processor is handed an sRGB image.
 *
 * @ingroup comp_vk_native
 */
bool
comp_vk_native_swapchain_is_true_srgb(struct xrt_swapchain *xsc);

/*!
 * #1559: record a raw copy of a source rect into this swapchain's UNORM
 * scratch image and return that image, so the compose blit reads the app's
 * bytes without an sRGB decode.
 *
 * No-op returning 0 for a swapchain whose images are already in the runtime's
 * raw format — and also on scratch-allocation failure, so the caller can
 * simply fall back to blitting the app image. The copy lands at the SAME
 * offsets it had in the app image, so only the array layer changes (to 0);
 * the caller's `srcOffsets` are reusable as-is.
 *
 * The app image must already be in `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`; the
 * returned scratch is left in `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`.
 *
 * @param xsc The swapchain.
 * @param cmd_ptr The `VkCommandBuffer` being recorded.
 * @param index Image index.
 * @param src_x, src_y, src_w, src_h Source rect, clamped to the image.
 * @param array_layer Source array layer.
 * @return The scratch `VkImage` as uint64_t, or 0 if the caller should use the
 *         app image directly.
 *
 * @ingroup comp_vk_native
 */
uint64_t
comp_vk_native_swapchain_stage_unorm_copy(struct xrt_swapchain *xsc,
                                          void *cmd_ptr,
                                          uint32_t index,
                                          int32_t src_x,
                                          int32_t src_y,
                                          uint32_t src_w,
                                          uint32_t src_h,
                                          uint32_t array_layer);

#ifdef __cplusplus
}
#endif
