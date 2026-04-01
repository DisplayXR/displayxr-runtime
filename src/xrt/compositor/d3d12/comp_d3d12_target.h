// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 DXGI swapchain target for display output.
 * @author David Fattal
 * @ingroup comp_d3d12
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations (C++ structs)
struct comp_d3d12_target;
struct comp_d3d12_compositor;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a D3D12 output target (DXGI swapchain).
 *
 * @param c The D3D12 compositor.
 * @param hwnd The window handle to present to.
 * @param width Preferred width.
 * @param height Preferred height.
 * @param out_target Pointer to receive the created target.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_target_create(struct comp_d3d12_compositor *c,
                         void *hwnd,
                         uint32_t width,
                         uint32_t height,
                         struct comp_d3d12_target **out_target);

/*!
 * Destroy a D3D12 output target.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_target_destroy(struct comp_d3d12_target **target_ptr);

/*!
 * Present the rendered image.
 *
 * @param target The target.
 * @param sync_interval VSync interval (1 for VSync, 0 for immediate).
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_target_present(struct comp_d3d12_target *target, uint32_t sync_interval);

/*!
 * Get target dimensions.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_target_get_dimensions(struct comp_d3d12_target *target,
                                 uint32_t *out_width,
                                 uint32_t *out_height);

/*!
 * Get the current back buffer index.
 *
 * @ingroup comp_d3d12
 */
uint32_t
comp_d3d12_target_get_current_index(struct comp_d3d12_target *target);

/*!
 * Get the back buffer resource at the given index.
 *
 * @ingroup comp_d3d12
 */
void *
comp_d3d12_target_get_back_buffer(struct comp_d3d12_target *target, uint32_t index);

/*!
 * Get the RTV CPU descriptor handle for the given back buffer index.
 *
 * @ingroup comp_d3d12
 */
uint64_t
comp_d3d12_target_get_rtv_handle(struct comp_d3d12_target *target, uint32_t index);

/*!
 * Resize the target swapchain.
 *
 * @param target The target.
 * @param width New width.
 * @param height New height.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_target_resize(struct comp_d3d12_target *target,
                         uint32_t width,
                         uint32_t height);

/*!
 * Check whether the target created a child window fallback.
 *
 * Returns true if the target had to create a WS_CHILD window because the
 * app's HWND already had a DXGI swapchain (E_ACCESSDENIED fallback).
 *
 * @ingroup comp_d3d12
 */
bool
comp_d3d12_target_has_child_window(struct comp_d3d12_target *target);

/*!
 * Resize the child window to match the parent's client area.
 *
 * No-op if the target does not have a child window fallback.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_target_resize_child_window(struct comp_d3d12_target *target,
                                      uint32_t width,
                                      uint32_t height);

#ifdef __cplusplus
}
#endif
