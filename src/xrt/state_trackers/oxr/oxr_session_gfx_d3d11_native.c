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
#include "util/u_debug.h"

#include "xrt/xrt_instance.h"
#include "xrt/xrt_config_have.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_handle.h"

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
#include "d3d11/comp_d3d11_compositor.h"
#endif

/*
 * Environment variable to enable/disable D3D11 native compositor.
 * Default is TRUE - D3D11 native compositor is enabled by default for in-process mode.
 * Set OXR_ENABLE_D3D11_NATIVE_COMPOSITOR=0 to force Vulkan compositor (for debugging).
 */
DEBUG_GET_ONCE_BOOL_OPTION(enable_d3d11_native_compositor, "OXR_ENABLE_D3D11_NATIVE_COMPOSITOR", true)

/*!
 * Check if D3D11 native compositor should be used.
 *
 * The D3D11 native compositor is preferred when:
 * - The D3D11 native compositor is built
 * - We're using a D3D11 graphics binding
 * - We're NOT running in IPC/service mode (compositor must be in same process)
 * - The OXR_ENABLE_D3D11_NATIVE_COMPOSITOR environment variable is set
 *
 * Window handle is OPTIONAL:
 * - If provided via XR_EXT_session_target, compositor uses app's window
 * - If NULL, compositor creates its own window (for apps like Blender)
 *
 * This bypasses Vulkan entirely and solves interop issues on Intel GPUs.
 *
 * NOTE: The D3D11 native compositor only works in in-process mode because it
 * needs direct access to the app's D3D11 device. In IPC mode (when
 * monado-service is running), the Vulkan compositor in the server process
 * handles compositing.
 *
 * Default is ENABLED for in-process mode (Leia displays).
 * Set OXR_ENABLE_D3D11_NATIVE_COMPOSITOR=0 to force Vulkan compositor.
 */
bool
oxr_d3d11_native_compositor_supported(struct oxr_system *sys, void *window_handle)
{
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	// Window handle is now OPTIONAL - compositor will create its own window if NULL
	(void)window_handle;

	// Always log the env var check result at INFO level for debugging
	bool env_enabled = debug_get_bool_option_enable_d3d11_native_compositor();
	U_LOG_IFL_I(U_LOGGING_INFO,"D3D11 native compositor check: XRT_HAVE_D3D11_NATIVE_COMPOSITOR=defined, "
	        "OXR_ENABLE_D3D11_NATIVE_COMPOSITOR=%s, window_handle=%p",
	        env_enabled ? "1 (enabled)" : "0 (disabled)", window_handle);

	// D3D11 native compositor is disabled by default because it only works
	// in in-process mode. When using IPC/service mode (monado-service),
	// the Vulkan compositor in the server process handles compositing.
	// Enable explicitly for in-process testing.
	if (!env_enabled) {
		U_LOG_IFL_I(U_LOGGING_INFO,"D3D11 native compositor DISABLED - falling back to Vulkan compositor");
		return false;
	}

	if (window_handle != NULL) {
		U_LOG_IFL_I(U_LOGGING_INFO,"D3D11 native compositor ENABLED with app-provided window");
	} else {
		U_LOG_IFL_I(U_LOGGING_INFO,"D3D11 native compositor ENABLED, will create own window");
	}
	return true;
#else
	U_LOG_IFL_I(U_LOGGING_INFO,"D3D11 native compositor check: XRT_HAVE_D3D11_NATIVE_COMPOSITOR=NOT defined (not compiled in)");
	(void)sys;
	(void)window_handle;
	return false;
#endif
}

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR

// Forward declaration - defined in oxr_swapchain_d3d11.c
// Use the native version that correctly handles xrt_swapchain_native structure
extern XrResult
oxr_swapchain_d3d11_native_create(struct oxr_logger *log,
                                  struct oxr_session *sess,
                                  const XrSwapchainCreateInfo *createInfo,
                                  struct oxr_swapchain **out_swapchain);

XrResult
oxr_session_populate_d3d11_native(struct oxr_logger *log,
                                   struct oxr_system *sys,
                                   XrGraphicsBindingD3D11KHR const *next,
                                   void *window_handle,
                                   struct oxr_session *sess)
{
	struct xrt_device *xdev = get_role_head(sess->sys);
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
	// Use native swapchain create that handles xrt_swapchain_native layout
	sess->create_swapchain = oxr_swapchain_d3d11_native_create;

	U_LOG_IFL_I(U_LOGGING_INFO,"Using D3D11 native compositor (bypassing Vulkan)");

	return XR_SUCCESS;
}

#endif /* XRT_HAVE_D3D11_NATIVE_COMPOSITOR */
