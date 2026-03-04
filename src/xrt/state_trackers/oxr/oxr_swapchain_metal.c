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

/*!
 * Enumerate images for Metal native compositor.
 *
 * The native compositor creates xrt_swapchain_native where the Metal
 * textures (id<MTLTexture>) are stored in images[i].handle.
 */
static XrResult
metal_native_enumerate_images(struct oxr_logger *log,
                              struct oxr_swapchain *sc,
                              uint32_t count,
                              XrSwapchainImageBaseHeader *images)
{
	struct xrt_swapchain_native *xscn = (struct xrt_swapchain_native *)sc->swapchain;
	XrSwapchainImageMetalKHR *metal_imgs = (XrSwapchainImageMetalKHR *)images;

	for (uint32_t i = 0; i < count; i++) {
		// The Metal native compositor stores id<MTLTexture> in handle
		metal_imgs[i].texture = (void *)(uintptr_t)xscn->images[i].handle;
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
