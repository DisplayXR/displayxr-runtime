// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds D3D12 native compositor specific session functions.
 *
 * This file provides session integration for the D3D12 native compositor
 * that bypasses Vulkan entirely. This is used when XR_EXT_win32_window_binding
 * provides a window handle and the system supports D3D12 native composition.
 *
 * @author David Fattal
 * @ingroup oxr_main
 * @ingroup comp_d3d12
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

#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
#include "d3d12/comp_d3d12_compositor.h"
#endif

/*
 * Environment variable to enable/disable D3D12 native compositor.
 * Default is TRUE - D3D12 native compositor is enabled by default for in-process mode.
 * Set OXR_ENABLE_D3D12_NATIVE_COMPOSITOR=0 to force Vulkan compositor (for debugging).
 */
DEBUG_GET_ONCE_BOOL_OPTION(enable_d3d12_native_compositor, "OXR_ENABLE_D3D12_NATIVE_COMPOSITOR", true)

bool
oxr_d3d12_native_compositor_supported(struct oxr_system *sys, void *window_handle)
{
#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR
	(void)window_handle;

	bool is_service_mode = sys->xsysc != NULL && sys->xsysc->info.is_service_mode;

	bool env_enabled = debug_get_bool_option_enable_d3d12_native_compositor();
	U_LOG_IFL_I(U_LOGGING_INFO, "D3D12 native compositor check: XRT_HAVE_D3D12_NATIVE_COMPOSITOR=defined, "
	        "OXR_ENABLE_D3D12_NATIVE_COMPOSITOR=%s, window_handle=%p, is_service_mode=%s",
	        env_enabled ? "1 (enabled)" : "0 (disabled)", window_handle,
	        is_service_mode ? "true" : "false");

	if (is_service_mode) {
		U_LOG_IFL_I(U_LOGGING_INFO,
		    "D3D12 native compositor DISABLED - running in service mode (IPC)");
		return false;
	}

	if (!env_enabled) {
		U_LOG_IFL_I(U_LOGGING_INFO, "D3D12 native compositor DISABLED - falling back to Vulkan compositor");
		return false;
	}

	if (window_handle != NULL) {
		U_LOG_IFL_I(U_LOGGING_INFO, "D3D12 native compositor ENABLED with app-provided window");
	} else {
		U_LOG_IFL_I(U_LOGGING_INFO, "D3D12 native compositor ENABLED, will create own window");
	}
	return true;
#else
	U_LOG_IFL_I(U_LOGGING_INFO,
	    "D3D12 native compositor check: XRT_HAVE_D3D12_NATIVE_COMPOSITOR=NOT defined (not compiled in)");
	(void)sys;
	(void)window_handle;
	return false;
#endif
}

#ifdef XRT_HAVE_D3D12_NATIVE_COMPOSITOR

// Forward declaration - defined in oxr_swapchain_d3d12_native.c
extern XrResult
oxr_swapchain_d3d12_native_create(struct oxr_logger *log,
                                  struct oxr_session *sess,
                                  const XrSwapchainCreateInfo *createInfo,
                                  struct oxr_swapchain **out_swapchain);

XrResult
oxr_session_populate_d3d12_native(struct oxr_logger *log,
                                   struct oxr_system *sys,
                                   XrGraphicsBindingD3D12KHR const *next,
                                   void *window_handle,
                                   void *shared_texture_handle,
                                   struct oxr_session *sess)
{
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	// Get D3D12 display processor factory from system compositor info
	void *dp_factory_d3d12 = NULL;
	if (sys->xsysc != NULL) {
		dp_factory_d3d12 = sys->xsysc->info.dp_factory_d3d12;
	}

	// Create the D3D12 native compositor
	xrt_result_t xret = comp_d3d12_compositor_create(
	    xdev, window_handle, shared_texture_handle,
	    (void *)next->device, (void *)next->queue,
	    dp_factory_d3d12, &xcn);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create D3D12 native compositor: %d", xret);
	}

	// Set system devices for qwerty driver support
	comp_d3d12_compositor_set_system_devices(&xcn->base, sess->sys->xsysd);

	// Pass legacy app tile scaling flag so compositor can disable 1/2/3 mode keys
	if (sess->sys->xsysc != NULL) {
		comp_d3d12_compositor_set_legacy_app_tile_scaling(
		    &xcn->base, sess->sys->xsysc->info.legacy_app_tile_scaling);
	}

	// Set the compositor directly - no client wrapper needed
	sess->xcn = xcn;
	sess->compositor = &xcn->base;
	sess->create_swapchain = oxr_swapchain_d3d12_native_create;
	sess->is_d3d12_native_compositor = true;

	// Native compositor doesn't use multi-compositor event system
	sess->compositor_visible = true;
	sess->compositor_focused = true;

	// Set ext_app_mode for shared texture / window binding apps
	sess->has_external_window =
	    (window_handle != NULL || shared_texture_handle != NULL);
	if (sess->has_external_window) {
		struct xrt_device *head = get_role_head(sess->sys);
		if (head != NULL) {
			xrt_device_set_property(head, XRT_DEVICE_PROPERTY_EXT_APP_MODE, 1);
		}
	}

	U_LOG_IFL_I(U_LOGGING_INFO, "Using D3D12 native compositor (bypassing Vulkan)%s",
	            shared_texture_handle ? " — shared texture mode" : "");

	return XR_SUCCESS;
}

#endif /* XRT_HAVE_D3D12_NATIVE_COMPOSITOR */
