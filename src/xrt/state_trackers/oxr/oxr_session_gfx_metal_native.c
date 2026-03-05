// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds Metal native compositor specific session functions.
 *
 * This file provides session integration for the Metal native compositor
 * that bypasses Vulkan entirely on macOS. Mirrors the D3D11 native
 * compositor pattern in oxr_session_gfx_d3d11_native.c.
 *
 * @author David Fattal
 * @ingroup oxr_main
 * @ingroup comp_metal
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

#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
#include "metal/comp_metal_compositor.h"
#endif

/*
 * Environment variable to enable/disable Metal native compositor.
 * Default is TRUE - Metal native compositor is enabled by default on macOS.
 * Set OXR_ENABLE_METAL_NATIVE_COMPOSITOR=0 to force Vulkan compositor (for debugging).
 */
DEBUG_GET_ONCE_BOOL_OPTION(enable_metal_native_compositor, "OXR_ENABLE_METAL_NATIVE_COMPOSITOR", true)

/*!
 * Check if Metal native compositor should be used.
 */
bool
oxr_metal_native_compositor_supported(struct oxr_system *sys, void *window_handle)
{
#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR
	(void)window_handle;

	// Check if we're running in service/IPC mode
	bool is_service_mode = sys->xsysc != NULL && sys->xsysc->info.is_service_mode;

	bool env_enabled = debug_get_bool_option_enable_metal_native_compositor();
	U_LOG_IFL_I(U_LOGGING_INFO,
	            "Metal native compositor check: XRT_HAVE_METAL_NATIVE_COMPOSITOR=defined, "
	            "OXR_ENABLE_METAL_NATIVE_COMPOSITOR=%s, window_handle=%p, is_service_mode=%s",
	            env_enabled ? "1 (enabled)" : "0 (disabled)", window_handle,
	            is_service_mode ? "true" : "false");

	if (is_service_mode) {
		U_LOG_IFL_I(U_LOGGING_INFO,
		            "Metal native compositor DISABLED - running in service mode (IPC)");
		return false;
	}

	if (!env_enabled) {
		U_LOG_IFL_I(U_LOGGING_INFO,
		            "Metal native compositor DISABLED - falling back to Vulkan compositor");
		return false;
	}

	if (window_handle != NULL) {
		U_LOG_IFL_I(U_LOGGING_INFO, "Metal native compositor ENABLED with app-provided window");
	} else {
		U_LOG_IFL_I(U_LOGGING_INFO, "Metal native compositor ENABLED (no window provided — "
		            "offscreen mode will be determined by caller)");
	}
	return true;
#else
	U_LOG_IFL_I(U_LOGGING_INFO,
	            "Metal native compositor check: XRT_HAVE_METAL_NATIVE_COMPOSITOR=NOT defined");
	(void)sys;
	(void)window_handle;
	return false;
#endif
}

#ifdef XRT_HAVE_METAL_NATIVE_COMPOSITOR

// Forward declaration - defined in oxr_swapchain_metal.c
extern XrResult
oxr_swapchain_metal_native_create(struct oxr_logger *log,
                                  struct oxr_session *sess,
                                  const XrSwapchainCreateInfo *createInfo,
                                  struct oxr_swapchain **out_swapchain);

XrResult
oxr_session_populate_metal_native(struct oxr_logger *log,
                                  struct oxr_system *sys,
                                  XrGraphicsBindingMetalKHR const *next,
                                  void *window_handle,
                                  bool offscreen,
                                  void *shared_texture_handle,
                                  struct oxr_session *sess)
{
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	// Get Metal display processor factory from system compositor info
	void *dp_factory_metal = NULL;
	if (sys->xsysc != NULL) {
		dp_factory_metal = sys->xsysc->info.dp_factory_metal;
	}

	// Create the Metal native compositor
	xrt_result_t xret = comp_metal_compositor_create(
	    xdev, window_handle, (void *)next->commandQueue, dp_factory_metal, offscreen,
	    shared_texture_handle, &xcn);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create Metal native compositor: %d", xret);
	}

	// Set system devices for qwerty driver support
	comp_metal_compositor_set_system_devices(&xcn->base, sess->sys->xsysd);

	// Set system compositor info for HUD (display dimensions, nominal viewer)
	if (sys->xsysc != NULL) {
		comp_metal_compositor_set_sys_info(&xcn->base, &sys->xsysc->info);
	}

	// Set the compositor directly - no client wrapper needed
	sess->xcn = xcn;
	sess->compositor = &xcn->base;
	sess->create_swapchain = oxr_swapchain_metal_native_create;
	sess->is_metal_native_compositor = true;

	// Metal native compositor doesn't use the multi-compositor event system
	sess->compositor_visible = true;
	sess->compositor_focused = true;

	U_LOG_IFL_I(U_LOGGING_INFO, "Using Metal native compositor (bypassing Vulkan)");

	return XR_SUCCESS;
}

#endif /* XRT_HAVE_METAL_NATIVE_COMPOSITOR */
