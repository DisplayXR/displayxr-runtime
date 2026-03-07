// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL session using native GL compositor (bypasses Vulkan).
 *
 * On Windows, OpenGL apps are routed through the native GL compositor
 * instead of the Vulkan interop path. The compositor creates its own
 * WGL context that shares textures with the app via wglShareLists.
 *
 * @author David Fattal
 * @ingroup oxr_main
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

#ifdef XRT_HAVE_GL_NATIVE_COMPOSITOR
#include "gl/comp_gl_compositor.h"
#endif

/*
 * Environment variable to enable/disable GL native compositor.
 * Default is TRUE — GL native compositor is enabled by default for in-process mode.
 * Set OXR_ENABLE_GL_NATIVE_COMPOSITOR=0 to force Vulkan interop (for debugging).
 */
DEBUG_GET_ONCE_BOOL_OPTION(enable_gl_native_compositor, "OXR_ENABLE_GL_NATIVE_COMPOSITOR", true)

bool
oxr_gl_native_compositor_supported(struct oxr_system *sys)
{
#ifdef XRT_HAVE_GL_NATIVE_COMPOSITOR
	// Check if we're running in service/IPC mode
	bool is_service_mode = sys->xsysc != NULL && sys->xsysc->info.is_service_mode;

	bool env_enabled = debug_get_bool_option_enable_gl_native_compositor();
	U_LOG_IFL_I(U_LOGGING_INFO, "GL native compositor check: XRT_HAVE_GL_NATIVE_COMPOSITOR=defined, "
	        "OXR_ENABLE_GL_NATIVE_COMPOSITOR=%s, is_service_mode=%s",
	        env_enabled ? "1 (enabled)" : "0 (disabled)",
	        is_service_mode ? "true" : "false");

	if (is_service_mode) {
		U_LOG_IFL_I(U_LOGGING_INFO,
		    "GL native compositor DISABLED - running in service mode (IPC)");
		return false;
	}

	if (!env_enabled) {
		U_LOG_IFL_I(U_LOGGING_INFO, "GL native compositor DISABLED - falling back to Vulkan interop");
		return false;
	}

	U_LOG_IFL_I(U_LOGGING_INFO, "GL native compositor ENABLED");
	return true;
#else
	U_LOG_IFL_I(U_LOGGING_INFO, "GL native compositor check: XRT_HAVE_GL_NATIVE_COMPOSITOR=NOT defined");
	(void)sys;
	return false;
#endif
}

#ifdef XRT_HAVE_GL_NATIVE_COMPOSITOR

XrResult
oxr_session_populate_gl_native(struct oxr_logger *log,
                                struct oxr_system *sys,
                                void *gl_context,
                                void *gl_display,
                                void *shared_texture_handle,
                                struct oxr_session *sess)
{
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	// Get GL display processor factory from system compositor info
	void *dp_factory_gl = NULL;
	// TODO: dp_factory_gl when GL display processors are implemented

	// Create the GL native compositor
	xrt_result_t xret = comp_gl_compositor_create(
	    xdev,
	    NULL,  // window_handle (compositor creates its own)
	    gl_context,
	    gl_display,
	    dp_factory_gl,
	    shared_texture_handle,
	    &xcn);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create GL native compositor: %d", xret);
	}

	// Set system devices for qwerty driver support
	comp_gl_compositor_set_system_devices(&xcn->base, sess->sys->xsysd);

	// Set system compositor info for display dimensions
	if (sys->xsysc != NULL) {
		comp_gl_compositor_set_sys_info(&xcn->base, &sys->xsysc->info);
	}

	// Set the compositor directly — no client wrapper needed.
	// The GL native compositor creates swapchains with xrt_swapchain_gl
	// as the first member, so oxr_swapchain_gl_create works directly.
	sess->xcn = xcn;
	sess->compositor = &xcn->base;
	sess->create_swapchain = oxr_swapchain_gl_create;
	sess->is_gl_native_compositor = true;

	// Native compositor doesn't use multi-compositor event system,
	// so set visibility/focus flags directly.
	sess->compositor_visible = true;
	sess->compositor_focused = true;

	U_LOG_W("Using GL native compositor (bypassing Vulkan)");

	return XR_SUCCESS;
}

#endif /* XRT_HAVE_GL_NATIVE_COMPOSITOR */
