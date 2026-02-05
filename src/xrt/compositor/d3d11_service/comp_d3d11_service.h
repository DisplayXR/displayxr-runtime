// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 service compositor interface.
 * @author David Fattal
 * @ingroup comp_d3d11_service
 *
 * This compositor is designed for IPC/service mode where the compositor
 * runs in a separate process from the OpenXR application. It:
 * - Creates its own D3D11 device (not using the app's device)
 * - Imports swapchain images from clients via DXGI shared handles
 * - Uses KeyedMutex for cross-process synchronization
 * - Integrates with Leia SR for light field display output
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * @defgroup comp_d3d11_service D3D11 Service Compositor
 * @ingroup xrt
 *
 * D3D11-based compositor for Windows service mode (IPC).
 * Avoids Vulkan-D3D11 interop issues on Intel iGPUs by
 * using pure D3D11 throughout.
 */

/*!
 * Create the D3D11 service system compositor.
 *
 * This creates a compositor that runs in service mode with its own
 * D3D11 device, capable of importing swapchains from client processes
 * via DXGI shared handles.
 *
 * @param xdev The head device to render for.
 * @param xsysd System devices for qwerty input support (may be NULL).
 * @param[out] out_xsysc Pointer to receive the system compositor.
 * @return XRT_SUCCESS on success.
 *
 * @ingroup comp_d3d11_service
 */
xrt_result_t
comp_d3d11_service_create_system(struct xrt_device *xdev,
                                 struct xrt_system_devices *xsysd,
                                 struct xrt_system_compositor **out_xsysc);


#ifdef __cplusplus
}
#endif
