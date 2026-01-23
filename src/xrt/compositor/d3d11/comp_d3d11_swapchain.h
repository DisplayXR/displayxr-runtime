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

#ifdef __cplusplus
}
#endif
