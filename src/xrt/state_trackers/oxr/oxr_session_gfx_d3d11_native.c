// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds D3D11 native compositor specific session functions.
 *
 * This file provides session integration for the D3D11 native compositor
 * that bypasses Vulkan entirely. This is used when XR_EXT_win32_window_binding
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
 * - If provided via XR_EXT_win32_window_binding, compositor uses app's window
 * - If NULL, compositor creates its own window (for apps like Blender)
 *
 * This bypasses Vulkan entirely and solves interop issues on Intel GPUs.
 *
 * NOTE: The D3D11 native compositor only works in in-process mode because it
 * needs direct access to the app's D3D11 device. In IPC mode (when
 * displayxr-service is running), the Vulkan compositor in the server process
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

	// Check if we're running in service/IPC mode
	// D3D11 native compositor requires direct access to the app's D3D11 device,
	// which is not possible when the compositor is in a separate process
	bool is_service_mode = sys->xsysc != NULL && sys->xsysc->info.is_service_mode;

	// Always log the env var check result at INFO level for debugging
	bool env_enabled = debug_get_bool_option_enable_d3d11_native_compositor();
	U_LOG_IFL_I(U_LOGGING_INFO,"D3D11 native compositor check: XRT_HAVE_D3D11_NATIVE_COMPOSITOR=defined, "
	        "OXR_ENABLE_D3D11_NATIVE_COMPOSITOR=%s, window_handle=%p, is_service_mode=%s",
	        env_enabled ? "1 (enabled)" : "0 (disabled)", window_handle,
	        is_service_mode ? "true" : "false");

	// D3D11 native compositor cannot work in service/IPC mode
	// because it needs direct access to the app's D3D11 device
	if (is_service_mode) {
		U_LOG_IFL_I(U_LOGGING_INFO,
		    "D3D11 native compositor DISABLED - running in service mode (IPC). "
		    "Server-side compositor handles rendering (D3D11 service or Vulkan depending on server config)");
		return false;
	}

	// D3D11 native compositor is disabled by default because it only works
	// in in-process mode. When using IPC/service mode (displayxr-service),
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
                                   void *shared_texture_handle,
                                   struct oxr_session *sess)
{
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	// Get D3D11 display processor factory from system compositor info (set by target builder)
	void *dp_factory_d3d11 = NULL;
	if (sys->xsysc != NULL) {
		dp_factory_d3d11 = sys->xsysc->info.dp_factory_d3d11;
	}

	// Create the D3D11 native compositor
	xrt_result_t xret = comp_d3d11_compositor_create(
	    xdev, window_handle, (void *)next->device, dp_factory_d3d11, shared_texture_handle, &xcn);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create D3D11 native compositor: %d", xret);
	}

	// Set system devices for debug GUI qwerty driver support
	comp_d3d11_compositor_set_system_devices(&xcn->base, sess->sys->xsysd);

	// Pass legacy app tile scaling flag and compromise view scale to compositor
	if (sess->sys->xsysc != NULL) {
		comp_d3d11_compositor_set_legacy_app_tile_scaling(
		    &xcn->base,
		    sess->sys->xsysc->info.legacy_app_tile_scaling,
		    sess->sys->xsysc->info.legacy_view_scale_x,
		    sess->sys->xsysc->info.legacy_view_scale_y);
	}

	// Set the compositor directly - no client wrapper needed
	sess->xcn = xcn;
	sess->compositor = &xcn->base;
	// Use native swapchain create that handles xrt_swapchain_native layout
	sess->create_swapchain = oxr_swapchain_d3d11_native_create;
	// Mark that we're using D3D11 native compositor (not multi_compositor)
	sess->is_d3d11_native_compositor = true;

	// D3D11 native compositor doesn't use the multi-compositor event system,
	// so we set visibility/focus flags directly like headless mode.
	// This enables the session state to transition from READY -> FOCUSED.
	sess->compositor_visible = true;
	sess->compositor_focused = true;

	// Track external window / shared texture mode
	sess->has_external_window =
	    (window_handle != NULL || shared_texture_handle != NULL);
	if (sess->has_external_window) {
		struct xrt_device *head = GET_XDEV_BY_ROLE(sess->sys, head);
		if (head != NULL) {
			xrt_device_set_property(head, XRT_DEVICE_PROPERTY_EXT_APP_MODE, 1);
		}
	}

	U_LOG_IFL_I(U_LOGGING_INFO,"Using D3D11 native compositor (bypassing Vulkan)%s",
	            shared_texture_handle ? " — shared texture mode" : "");

	return XR_SUCCESS;
}

#endif /* XRT_HAVE_D3D11_NATIVE_COMPOSITOR */
