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
 * The four values are snapshotted once by the face-tracking worker
 * thread right after @ref leia_core_is_initialized first returns true
 * and stored on the wrapper struct. Subsequent calls return the
 * cached values without re-acquiring CNSDK's device config every
 * frame. Returns false until that snapshot has happened; caller is
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
 * Idempotent: lazily create the CNSDK Vulkan interlacer in atlas mode
 * once @ref leia_core_is_initialized returns true. Safe to call every
 * frame. Atlas mode means CNSDK accepts the SBS atlas VkImage+View
 * directly via @ref leia_cnsdk_weave and does the L/R split internally;
 * the DP doesn't have to manage per-view images or per-tile blits.
 *
 * @return true if the interlacer exists and is ready to weave.
 */
bool
leia_cnsdk_ensure_interlacer(struct leia_cnsdk *cnsdk,
                              VkDevice device,
                              VkPhysicalDevice physDev,
                              VkFormat targetFmt);

/*!
 * Fetch the latest predicted primary face position from CNSDK.
 *
 * Returns false until face tracking is running and CNSDK has a face
 * lock. Position is returned in meters relative to the **display
 * center** (matching `xrt_eye_position`'s convention), even though
 * CNSDK natively returns millimeters relative to the camera. The
 * wrapper does the unit conversion + camera-center translation using
 * the cached `cameraCenterX/Y/Z` from `leia_device_config` populated
 * once the core is initialized.
 *
 * @param[out] out_x  Face position X (meters, display-relative).
 * @param[out] out_y  Face position Y (meters, display-relative).
 * @param[out] out_z  Face position Z (meters, +toward viewer).
 * @return true if a valid face was returned.
 */
bool
leia_cnsdk_get_primary_face(struct leia_cnsdk *cnsdk,
                            float *out_x,
                            float *out_y,
                            float *out_z);

/*!
 * Perform CNSDK Vulkan interlacing on an SBS atlas.
 *
 * Atlas mode: pass the runtime's pre-composited SBS atlas image+view
 * directly. CNSDK does the L/R split internally via
 * @ref leia_interlacer_vulkan_set_interlace_view_texture_atlas. The DP
 * does no per-view image management and no per-frame blits, so there's
 * no GPU stall between us and CNSDK — its `do_post_process` records
 * and submits its own cmd buffer when it's ready.
 *
 * Caller must have first invoked @ref leia_cnsdk_ensure_interlacer; if
 * the interlacer isn't ready yet this function is a no-op (no submit,
 * no GPU side effects), making it safe to call every frame during the
 * async core-init window.
 *
 * @param cnsdk         Opaque CNSDK handle.
 * @param device        Vulkan logical device.
 * @param physDev       Vulkan physical device.
 * @param atlas_image   SBS atlas VkImage.
 * @param atlas_view    Matching VkImageView covering the full atlas.
 * @param atlas_width   Atlas width in pixels (= view_w * tile_columns).
 * @param atlas_height  Atlas height in pixels (= view_h * tile_rows).
 * @param targetFmt     Format of the target / swapchain image.
 * @param w             Target width in pixels.
 * @param h             Target height in pixels.
 * @param fb            Target framebuffer.
 * @param targetImage   Target VkImage (for CNSDK-side layout transitions).
 */
void
leia_cnsdk_weave(struct leia_cnsdk *cnsdk,
                 VkDevice device,
                 VkPhysicalDevice physDev,
                 VkImage atlas_image,
                 VkImageView atlas_view,
                 uint32_t atlas_width,
                 uint32_t atlas_height,
                 VkFormat targetFmt,
                 uint32_t w,
                 uint32_t h,
                 VkFramebuffer fb,
                 VkImage targetImage);

#ifdef __cplusplus
}
#endif
