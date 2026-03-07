// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native Metal compositor — public C header.
 *
 * Mirrors the D3D11 native compositor: creates Metal swapchains directly,
 * renders layers into a side-by-side stereo texture, optionally weaves
 * through a display processor, and presents to a CAMetalLayer.
 *
 * @author David Fattal
 * @ingroup comp_metal
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_display_metrics.h"

// Forward declarations
struct xrt_system_devices;
struct xrt_window_metrics;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a native Metal compositor.
 *
 * @param xdev            HMD device for rendering info.
 * @param window_handle   App-provided NSView* (NULL = auto-create window).
 * @param command_queue   App's id<MTLCommandQueue> from XrGraphicsBindingMetalKHR.
 * @param dp_factory_metal Display processor factory (may be NULL).
 * @param offscreen       If true and window_handle is NULL, create a hidden
 *                        window (no visible UI). Used when the app provides a
 *                        Cocoa window binding with viewHandle=NULL to signal
 *                        offscreen / shared-texture mode.
 * @param shared_iosurface  IOSurfaceRef for shared texture output, or NULL.
 *                        When non-NULL, the compositor renders into this
 *                        IOSurface instead of the CAMetalLayer drawable.
 * @param[out] out_xc     Created compositor on success.
 * @return XRT_SUCCESS on success.
 *
 * @ingroup comp_metal
 */
xrt_result_t
comp_metal_compositor_create(struct xrt_device *xdev,
                             void *window_handle,
                             void *command_queue,
                             void *dp_factory_metal,
                             bool offscreen,
                             void *shared_iosurface,
                             struct xrt_compositor_native **out_xc);

/*!
 * Get predicted eye positions from the display processor.
 * Returns false if eye tracking is not available.
 */
bool
comp_metal_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                  struct xrt_vec3 *out_left_eye,
                                                  struct xrt_vec3 *out_right_eye);

/*!
 * Get physical display dimensions in meters.
 * Returns false if not available.
 */
bool
comp_metal_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                             float *out_width_m,
                                             float *out_height_m);

/*!
 * Get window metrics for adaptive FOV calculation.
 * Returns false if not available.
 */
bool
comp_metal_compositor_get_window_metrics(struct xrt_compositor *xc,
                                         struct xrt_window_metrics *out_metrics);

/*!
 * Request a display mode switch (2D/3D).
 * Returns false if not supported.
 */
bool
comp_metal_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d);

/*!
 * Set system devices for qwerty driver support.
 */
void
comp_metal_compositor_set_system_devices(struct xrt_compositor *xc,
                                         struct xrt_system_devices *xsysd);

/*!
 * Set system compositor info (for HUD display dimensions/nominal viewer).
 */
void
comp_metal_compositor_set_sys_info(struct xrt_compositor *xc,
                                    const struct xrt_system_compositor_info *info);

/*!
 * Mark the compositor's swapchain content as coming from OpenGL (bottom-up origin).
 * The compositor will flip Y when sampling swapchain textures.
 */
void
comp_metal_compositor_set_source_gl(struct xrt_compositor *xc);

/*!
 * Get the MTLTexture (as void*) for a given swapchain image index.
 * Used by Metal native apps to enumerate swapchain images.
 * The handle stored in xrt_image_native is an IOSurfaceRef for cross-API sharing;
 * this function returns the actual id<MTLTexture> wrapping that IOSurface.
 */
void *
comp_metal_swapchain_get_texture(struct xrt_swapchain *xsc, uint32_t index);

/*!
 * Get the system default Metal device (id<MTLDevice> as void*).
 * Used by xrGetMetalGraphicsRequirementsKHR to return a real device pointer.
 */
void *
comp_metal_get_system_default_device(void);

#ifdef __cplusplus
}
#endif
