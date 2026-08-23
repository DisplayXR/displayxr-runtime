// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for null compositor interfaces.
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_null
 */


#pragma once

#include "xrt/xrt_results.h"
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


struct xrt_device;
struct xrt_system_compositor;

/*!
 * Creates a @ref null_compositor.
 *
 * @ingroup comp_null
 */
xrt_result_t
null_compositor_create_system(struct xrt_device *xdev, struct xrt_system_compositor **out_xsysc);

/*!
 * Creates a @ref null_compositor with specified recommended view dimensions and refresh rate.
 *
 * This variant allows passing custom recommended dimensions and refresh rate
 * (e.g., from SR display) instead of using the default hardcoded values.
 *
 * @param xdev The device to create the compositor for.
 * @param recommended_width Recommended view width per eye (0 to use default).
 * @param recommended_height Recommended view height per eye (0 to use default).
 * @param refresh_rate_hz Display refresh rate in Hz (0 to use default 20 Hz).
 * @param scanout_adapter_luid Packed LUID of the adapter that scans out the 3D
 *        panel, or 0 if unknown/not needed. Only the instance layer can resolve
 *        this (it owns the panel rect); it is forwarded to the Vulkan bundle,
 *        where `DXR_VK_FORCE_GPU=scanout` consumes it. @see #918
 * @param render_adapter_luid Packed LUID of the adapter the runtime's ADR-037
 *        §2 capability ranking chose to render on, or 0 if unknown. Same
 *        reason it arrives from here: aux_vk can see neither DXGI nor the
 *        plug-in. Forwarded to the Vulkan bundle, where it selects the
 *        physical device. @see #918
 * @param out_xsysc Pointer to receive the created system compositor.
 *
 * @ingroup comp_null
 */
xrt_result_t
null_compositor_create_system_with_dims(struct xrt_device *xdev,
                                        uint32_t recommended_width,
                                        uint32_t recommended_height,
                                        float refresh_rate_hz,
                                        uint64_t scanout_adapter_luid,
                                        uint64_t render_adapter_luid,
                                        struct xrt_system_compositor **out_xsysc);


#ifdef __cplusplus
}
#endif
