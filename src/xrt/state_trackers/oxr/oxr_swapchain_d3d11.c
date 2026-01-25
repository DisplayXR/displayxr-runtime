// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds D3D11 swapchain related functions.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author David Fattal
 * @ingroup oxr_main
 * @ingroup comp_client
 */

#include <stdlib.h>

#include "xrt/xrt_gfx_d3d11.h"
#include "xrt/xrt_compositor.h"
#include "util/u_debug.h"
#include "util/u_logging.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_swapchain_common.h"


/*!
 * Enumerate images for D3D11 client compositor (wraps Vulkan compositor).
 *
 * This is used when a D3D11 client compositor wraps a Vulkan native compositor.
 * The client compositor creates xrt_swapchain_d3d11 with direct texture pointers.
 */
static XrResult
d3d11_enumerate_images(struct oxr_logger *log,
                       struct oxr_swapchain *sc,
                       uint32_t count,
                       XrSwapchainImageBaseHeader *images)
{
	struct xrt_swapchain_d3d11 *xscd3d = (struct xrt_swapchain_d3d11 *)sc->swapchain;
	XrSwapchainImageD3D11KHR *d3d_imgs = (XrSwapchainImageD3D11KHR *)images;

	for (uint32_t i = 0; i < count; i++) {
		d3d_imgs[i].texture = xscd3d->images[i];
	}

	return oxr_session_success_result(sc->sess);
}

/*!
 * Enumerate images for D3D11 native compositor (bypasses Vulkan).
 *
 * This is used when using the D3D11 native compositor directly.
 * The native compositor creates xrt_swapchain_native where the textures
 * are stored in images[i].handle as ID3D11Texture2D* pointers.
 */
static XrResult
d3d11_native_enumerate_images(struct oxr_logger *log,
                              struct oxr_swapchain *sc,
                              uint32_t count,
                              XrSwapchainImageBaseHeader *images)
{
	struct xrt_swapchain_native *xscn = (struct xrt_swapchain_native *)sc->swapchain;
	XrSwapchainImageD3D11KHR *d3d_imgs = (XrSwapchainImageD3D11KHR *)images;

	// Debug logging
	static int enum_count = 0;
	if (enum_count < 5) {
		U_LOG_W("[DEBUG] d3d11_native_enumerate_images: sc=%p, xscn=%p, count=%u",
		        (void *)sc, (void *)xscn, count);
	}

	for (uint32_t i = 0; i < count; i++) {
		// The D3D11 native compositor stores ID3D11Texture2D* in handle
		d3d_imgs[i].texture = (ID3D11Texture2D *)xscn->images[i].handle;

		if (enum_count < 5) {
			U_LOG_W("[DEBUG] d3d11_native_enumerate_images: image[%u].handle=%p, texture=%p",
			        i, (void *)xscn->images[i].handle, (void *)d3d_imgs[i].texture);
		}
	}

	if (enum_count < 5) {
		enum_count++;
	}

	return oxr_session_success_result(sc->sess);
}

/*!
 * Create D3D11 swapchain for use with D3D11 client compositor (Vulkan backend).
 */
XrResult
oxr_swapchain_d3d11_create(struct oxr_logger *log,
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

	// Set our API specific function(s).
	sc->enumerate_images = d3d11_enumerate_images;

	*out_swapchain = sc;

	return XR_SUCCESS;
}

/*!
 * Create D3D11 swapchain for use with D3D11 native compositor (no Vulkan).
 *
 * This is used when the D3D11 native compositor is enabled. The swapchains
 * created by the native compositor have a different structure than those
 * created by the D3D11 client compositor (which wraps Vulkan).
 */
XrResult
oxr_swapchain_d3d11_native_create(struct oxr_logger *log,
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
	sc->enumerate_images = d3d11_native_enumerate_images;

	*out_swapchain = sc;

	return XR_SUCCESS;
}
