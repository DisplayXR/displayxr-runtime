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

#ifdef __cplusplus
}
#endif
