// Copyright 2025, The DisplayXR Project
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
 * @param hwnd The window handle from XR_EXT_win32_window_binding / an NSView from
 *        XR_EXT_cocoa_window_binding / a struct comp_vk_native_xlib_handle* from
 *        XR_EXT_xlib_window_binding on desktop Linux (or NULL for own window).
 * @param vk_instance The app's VkInstance.
 * @param vk_physical_device The app's VkPhysicalDevice.
 * @param vk_device The app's VkDevice.
 * @param queue_family_index Queue family index for graphics.
 * @param queue_index Queue index within the family.
 * @param dp_factory_vk Display processor factory (xrt_dp_factory_vk_fn_t), or NULL.
 * @param shared_texture_handle Shared texture HANDLE for offscreen mode, or NULL.
 * @param transparent_background Request transparent (PRE_MULTIPLIED) compositeAlpha
 *        on the presented swapchain. Forwarded to the display processor as the
 *        transparency enable (set_transparent_background).
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
 * Select the eye-tracking control mode (MANAGED=0 / MANUAL=1) on the display
 * processor — the policy counterpart to @ref
 * comp_vk_native_compositor_request_display_mode. No-op if the DP doesn't react.
 *
 * @param xc   The compositor.
 * @param mode 0 = MANAGED, 1 = MANUAL.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_compositor_set_eye_tracking_mode(struct xrt_compositor *xc, uint32_t mode);

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

/*!
 * XR_EXT_display_zones (ADR-027): set the frame's explicit wish for the next
 * layer_commit — @p mask is the compositor-side mask state of the
 * XrLocal3DZoneMaskEXT referenced via XrDisplayZonesFrameEndInfoEXT.wishMask
 * (oxr_local_3d_zone_ext::comp_mask), or NULL to auto-derive the wish from
 * the frame's zone rects. Called by oxr on every zones frame before
 * xrt_comp_layer_commit; consumed by that commit. No-op outside zones frames.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_compositor_zones_set_frame_wish(struct xrt_compositor *xc, void *mask);

/*!
 * #439 Phase 3 Q4 — current recommended per-view render size (renderer view
 * dims, recomputed each frame from the effective canvas). The oxr frame-end
 * poll fires XrEventDataLocal3DZoneViewSizeChangedEXT when this changes.
 *
 * @ingroup comp_vk_native
 */
bool
comp_vk_native_compositor_get_recommended_view_size(struct xrt_compositor *xc, uint32_t *out_w, uint32_t *out_h);

#ifdef __cplusplus
}
#endif
