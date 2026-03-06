// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds Metal swapchain related functions.
 *
 * Mirrors the D3D11 swapchain pattern in oxr_swapchain_d3d11.c for
 * Metal native compositor.
 *
 * @author David Fattal
 * @ingroup oxr_main
 * @ingroup comp_metal
 */

#include <stdlib.h>

#include "xrt/xrt_compositor.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_swapchain_common.h"

#include "metal/comp_metal_compositor.h"

/*!
 * Enumerate images for Metal native compositor.
 *
 * The native handle is an IOSurfaceRef (for cross-API sharing with Vulkan).
 * Use comp_metal_swapchain_get_texture() to get the actual id<MTLTexture>.
 */
static XrResult
metal_native_enumerate_images(struct oxr_logger *log,
                              struct oxr_swapchain *sc,
                              uint32_t count,
                              XrSwapchainImageBaseHeader *images)
{
	XrSwapchainImageMetalKHR *metal_imgs = (XrSwapchainImageMetalKHR *)images;

	for (uint32_t i = 0; i < count; i++) {
		metal_imgs[i].texture = comp_metal_swapchain_get_texture(sc->swapchain, i);
	}

	return oxr_session_success_result(sc->sess);
}

/*!
 * Create Metal swapchain for use with Metal native compositor.
 */
XrResult
oxr_swapchain_metal_native_create(struct oxr_logger *log,
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

	sc->enumerate_images = metal_native_enumerate_images;

	*out_swapchain = sc;

	return XR_SUCCESS;
}
