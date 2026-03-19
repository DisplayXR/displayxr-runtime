// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 DXGI swapchain target for display output.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations (C++ structs)
struct comp_d3d11_target;
struct comp_d3d11_compositor;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a D3D11 output target (DXGI swapchain).
 *
 * @param c The D3D11 compositor.
 * @param hwnd The window handle to present to.
 * @param width Preferred width.
 * @param height Preferred height.
 * @param out_target Pointer to receive the created target.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_target_create(struct comp_d3d11_compositor *c,
                         void *hwnd,
                         uint32_t width,
                         uint32_t height,
                         struct comp_d3d11_target **out_target);

/*!
 * Destroy a D3D11 output target.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_target_destroy(struct comp_d3d11_target **target_ptr);

/*!
 * Acquire the next image for rendering.
 *
 * @param target The target.
 * @param out_index Index of the acquired image.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_target_acquire(struct comp_d3d11_target *target, uint32_t *out_index);

/*!
 * Present the rendered image.
 *
 * @param target The target.
 * @param sync_interval VSync interval (1 for VSync, 0 for immediate).
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_target_present(struct comp_d3d11_target *target, uint32_t sync_interval);

/*!
 * Get target dimensions.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_target_get_dimensions(struct comp_d3d11_target *target,
                                 uint32_t *out_width,
                                 uint32_t *out_height);

/*!
 * Re-bind the target's render target view and viewport (without clearing).
 *
 * Call this after operations that change the bound RTV (e.g. renderer draw)
 * and before the display processor writes to the target.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_target_bind(struct comp_d3d11_target *target);

/*!
 * Get the back buffer texture (ID3D11Texture2D*) for direct pixel copy.
 *
 * @ingroup comp_d3d11
 */
void *
comp_d3d11_target_get_back_buffer(struct comp_d3d11_target *target);

/*!
 * Resize the target swapchain.
 *
 * @param target The target.
 * @param width New width.
 * @param height New height.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_target_resize(struct comp_d3d11_target *target,
                         uint32_t width,
                         uint32_t height);

#ifdef __cplusplus
}
#endif
