// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native Vulkan compositor that renders directly without multi-compositor.
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_display_metrics.h"

// Forward declarations
struct comp_vk_native_compositor;
struct xrt_system_devices;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a native Vulkan compositor.
 *
 * This compositor renders directly using Vulkan without the multi-compositor,
 * following the same pattern as the D3D11 native compositor.
 *
 * @param xdev The device we are displaying to.
 * @param hwnd The window handle from XR_EXT_win32_window_binding (or NULL for own window).
 * @param vk_instance The app's VkInstance.
 * @param vk_physical_device The app's VkPhysicalDevice.
 * @param vk_device The app's VkDevice.
 * @param queue_family_index Queue family index for graphics.
 * @param queue_index Queue index within the family.
 * @param dp_factory_vk Display processor factory (xrt_dp_factory_vk_fn_t), or NULL.
 * @param shared_texture_handle Shared texture HANDLE for offscreen mode, or NULL.
 * @param transparent_background Request transparent (PRE_MULTIPLIED) compositeAlpha
 *        on the presented swapchain. Forwarded to the display processor as the
 *        chroma-key gate.
 * @param chroma_key_color 0x00BBGGRR. When @p transparent_background is true the
 *        display processor uses this color in its pre-weave fill / post-weave strip
 *        passes. Pass 0 to let the DP pick a content-safe default (magenta).
 * @param out_xc Pointer to receive the created compositor.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_compositor_create(struct xrt_device *xdev,
                                 void *hwnd,
                                 void *vk_instance,
                                 void *vk_physical_device,
                                 void *vk_device,
                                 uint32_t queue_family_index,
                                 uint32_t queue_index,
                                 void *dp_factory_vk,
                                 void *shared_texture_handle,
                                 bool transparent_background,
                                 uint32_t chroma_key_color,
                                 int32_t display_screen_left,
                                 int32_t display_screen_top,
                                 struct xrt_compositor_native **out_xc);

/*!
 * Get the predicted eye positions from the display processor.
 *
 * @param xc The compositor.
 * @param out_eye_pos Output eye positions (N-view).
 *
 * @return true if eye tracking is available and positions were retrieved.
 *
 * @ingroup comp_vk_native
 */
bool
comp_vk_native_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                      struct xrt_eye_positions *out_eye_pos);

/*!
 * Get the display dimensions from the display processor.
 *
 * @param xc The compositor.
 * @param out_width_m Pointer to receive display width in meters.
 * @param out_height_m Pointer to receive display height in meters.
 *
 * @return true if dimensions are available.
 *
 * @ingroup comp_vk_native
 */
bool
comp_vk_native_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                                  float *out_width_m,
                                                  float *out_height_m);

/*!
 * Get window metrics for adaptive FOV and eye position adjustment.
 *
 * @param xc The compositor.
 * @param[out] out_metrics Pointer to receive the computed window metrics.
 *
 * @return true if valid window metrics are available.
 *
 * @ingroup comp_vk_native
 */
bool
comp_vk_native_compositor_get_window_metrics(struct xrt_compositor *xc,
                                              struct xrt_window_metrics *out_metrics);

/*!
 * Request display mode switch (2D/3D) via display processor.
 *
 * @param xc The compositor.
 * @param enable_3d true to switch to 3D mode, false for 2D mode.
 * @return true on success.
 *
 * @ingroup comp_vk_native
 */
bool
comp_vk_native_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d);

/*!
 * Set the system devices for the debug GUI (needed for qwerty driver support).
 *
 * @param xc The compositor.
 * @param xsysd The system devices (may be NULL to disable qwerty support).
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_compositor_set_system_devices(struct xrt_compositor *xc,
                                              struct xrt_system_devices *xsysd);

/*!
 * Set system compositor info (display dimensions, nominal viewer position, etc.).
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_compositor_set_sys_info(struct xrt_compositor *xc,
                                        const struct xrt_system_compositor_info *info);

/*!
 * Set legacy app tile scaling flag (gates 1/2/3 key mode selection).
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_compositor_set_legacy_app_tile_scaling(struct xrt_compositor *xc, bool legacy);

/*!
 * Set canvas output rect for shared-texture apps.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_compositor_set_output_rect(struct xrt_compositor *xc,
                                           int32_t x, int32_t y,
                                           uint32_t w, uint32_t h);

/*!
 * Register a full-window 2D shared texture for the surround region (Spec v6).
 *
 * On Windows the handle is an NT HANDLE imported via VK_KHR_external_memory_win32;
 * on POSIX, an fd via VK_KHR_external_memory_fd. Pass shared_handle == NULL to
 * clear. See comp_d3d11_compositor_set_surround_2d for the cross-platform
 * semantics.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_compositor_set_surround_2d(struct xrt_compositor *xc,
                                           void *shared_handle,
                                           uint32_t w, uint32_t h);

/*!
 * Get the vk_bundle from a VK native compositor (for sub-modules).
 *
 * @ingroup comp_vk_native
 */
struct vk_bundle *
comp_vk_native_compositor_get_vk(struct comp_vk_native_compositor *c);

/*!
 * Get the queue family index from a VK native compositor (for sub-modules).
 *
 * @ingroup comp_vk_native
 */
uint32_t
comp_vk_native_compositor_get_queue_family(struct comp_vk_native_compositor *c);

/*
 * XR_EXT_local_3d_zone — authored 2D/3D mask consumer, VK leg.
 * Contracts + scoping (the composite consumer rides with Phase 3 — handle
 * apps have no 2D source before the xrEndFrame 2D layer):
 * docs/roadmap/unified-2d-3d-crossapi-impl.md §4.
 *
 * STUBS until that leg lands — all return XRT_ERROR_NOT_IMPLEMENTED and the
 * oxr caps query reports supported = false for VK sessions until then.
 * NOTE: unlike the D3D11/D3D12 guards, XRT_HAVE_VK_NATIVE_COMPOSITOR is
 * defined on every platform, so these must stay real (linkable) symbols.
 */

/*!
 * Create the compositor-side mask state (R8_UNORM image, w×h client px).
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_compositor_zone_mask_create(struct xrt_compositor *xc,
                                           uint32_t w, uint32_t h,
                                           void **out_mask);

/*!
 * Tier 1 — fill the whole mask: all-3D (enable_3d) or all-2D.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_compositor_zone_mask_set_whole(struct xrt_compositor *xc,
                                              void *mask,
                                              bool enable_3d);

/*!
 * Tier 2 — rasterize client-window-pixel rects as the 3D region.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                              void *mask,
                                              uint32_t count,
                                              const struct xrt_rect *rects);

/*!
 * Tier 3 — hand back the VkImage + VkImageView for freeform app drawing
 * (the compositor shares the app's VkDevice; same-queue submission order is
 * the sync contract).
 *
 * @param out_image      Receives a VkImage (as void*).
 * @param out_image_view Receives a VkImageView (as void*).
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_compositor_zone_mask_acquire_rt(struct xrt_compositor *xc,
                                               void *mask,
                                               void **out_image,
                                               void **out_image_view,
                                               uint32_t *out_w,
                                               uint32_t *out_h);

/*!
 * Stage the mask's current contents for the next frame submission.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask);

/*!
 * Destroy the compositor-side mask state.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask);

#ifdef __cplusplus
}
#endif
