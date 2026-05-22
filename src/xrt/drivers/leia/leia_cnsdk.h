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
 * Fetch native display metrics from CNSDK's device config.
 *
 * Returns false (without touching outputs) until @ref leia_cnsdk_create
 * has finished async-initializing the underlying leia_core. Caller is
 * expected to poll across frames.
 *
 * @param[out] out_width_m   Display physical width in meters.
 * @param[out] out_height_m  Display physical height in meters.
 * @param[out] out_pixel_w   Panel pixel width.
 * @param[out] out_pixel_h   Panel pixel height.
 * @return true if all outputs were populated.
 */
bool
leia_cnsdk_get_display_metrics(struct leia_cnsdk *cnsdk,
                               float *out_width_m,
                               float *out_height_m,
                               uint32_t *out_pixel_w,
                               uint32_t *out_pixel_h);

/*!
 * Non-blocking check for whether CNSDK face tracking is running.
 *
 * Enable + start happens asynchronously on a worker thread spawned by
 * @ref leia_cnsdk_create, so callers can poll this every frame from the
 * render thread without stalling. Returns true once the worker has
 * finished enabling + starting; false until then (or permanently if
 * the enable call failed).
 *
 * @return true once face tracking is started.
 */
bool
leia_cnsdk_ensure_face_tracking_started(struct leia_cnsdk *cnsdk);

/*!
 * Fetch the latest predicted primary face position from CNSDK.
 *
 * Returns false until face tracking is running and CNSDK has a face
 * lock. Position is in meters; coordinate system matches xrt_eye_position
 * (x = right, y = up, z = toward viewer, origin = display center).
 *
 * @param[out] out_x  Face position X (meters).
 * @param[out] out_y  Face position Y (meters).
 * @param[out] out_z  Face position Z (meters, distance from display).
 * @return true if a valid face was returned.
 */
bool
leia_cnsdk_get_primary_face(struct leia_cnsdk *cnsdk,
                            float *out_x,
                            float *out_y,
                            float *out_z);

/*!
 * Perform CNSDK Vulkan interlacing.
 *
 * Lazily creates the interlacer on first call after the core is ready.
 *
 * @param cnsdk           Opaque CNSDK handle.
 * @param device          Vulkan logical device.
 * @param physDev         Vulkan physical device.
 * @param left            Image view for the left eye.
 * @param right           Image view for the right eye.
 * @param targetFmt       Format of the target / swapchain image.
 * @param w               Target width in pixels.
 * @param h               Target height in pixels.
 * @param fb              Target framebuffer.
 * @param targetImage     Target VkImage (for layout transitions).
 * @param waitSemaphore   Binary semaphore CNSDK should wait on before
 *                        sampling the input views (use to chain a prior
 *                        upload submit into the weave on the GPU side and
 *                        avoid a host stall). Pass VK_NULL_HANDLE to skip.
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
                 VkImage targetImage,
                 VkSemaphore waitSemaphore);

#ifdef __cplusplus
}
#endif
