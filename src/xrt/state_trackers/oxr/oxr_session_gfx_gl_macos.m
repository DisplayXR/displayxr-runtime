// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL session using Metal native compositor for presentation (macOS).
 *
 * On macOS, OpenGL apps are routed through the Metal native compositor:
 *   App (OpenGL) -> comp_gl_client -> Metal native compositor -> CAMetalLayer
 *
 * The Metal compositor creates IOSurface-backed textures. The GL client
 * imports them via CGLTexImageIOSurface2D for zero-copy cross-API sharing.
 *
 * @author David Fattal
 * @ingroup oxr_main
 */

#include <stdlib.h>

#include "util/u_misc.h"
#include "util/u_logging.h"

#include "xrt/xrt_instance.h"
#include "xrt/xrt_config_have.h"
#include "xrt/xrt_gfx_macos_gl.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_handle.h"

#if defined(XRT_HAVE_METAL_NATIVE_COMPOSITOR) && defined(XRT_HAVE_OPENGL)

#include "metal/comp_metal_compositor.h"
#include "openxr/XR_EXT_macos_gl_binding.h"

/*!
 * Convert Metal pixel format enum values to GL format enum values.
 * Metal and GL use different format numbering.
 */
static int64_t
metal_format_to_gl(int64_t metal_fmt)
{
	switch (metal_fmt) {
	case 70:  return 0x8058; // MTLPixelFormatRGBA8Unorm -> GL_RGBA8
	case 71:  return 0x8C43; // MTLPixelFormatRGBA8Unorm_sRGB -> GL_SRGB8_ALPHA8
	case 80:  return 0x8058; // MTLPixelFormatBGRA8Unorm -> GL_RGBA8
	case 81:  return 0x8C43; // MTLPixelFormatBGRA8Unorm_sRGB -> GL_SRGB8_ALPHA8
	case 115: return 0x881A; // MTLPixelFormatRGBA16Float -> GL_RGBA16F
	default:  return metal_fmt;
	}
}

XrResult
oxr_session_populate_gl_macos(struct oxr_logger *log,
                               struct oxr_system *sys,
                               const void *next_ptr,
                               struct oxr_session *sess)
{
	const XrGraphicsBindingOpenGLMacOSEXT *next = (const XrGraphicsBindingOpenGLMacOSEXT *)next_ptr;
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	// Get Metal display processor factory from system compositor info
	void *dp_factory_metal = NULL;
	if (sys->xsysc != NULL) {
		dp_factory_metal = sys->xsysc->info.dp_factory_metal;
	}

	// Create the Metal native compositor (it will create its own device)
	xrt_result_t xret = comp_metal_compositor_create(
	    xdev,
	    NULL,  // window_handle
	    NULL,  // command_queue
	    dp_factory_metal,
	    false, // offscreen
	    NULL,  // shared_iosurface
	    &xcn);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create Metal native compositor for GL app: %d", xret);
	}

	// Set system devices for qwerty driver support
	comp_metal_compositor_set_system_devices(&xcn->base, sess->sys->xsysd);

	// Set system compositor info for display dimensions
	if (sys->xsysc != NULL) {
		comp_metal_compositor_set_sys_info(&xcn->base, &sys->xsysc->info);
	}

	// Convert format list from Metal to GL enum values
	for (uint32_t i = 0; i < xcn->base.info.format_count; i++) {
		xcn->base.info.formats[i] = metal_format_to_gl(xcn->base.info.formats[i]);
	}

	// Wrap the Metal native compositor with a macOS GL client compositor.
	// comp_gl_macos_client imports IOSurface-backed textures as GL textures.
	struct xrt_compositor_gl *xcgl = xrt_gfx_provider_create_gl_macos(xcn, next->cglContext);

	if (xcgl == NULL) {
		xrt_comp_native_destroy(&xcn);
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create GL client compositor wrapping Metal native");
	}

	sess->xcn = xcn;
	sess->compositor = &xcgl->base;
	sess->create_swapchain = oxr_swapchain_gl_create;

	// Propagate native compositor's visibility/focus flags
	xcgl->base.info.initial_visible = xcn->base.info.initial_visible;
	xcgl->base.info.initial_focused = xcn->base.info.initial_focused;

	U_LOG_W("Using Metal native compositor for OpenGL app (GL -> Metal via IOSurface)");

	return XR_SUCCESS;
}

#endif /* XRT_HAVE_METAL_NATIVE_COMPOSITOR && XRT_HAVE_OPENGL */
