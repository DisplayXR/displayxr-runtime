// Copyright 2024-2025, The DisplayXR Project
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
 * #1589: the FORMAT-HONEST shader resource view for a swapchain image — the
 * app's TRUE format, so an `_SRGB` source decodes to linear on sample and a
 * UNORM source reads the linear values it holds.
 *
 * This is what the compose path binds, because it writes through an `_SRGB`
 * render target that blends in linear and encodes once on write. Every other
 * path keeps @ref comp_d3d11_swapchain_get_srv, whose non-decoding view is
 * what "hand the app's bytes on unchanged" means. Falls back to that view
 * when the two would be identical, under the escape hatch, or on failure.
 *
 * @param xsc The swapchain.
 * @param index Image index.
 * @return The SRV as void pointer, or NULL if not available.
 *
 * @ingroup comp_d3d11
 */
void *
comp_d3d11_swapchain_get_compose_srv(struct xrt_swapchain *xsc, uint32_t index);

/*!
 * #1589: did the app request an `*_SRGB` colour swapchain?
 *
 * ADR-021 §6: the format IS the declaration. True ⟹ the bytes are
 * display-referred and may be handed to the ENCODED atlas unchanged; false ⟹
 * they are scene-linear and owe the encode.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_swapchain_is_srgb(struct xrt_swapchain *xsc);

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
 * Get the array (layer) size of a swapchain (XrSwapchainCreateInfo.arraySize).
 * Returns 1 for non-layered swapchains.
 *
 * @param xsc The swapchain.
 * @return The number of array layers (>= 1).
 *
 * @ingroup comp_d3d11
 */
uint32_t
comp_d3d11_swapchain_get_array_size(struct xrt_swapchain *xsc);

#ifdef __cplusplus
}
#endif
