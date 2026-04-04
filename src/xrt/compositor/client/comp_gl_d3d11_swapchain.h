// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL client swapchain using WGL_NV_DX_interop2 for D3D11 shared textures.
 * @ingroup comp_client
 */

#pragma once

#include "comp_gl_client.h"

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * @class client_gl_d3d11_swapchain
 *
 * Wraps D3D11 shared textures as GL textures via WGL_NV_DX_interop2.
 * Used when the IPC service exports D3D11 NT handles (shell/IPC mode).
 *
 * @ingroup comp_client
 * @implements xrt_swapchain_gl
 */
struct client_gl_d3d11_swapchain
{
	struct client_gl_swapchain base;

	//! D3D11 device created for interop
	void *dx_device;   // ID3D11Device*
	void *dx_device1;  // ID3D11Device1* (for OpenSharedResource1)
	void *dx_context;  // ID3D11DeviceContext*

	//! WGL interop device handle from wglDXOpenDeviceNV
	void *dx_interop_device; // HANDLE

	//! Per-image D3D11 textures imported from shared handles (has KeyedMutex)
	void *dx_textures[XRT_MAX_SWAPCHAIN_IMAGES]; // ID3D11Texture2D*

	//! Per-image staging textures for WGL interop (no KeyedMutex)
	void *dx_staging_textures[XRT_MAX_SWAPCHAIN_IMAGES]; // ID3D11Texture2D*

	//! Per-image KeyedMutex interfaces for shared textures
	void *dx_keyed_mutexes[XRT_MAX_SWAPCHAIN_IMAGES]; // IDXGIKeyedMutex*

	//! Per-image WGL interop object handles from wglDXRegisterObjectNV
	void *dx_interop_objects[XRT_MAX_SWAPCHAIN_IMAGES]; // HANDLE

	//! Number of images
	uint32_t image_count;
};

/*!
 * Check if WGL_NV_DX_interop2 is available for D3D11 shared texture import.
 */
bool
client_gl_d3d11_interop_available(void);

/*!
 * Create a GL swapchain that imports D3D11 shared textures via WGL_NV_DX_interop2.
 *
 * @see client_gl_swapchain_create_func_t, client_gl_compositor_init
 */
struct xrt_swapchain *
client_gl_d3d11_swapchain_create(struct xrt_compositor *xc,
                                 const struct xrt_swapchain_create_info *info,
                                 struct xrt_swapchain_native *xscn,
                                 struct client_gl_swapchain **out_cglsc);


#ifdef __cplusplus
}
#endif
