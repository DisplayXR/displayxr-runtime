// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 native swapchain implementation.
 * @author David Fattal
 * @ingroup comp_d3d12
 */

#pragma once

#include "xrt/xrt_compositor.h"

// Forward declarations (C++ structs)
struct comp_d3d12_compositor;
struct comp_d3d12_swapchain;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a D3D12 native swapchain.
 *
 * Creates D3D12 committed resources that the application can render to directly.
 * No Vulkan interop is involved.
 *
 * @param c The D3D12 compositor.
 * @param info Swapchain creation info.
 * @param out_xsc Pointer to receive the created swapchain.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_swapchain_create(struct comp_d3d12_compositor *c,
                            const struct xrt_swapchain_create_info *info,
                            struct xrt_swapchain **out_xsc);

/*!
 * Get the dimensions of a swapchain.
 *
 * @param xsc The swapchain.
 * @param[out] out_w Width in pixels.
 * @param[out] out_h Height in pixels.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_swapchain_get_dimensions(struct xrt_swapchain *xsc, uint32_t *out_w, uint32_t *out_h);

/*!
 * Get the D3D12 resource for a swapchain image.
 *
 * @param xsc The swapchain.
 * @param index Image index.
 * @return The resource as void pointer, or NULL if not available.
 *
 * @ingroup comp_d3d12
 */
void *
comp_d3d12_swapchain_get_resource(struct xrt_swapchain *xsc, uint32_t index);

#ifdef __cplusplus
}
#endif
