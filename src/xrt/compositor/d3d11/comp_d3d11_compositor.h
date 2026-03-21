// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native D3D11 compositor that bypasses Vulkan entirely.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_display_metrics.h"

// Forward declarations
struct comp_d3d11_compositor;
struct xrt_system_devices;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a native D3D11 compositor.
 *
 * This compositor renders directly using D3D11 without any Vulkan involvement,
 * solving interop issues on Intel integrated GPUs where importing D3D11 textures
 * into Vulkan fails with VK_ERROR_FORMAT_NOT_SUPPORTED.
 *
 * @param xdev The device we are displaying to.
 * @param hwnd The window handle from XR_EXT_win32_window_binding (or NULL for fullscreen).
 * @param d3d11_device The D3D11 device from the application's graphics binding.
 * @param dp_factory_d3d11 Display processor factory (xrt_dp_factory_d3d11_fn_t), or NULL.
 * @param out_xc Pointer to receive the created compositor.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_compositor_create(struct xrt_device *xdev,
                             void *hwnd,
                             void *d3d11_device,
                             void *dp_factory_d3d11,
                             void *shared_texture_handle,
                             struct xrt_compositor_native **out_xc);

/*!
 * Set the output rect within the app's client area where the shared
 * texture will be displayed. The runtime positions the hidden SR weaver
 * window at this sub-rect for correct interlacing alignment.
 *
 * Only meaningful in shared-texture mode with an app HWND.
 * Call when the blit viewport changes (e.g. on window resize).
 *
 * @param xc The compositor.
 * @param x  Left edge in client-area pixels.
 * @param y  Top edge in client-area pixels.
 * @param w  Width in pixels.
 * @param h  Height in pixels.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_compositor_set_output_rect(struct xrt_compositor *xc,
                                       int32_t x, int32_t y,
                                       uint32_t w, uint32_t h);

/*!
 * Get the predicted eye positions from the display processor.
 *
 * @param xc The compositor.
 * @param out_eye_pos Output eye positions (N-view).
 *
 * @return true if eye tracking is available and positions were retrieved.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
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
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                              float *out_width_m,
                                              float *out_height_m);

/*!
 * Get window metrics for adaptive FOV and eye position adjustment.
 *
 * Computes display pixel info, window client area geometry, and derived
 * physical sizes / center offsets needed for Kooima FOV correction.
 *
 * @param xc The compositor.
 * @param[out] out_metrics Pointer to receive the computed window metrics.
 *
 * @return true if valid window metrics are available.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_get_window_metrics(struct xrt_compositor *xc,
                                          struct xrt_window_metrics *out_metrics);

/*!
 * Request display mode switch (2D/3D) via display processor.
 *
 * @param xc The compositor.
 * @param enable_3d true to switch to 3D mode, false for 2D mode.
 * @return true on success.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d);

/*!
 * Set the system devices for the debug GUI (needed for qwerty driver support).
 *
 * This should be called after creating the compositor when xsysd is available.
 * The debug GUI needs xsysd to route keyboard events to qwerty devices.
 *
 * @param xc The compositor.
 * @param xsysd The system devices (may be NULL to disable qwerty support).
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_compositor_set_system_devices(struct xrt_compositor *xc,
                                          struct xrt_system_devices *xsysd);

/*!
 * Set the legacy app tile scaling flag for the compositor.
 *
 * When true, the compositor disables direct rendering mode selection via
 * qwerty 1/2/3 keys and keeps view dimensions fixed at the compromise scale.
 *
 * @param xc The compositor.
 * @param legacy true if legacy app tile scaling is active.
 * @param scale_x Compromise view scale X (e.g. 0.5 for SBS).
 * @param scale_y Compromise view scale Y (e.g. 1.0 for SBS).
 * @param view_w Recommended per-view width in pixels (from oxr_system_fill_in).
 * @param view_h Recommended per-view height in pixels (from oxr_system_fill_in).
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_compositor_set_legacy_app_tile_scaling(struct xrt_compositor *xc,
                                                   bool legacy,
                                                   float scale_x,
                                                   float scale_y,
                                                   uint32_t view_w,
                                                   uint32_t view_h);

#ifdef __cplusplus
}
#endif
