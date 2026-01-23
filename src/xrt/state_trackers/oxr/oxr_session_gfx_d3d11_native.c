// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds D3D11 native compositor specific session functions.
 *
 * This file provides session integration for the D3D11 native compositor
 * that bypasses Vulkan entirely. This is used when XR_EXT_session_target
 * provides a window handle and the system supports D3D11 native composition.
 *
 * @author David Fattal
 * @ingroup oxr_main
 * @ingroup comp_d3d11
 */

#include <stdlib.h>

#include "util/u_misc.h"
#include "util/u_logging.h"

#include "xrt/xrt_instance.h"
#include "xrt/xrt_config_have.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_handle.h"

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
#include "d3d11/comp_d3d11_compositor.h"
#endif

/*!
 * Check if D3D11 native compositor should be used.
 *
 * The D3D11 native compositor is preferred when:
 * - We have a window handle from XR_EXT_session_target
 * - The D3D11 native compositor is built
 * - We're using a D3D11 graphics binding
 *
 * This bypasses Vulkan entirely and solves interop issues on Intel GPUs.
 */
bool
oxr_d3d11_native_compositor_supported(struct oxr_system *sys, void *window_handle)
{
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	// We need a window handle to use the D3D11 native compositor
	if (window_handle == NULL) {
		return false;
	}

	// D3D11 native compositor is available
	return true;
#else
	(void)sys;
	(void)window_handle;
	return false;
#endif
}

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR

/*!
 * Swapchain create function for D3D11 native compositor.
 */
static XrResult
oxr_swapchain_d3d11_native_create(struct oxr_logger *log,
                                   struct oxr_session *sess,
                                   const XrSwapchainCreateInfo *createInfo,
                                   struct oxr_swapchain **out_swapchain)
{
	// Use the standard swapchain creation path
	// The D3D11 native compositor handles swapchain creation internally
	return oxr_create_swapchain(log, sess, createInfo, out_swapchain);
}

XrResult
oxr_session_populate_d3d11_native(struct oxr_logger *log,
                                   struct oxr_system *sys,
                                   XrGraphicsBindingD3D11KHR const *next,
                                   void *window_handle,
                                   struct oxr_session *sess)
{
	struct xrt_device *xdev = sess->sys->xsysd->xdevs[sess->sys->xsysd->xdev_roles.head];
	struct xrt_compositor_native *xcn = NULL;

	// Create the D3D11 native compositor
	xrt_result_t xret = comp_d3d11_compositor_create(xdev, window_handle, (void *)next->device, &xcn);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create D3D11 native compositor: %d", xret);
	}

	// Set the compositor directly - no client wrapper needed
	sess->xcn = xcn;
	sess->compositor = &xcn->base;
	sess->create_swapchain = oxr_swapchain_d3d11_native_create;

	U_LOG_I("Using D3D11 native compositor (bypassing Vulkan)");

	return XR_SUCCESS;
}

#endif /* XRT_HAVE_D3D11_NATIVE_COMPOSITOR */
