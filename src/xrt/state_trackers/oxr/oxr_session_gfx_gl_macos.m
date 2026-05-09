// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL session using native GL compositor on macOS.
 *
 * On macOS, OpenGL apps are routed through the native GL compositor:
 *   App (OpenGL) -> GL native compositor (NSOpenGLView) -> display
 *
 * This is a direct GL pipeline — no Metal or Vulkan interop involved.
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

#if defined(XRT_HAVE_GL_NATIVE_COMPOSITOR)

// Fully native GL compositor — handles both windowed and shared IOSurface modes
#include "gl/comp_gl_compositor.h"
#include "openxr/XR_EXT_macos_gl_binding.h"

XrResult
oxr_session_populate_gl_macos(struct oxr_logger *log,
                               struct oxr_system *sys,
                               const void *next_ptr,
                               void *window_handle,
                               void *shared_iosurface,
                               bool transparent_background,
                               struct oxr_session *sess)
{
	const XrGraphicsBindingOpenGLMacOSEXT *next = (const XrGraphicsBindingOpenGLMacOSEXT *)next_ptr;

	// TODO: plumb transparent_background through comp_gl_compositor_create.
	// The GL native macOS compositor uses NSOpenGLView, which would need
	// setOpaque:NO + clear pixel-format alpha to match the Metal path. Not
	// implemented in PR #4 — request via XR_EXT_cocoa_window_binding will
	// be a no-op on the GL native path until then.
	if (transparent_background) {
		U_LOG_W("oxr_session_populate_gl_macos: transparentBackgroundEnabled requested but "
		        "GL native compositor doesn't yet implement NSOpenGLView alpha — ignoring");
	}

	// Both windowed and shared IOSurface modes use the GL native compositor
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	xrt_result_t xret = comp_gl_compositor_create(
	    xdev,
	    window_handle,        // NSView from cocoa_window_binding (or NULL for shared texture)
	    next->cglContext,     // App's CGLContextObj for texture sharing
	    NULL,                 // gl_display (not used on macOS)
	    (sys->xsysc != NULL) ? sys->xsysc->info.dp_factory_gl : NULL,
	    shared_iosurface,     // IOSurfaceRef for shared texture mode (or NULL for windowed)
	    /*transparent_background*/ false,  // GL transparent present path is deferred (PR #3b §3b)
	    /*chroma_key_color*/ 0,
	    &xcn);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create GL native compositor: %d", xret);
	}

	comp_gl_compositor_set_system_devices(&xcn->base, sess->sys->xsysd);
	if (sys->xsysc != NULL) {
		comp_gl_compositor_set_sys_info(&xcn->base, &sys->xsysc->info);
	}

	sess->xcn = xcn;
	sess->compositor = &xcn->base;
	sess->create_swapchain = oxr_swapchain_gl_create;
	sess->is_gl_native_compositor = true;

	sess->compositor_visible = true;
	sess->compositor_focused = true;

	if (shared_iosurface != NULL) {
		U_LOG_W("Using GL native compositor on macOS (shared IOSurface, no Metal routing)");
	} else {
		U_LOG_W("Using GL native compositor on macOS (direct NSOpenGLView, no Metal routing)");
	}

	return XR_SUCCESS;
}

#elif defined(XRT_HAVE_METAL_NATIVE_COMPOSITOR)
// Metal-only fallback (no GL native compositor available)

#include "metal/comp_metal_compositor.h"
#include "openxr/XR_EXT_macos_gl_binding.h"

static int64_t
metal_format_to_vk(int64_t metal_fmt)
{
	switch (metal_fmt) {
	case 70:  return 37;
	case 71:  return 43;
	case 80:  return 44;
	case 81:  return 50;
	case 115: return 97;
	case 252: return 126;
	default:  return metal_fmt;
	}
}

XrResult
oxr_session_populate_gl_macos(struct oxr_logger *log,
                               struct oxr_system *sys,
                               const void *next_ptr,
                               void *window_handle,
                               void *shared_iosurface,
                               struct oxr_session *sess)
{
	const XrGraphicsBindingOpenGLMacOSEXT *next = (const XrGraphicsBindingOpenGLMacOSEXT *)next_ptr;
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	void *dp_factory_metal = NULL;
	if (sys->xsysc != NULL) {
		dp_factory_metal = sys->xsysc->info.dp_factory_metal;
	}

	bool offscreen = (window_handle == NULL && shared_iosurface != NULL);

	xrt_result_t xret = comp_metal_compositor_create(
	    xdev, window_handle, NULL, dp_factory_metal,
	    offscreen, shared_iosurface, transparent_background, &xcn);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create Metal native compositor for GL app: %d", xret);
	}

	comp_metal_compositor_set_source_gl(&xcn->base);
	comp_metal_compositor_set_system_devices(&xcn->base, sess->sys->xsysd);
	if (sys->xsysc != NULL) {
		comp_metal_compositor_set_sys_info(&xcn->base, &sys->xsysc->info);
	}

	for (uint32_t i = 0; i < xcn->base.info.format_count; i++) {
		xcn->base.info.formats[i] = metal_format_to_vk(xcn->base.info.formats[i]);
	}

	struct xrt_compositor_gl *xcgl = xrt_gfx_provider_create_gl_macos(xcn, next->cglContext);
	if (xcgl == NULL) {
		xrt_comp_native_destroy(&xcn);
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create GL client compositor wrapping Metal native");
	}

	sess->xcn = xcn;
	sess->compositor = &xcgl->base;
	sess->create_swapchain = oxr_swapchain_gl_create;

	xcgl->base.info.initial_visible = xcn->base.info.initial_visible;
	xcgl->base.info.initial_focused = xcn->base.info.initial_focused;

	U_LOG_W("Using Metal native compositor for OpenGL app (GL -> Metal via IOSurface)");

	return XR_SUCCESS;
}

#endif
