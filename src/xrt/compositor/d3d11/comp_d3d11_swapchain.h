// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 native swapchain implementation.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#pragma once

#include "xrt/xrt_compositor.h"

// Forward declarations (C++ structs)
struct comp_d3d11_compositor;
struct comp_d3d11_swapchain;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a D3D11 native swapchain.
 *
 * Creates D3D11 textures that the application can render to directly.
 * No Vulkan interop is involved.
 *
 * @param c The D3D11 compositor.
 * @param info Swapchain creation info.
 * @param out_xsc Pointer to receive the created swapchain.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_swapchain_create(struct comp_d3d11_compositor *c,
                            const struct xrt_swapchain_create_info *info,
                            struct xrt_swapchain **out_xsc);

/*!
 * Get the shader resource view for a swapchain image.
 *
 * @param xsc The swapchain.
 * @param index Image index.
 * @return The SRV as void pointer, or NULL if not available.
 *
 * @ingroup comp_d3d11
 */
void *
comp_d3d11_swapchain_get_srv(struct xrt_swapchain *xsc, uint32_t index);

/*!
 * Get the render target view for a swapchain image.
 *
 * @param xsc The swapchain.
 * @param index Image index.
 * @return The RTV as void pointer, or NULL if not available.
 *
 * @ingroup comp_d3d11
 */
void *
comp_d3d11_swapchain_get_rtv(struct xrt_swapchain *xsc, uint32_t index);

/*!
 * Get the D3D11 texture for a swapchain image.
 *
 * @param xsc The swapchain.
 * @param index Image index.
 * @return The texture as void pointer, or NULL if not available.
 *
 * @ingroup comp_d3d11
 */
void *
comp_d3d11_swapchain_get_texture(struct xrt_swapchain *xsc, uint32_t index);

/*!
 * Get the dimensions of a swapchain.
 *
 * @param xsc The swapchain.
 * @param[out] out_w Width in pixels.
 * @param[out] out_h Height in pixels.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_swapchain_get_dimensions(struct xrt_swapchain *xsc, uint32_t *out_w, uint32_t *out_h);

/*!
 * Wait for GPU completion of all commands submitted up to the most recent
 * xrReleaseSwapchainImage for this swapchain.
 *
 * Uses ID3D11Fence + WaitForSingleObject on D3D11.4 (Windows 10+),
 * falls back to Flush + D3D11_QUERY_EVENT spin-wait on older hardware.
 *
 * @param xsc        The swapchain.
 * @param timeout_ms Maximum wait in milliseconds (100 is recommended).
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_swapchain_wait_gpu_complete(struct xrt_swapchain *xsc, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
