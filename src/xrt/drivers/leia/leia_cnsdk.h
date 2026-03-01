// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Opaque wrapper around CNSDK (Android) interlacing API.
 *
 * Encapsulates leia_core and leia_interlacer so the compositor
 * does not include CNSDK headers directly.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_results.h"
#include "xrt/xrt_vulkan_includes.h"

#ifdef __cplusplus
extern "C" {
#endif

struct leia_cnsdk;

/*!
 * Create and asynchronously initialise a CNSDK core + backlight.
 *
 * @param[out] out_cnsdk  Receives the opaque handle (NULL on failure).
 * @return XRT_SUCCESS on success.
 */
xrt_result_t
leia_cnsdk_create(struct leia_cnsdk **out_cnsdk);

/*!
 * Destroy a CNSDK handle and release all resources.
 *
 * @param cnsdk_ptr  Pointer to handle; set to NULL on return.
 */
void
leia_cnsdk_destroy(struct leia_cnsdk **cnsdk_ptr);

/*!
 * Check whether the asynchronous core init has completed.
 *
 * @return true once the core is fully initialised.
 */
bool
leia_cnsdk_is_initialized(struct leia_cnsdk *cnsdk);

/*!
 * Perform CNSDK Vulkan interlacing.
 *
 * Lazily creates the interlacer on first call after the core is ready.
 *
 * @param cnsdk        Opaque CNSDK handle.
 * @param device       Vulkan logical device.
 * @param physDev      Vulkan physical device.
 * @param left         Image view for the left eye.
 * @param right        Image view for the right eye.
 * @param targetFmt    Format of the target / swapchain image.
 * @param w            Target width in pixels.
 * @param h            Target height in pixels.
 * @param fb           Target framebuffer.
 * @param targetImage  Target VkImage (for layout transitions).
 */
void
leia_cnsdk_weave(struct leia_cnsdk *cnsdk,
                 VkDevice device,
                 VkPhysicalDevice physDev,
                 VkImageView left,
                 VkImageView right,
                 VkFormat targetFmt,
                 uint32_t w,
                 uint32_t h,
                 VkFramebuffer fb,
                 VkImage targetImage);

#ifdef __cplusplus
}
#endif
