// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 native swapchain implementation.
 * @author David Fattal
 * @ingroup comp_d3d12
 */

#pragma once

#include "xrt/xrt_compositor.h"

#include <dxgiformat.h>

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

/*!
 * The DXGI format a view over @p resource must use.
 *
 * Swapchain images are created TYPELESS (#1503), which is not a legal view
 * format, so every SRV the compositor builds over an application image has to
 * resolve back to a concrete member of the family. This returns the typed
 * format the app requested (stamped on the resource at create time), mapped
 * through the sRGB -> UNORM pass-through rule the display processor needs; for
 * a resource with no stamp — a runtime scratch, an engine-supplied shared
 * texture — it falls back to the resource's own descriptor.
 *
 * @param resource An `ID3D12Resource *`. NULL yields R8G8B8A8_UNORM.
 *
 * @ingroup comp_d3d12
 */
DXGI_FORMAT
comp_d3d12_swapchain_sample_format(void *resource);

/*!
 * #1589 — the FORMAT-HONEST view format for @p resource: the app's TRUE
 * format, so an `_SRGB` swapchain decodes to linear on sample and a UNORM
 * swapchain reads the linear values it holds (ADR-021 §6).
 *
 * The twin of @ref comp_d3d12_swapchain_sample_format, and the only difference
 * between them is the sRGB -> UNORM coercion: that one is right wherever the
 * runtime hands the app's bytes on UNCHANGED (the single-layer fast path,
 * zero-copy, the Local2D flatten), this one wherever the runtime writes
 * through an `_SRGB` render target that blends in linear and encodes once on
 * write. Same bytes, two readings — the swapchain format picks which is
 * correct, which is the whole of #1589.
 *
 * Still resolves a TYPELESS resource to something a view will accept, so it is
 * a safe drop-in at any SRV site.
 *
 * @param resource An `ID3D12Resource *`. NULL yields R8G8B8A8_UNORM.
 *
 * @ingroup comp_d3d12
 */
DXGI_FORMAT
comp_d3d12_swapchain_compose_format(void *resource);

/*!
 * #1589 — did the app request an `*_SRGB` colour swapchain for @p resource?
 *
 * ADR-021 §6: the format IS the declaration. True ⟹ the bytes are
 * display-referred and may reach the ENCODED atlas unchanged; false ⟹ they are
 * scene-linear and owe the encode, which only the compose target can apply.
 *
 * @param resource An `ID3D12Resource *`. NULL yields false.
 *
 * @ingroup comp_d3d12
 */
bool
comp_d3d12_swapchain_resource_is_srgb(void *resource);

#ifdef __cplusplus
}
#endif
