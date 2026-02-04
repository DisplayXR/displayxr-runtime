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
#include "leiasr/leiasr_types.h"

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
 * @param hwnd The window handle from XR_EXT_session_target (or NULL for fullscreen).
 * @param d3d11_device The D3D11 device from the application's graphics binding.
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
                             struct xrt_compositor_native **out_xc);

/*!
 * Get the predicted eye positions from the SR SDK weaver.
 *
 * @param xc The compositor.
 * @param out_left_eye Pointer to receive left eye position (x, y, z in meters).
 * @param out_right_eye Pointer to receive right eye position (x, y, z in meters).
 *
 * @return true if eye tracking is available and positions were retrieved.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                  struct xrt_vec3 *out_left_eye,
                                                  struct xrt_vec3 *out_right_eye);

/*!
 * Get the display dimensions from the SR SDK.
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
                                          struct leiasr_window_metrics *out_metrics);

/*!
 * Check for a pending render resolution change event.
 *
 * Returns true if a resize occurred since the last call, writing the new
 * recommended per-eye render dimensions to @p out_width / @p out_height.
 * The pending flag is cleared after a successful read.
 *
 * @param xc         The compositor.
 * @param out_width  Pointer to receive the new recommended width per eye.
 * @param out_height Pointer to receive the new recommended height per eye.
 *
 * @return true if a resolution change was pending, false otherwise.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_get_pending_render_resolution(struct xrt_compositor *xc,
                                                     uint32_t *out_width,
                                                     uint32_t *out_height);

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

#ifdef __cplusplus
}
#endif
