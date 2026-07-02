// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 native swapchain enumerate for native compositor.
 *
 * When using the D3D12 native compositor, swapchains are xrt_swapchain_native
 * with ID3D12Resource* stored in images[i].handle.
 *
 * @author David Fattal
 * @ingroup oxr_main
 */

#include <stdlib.h>

#include "xrt/xrt_gfx_d3d12.h"
#include "xrt/xrt_compositor.h"
#include "util/u_debug.h"
#include "util/u_logging.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_swapchain_common.h"


/*!
 * Enumerate images for D3D12 native compositor (bypasses Vulkan).
 *
 * The native compositor creates xrt_swapchain_native where the resources
 * are stored in images[i].handle as ID3D12Resource* pointers.
 */
static XrResult
d3d12_native_enumerate_images(struct oxr_logger *log,
                              struct oxr_swapchain *sc,
                              uint32_t count,
                              XrSwapchainImageBaseHeader *images)
{
	struct xrt_swapchain_native *xscn = (struct xrt_swapchain_native *)sc->swapchain;
	XrSwapchainImageD3D12KHR *d3d_imgs = (XrSwapchainImageD3D12KHR *)images;

	for (uint32_t i = 0; i < count; i++) {
		// The D3D12 native compositor stores ID3D12Resource* in handle
		d3d_imgs[i].texture = (ID3D12Resource *)xscn->images[i].handle;
	}

	return oxr_session_success_result(sc->sess);
}

XrResult
oxr_swapchain_d3d12_native_create(struct oxr_logger *log,
                                  struct oxr_session *sess,
                                  const XrSwapchainCreateInfo *createInfo,
                                  struct oxr_swapchain **out_swapchain)
{
	struct oxr_swapchain *sc;
	XrResult ret;

	ret = oxr_swapchain_common_create(log, sess, createInfo, &sc);
	if (ret != XR_SUCCESS) {
		return ret;
	}

	// Set our API specific function for native compositor.
	sc->enumerate_images = d3d12_native_enumerate_images;

	*out_swapchain = sc;

	return XR_SUCCESS;
}
