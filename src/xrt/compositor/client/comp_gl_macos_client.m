// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS CGL OpenGL client compositor — CGL context management
 *         and IOSurface-backed GL swapchain textures.
 * @author David Fattal
 * @ingroup comp_client
 */

#include <stdio.h>
#include <stdlib.h>

#import <OpenGL/OpenGL.h>
#import <OpenGL/CGLIOSurface.h>
#import <IOSurface/IOSurface.h>
#include <dlfcn.h>

#include "xrt/xrt_handles.h"
#include "util/u_misc.h"
#include "util/u_logging.h"

// Use GLAD for GL functions (not <OpenGL/gl3.h> which conflicts with GLAD)
#include "ogl/ogl_api.h"

#include "client/comp_gl_client.h"
#include "client/comp_gl_macos_client.h"


// ============================================================================
// CGL context helpers
// ============================================================================

static inline bool
context_matches(const struct client_gl_cgl_context *a, const struct client_gl_cgl_context *b)
{
	return a->cglContext == b->cglContext;
}

static inline void
context_save_current(struct client_gl_cgl_context *ctx)
{
	ctx->cglContext = CGLGetCurrentContext();
}

static inline bool
context_make_current(const struct client_gl_cgl_context *ctx)
{
	CGLError err = CGLSetCurrentContext((CGLContextObj)ctx->cglContext);
	return err == kCGLNoError;
}


// ============================================================================
// IOSurface-backed GL swapchain
// ============================================================================

struct client_gl_iosurface_swapchain
{
	struct client_gl_swapchain base;
};

static void
client_gl_iosurface_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct client_gl_iosurface_swapchain *sc = (struct client_gl_iosurface_swapchain *)xsc;

	uint32_t image_count = sc->base.base.base.image_count;
	struct client_gl_compositor *c = sc->base.gl_compositor;

	enum xrt_result xret = client_gl_compositor_context_begin(&c->base.base, CLIENT_GL_CONTEXT_REASON_OTHER);

	if (image_count > 0 && xret == XRT_SUCCESS) {
		glDeleteTextures(image_count, &sc->base.base.images[0]);
	}

	if (xret == XRT_SUCCESS) {
		client_gl_compositor_context_end(&c->base.base, CLIENT_GL_CONTEXT_REASON_OTHER);
	}

	xrt_swapchain_reference((struct xrt_swapchain **)&sc->base.xscn, NULL);
	free(sc);
}

/*!
 * Convert Metal pixel format to GL internal format for IOSurface binding.
 */
static GLenum
metal_format_to_gl_internal(int64_t metal_fmt)
{
	switch (metal_fmt) {
	case 70:  return GL_RGBA8;       // MTLPixelFormatRGBA8Unorm
	case 71:  return GL_SRGB8_ALPHA8; // MTLPixelFormatRGBA8Unorm_sRGB
	case 80:  return GL_RGBA8;       // MTLPixelFormatBGRA8Unorm (GL doesn't distinguish BGRA internal)
	case 81:  return GL_SRGB8_ALPHA8; // MTLPixelFormatBGRA8Unorm_sRGB
	case 115: return GL_RGBA16F;     // MTLPixelFormatRGBA16Float
	default:  return GL_RGBA8;
	}
}

static GLenum
metal_format_to_gl_format(int64_t metal_fmt)
{
	switch (metal_fmt) {
	case 80:
	case 81:  return GL_BGRA; // MTLPixelFormatBGRA8Unorm[_sRGB]
	default:  return GL_RGBA;
	}
}

static GLenum
metal_format_to_gl_type(int64_t metal_fmt)
{
	switch (metal_fmt) {
	case 115: return GL_HALF_FLOAT; // MTLPixelFormatRGBA16Float
	default:  return GL_UNSIGNED_BYTE;
	}
}

struct xrt_swapchain *
client_gl_iosurface_swapchain_create(struct xrt_compositor *xc,
                                      const struct xrt_swapchain_create_info *info,
                                      struct xrt_swapchain_native *xscn,
                                      struct client_gl_swapchain **out_cglsc)
{
	struct client_gl_compositor *c = client_gl_compositor(xc);

	if (xscn == NULL) {
		U_LOG_E("No native swapchain provided for IOSurface GL binding");
		return NULL;
	}

	struct xrt_swapchain *native_xsc = &xscn->base;

	struct client_gl_iosurface_swapchain *sc = U_TYPED_CALLOC(struct client_gl_iosurface_swapchain);
	sc->base.base.base.destroy = client_gl_iosurface_swapchain_destroy;
	sc->base.base.base.reference.count = 1;
	sc->base.base.base.image_count = native_xsc->image_count;
	sc->base.xscn = xscn;
	sc->base.tex_target = GL_TEXTURE_RECTANGLE;
	sc->base.gl_compositor = c;

	struct xrt_swapchain_gl *xscgl = &sc->base.base;
	glGenTextures(native_xsc->image_count, xscgl->images);

	GLenum gl_internal = metal_format_to_gl_internal(info->format);
	GLenum gl_format = metal_format_to_gl_format(info->format);
	GLenum gl_type = metal_format_to_gl_type(info->format);

	for (uint32_t i = 0; i < native_xsc->image_count; i++) {
		IOSurfaceRef surface = (IOSurfaceRef)xscn->images[i].handle;
		if (surface == NULL) {
			U_LOG_E("Swapchain image %u has no IOSurface handle", i);
			continue;
		}

		glBindTexture(GL_TEXTURE_RECTANGLE, xscgl->images[i]);

		// Bind the IOSurface to this GL texture via CGLTexImageIOSurface2D.
		// This creates a zero-copy shared backing between GL and Metal.
		CGLContextObj ctx = CGLGetCurrentContext();
		CGLError err = CGLTexImageIOSurface2D(
		    ctx,
		    GL_TEXTURE_RECTANGLE,
		    gl_internal,
		    (GLsizei)info->width,
		    (GLsizei)info->height,
		    gl_format,
		    gl_type,
		    surface,
		    0); // plane

		if (err != kCGLNoError) {
			U_LOG_E("CGLTexImageIOSurface2D failed for image %u: error %d", i, err);
		} else {
			U_LOG_W("IOSurface GL texture %u: %ux%u (internal=0x%x)",
			         i, info->width, info->height, gl_internal);
		}

		glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	glBindTexture(GL_TEXTURE_RECTANGLE, 0);

	*out_cglsc = &sc->base;
	return &sc->base.base.base;
}


// ============================================================================
// Client compositor
// ============================================================================

static inline struct client_gl_macos_compositor *
client_gl_macos_compositor(struct xrt_compositor *xc)
{
	return (struct client_gl_macos_compositor *)xc;
}

static void
client_gl_macos_compositor_destroy(struct xrt_compositor *xc)
{
	struct client_gl_macos_compositor *c = client_gl_macos_compositor(xc);
	client_gl_compositor_fini(&c->base);
	free(c);
}

static xrt_result_t
client_gl_context_begin_locked(struct xrt_compositor *xc, enum client_gl_context_reason reason)
{
	struct client_gl_macos_compositor *c = client_gl_macos_compositor(xc);

	context_save_current(&c->temp_context);

	bool need_make_current = !context_matches(&c->temp_context, &c->app_context);

	if (need_make_current && !context_make_current(&c->app_context)) {
		U_LOG_E("Failed to make CGL context current");
		return XRT_ERROR_OPENGL;
	}

	return XRT_SUCCESS;
}

static void
client_gl_context_end_locked(struct xrt_compositor *xc, enum client_gl_context_reason reason)
{
	struct client_gl_macos_compositor *c = client_gl_macos_compositor(xc);

	bool need_make_current = !context_matches(&c->temp_context, &c->app_context);

	if (need_make_current && !context_make_current(&c->temp_context)) {
		U_LOG_E("Failed to restore old CGL context!");
	}
}


// ============================================================================
// GLAD loader for macOS (dlsym on OpenGL framework)
// ============================================================================

static GLADapiproc
gl_get_proc_addr_macos(void *userptr, const char *name)
{
	return (GLADapiproc)dlsym(userptr, name);
}


// ============================================================================
// Public API
// ============================================================================

struct client_gl_macos_compositor *
client_gl_macos_compositor_create(struct xrt_compositor_native *xcn, void *cglContext)
{
	// Save current context
	struct client_gl_cgl_context current_ctx;
	context_save_current(&current_ctx);

	struct client_gl_cgl_context app_ctx = {
	    .cglContext = cglContext,
	};

	// Make app context current to load GL functions
	bool need_make_current = !context_matches(&current_ctx, &app_ctx);
	if (need_make_current && !context_make_current(&app_ctx)) {
		U_LOG_E("Failed to make CGL context current for initialization");
		return NULL;
	}

	// Load GL functions via GLAD using dlsym on the OpenGL framework
	void *gl_framework = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
	if (gl_framework == NULL) {
		U_LOG_E("Failed to open OpenGL framework: %s", dlerror());
		if (need_make_current) context_make_current(&current_ctx);
		return NULL;
	}

	int gl_result = gladLoadGLUserPtr(gl_get_proc_addr_macos, gl_framework);

	if (glGetString != NULL) {
		U_LOG_W("OpenGL context (macOS CGL):"
		        "\n\tGL_VERSION: %s"
		        "\n\tGL_RENDERER: %s"
		        "\n\tGL_VENDOR: %s",
		        glGetString(GL_VERSION),
		        glGetString(GL_RENDERER),
		        glGetString(GL_VENDOR));
	}

	// Restore prior context
	if (need_make_current && !context_make_current(&current_ctx)) {
		U_LOG_E("Failed to restore old CGL context during init!");
	}

	if (gl_result == 0) {
		U_LOG_E("Failed to load GL functions via GLAD: 0x%08x", gl_result);
		dlclose(gl_framework);
		return NULL;
	}

	// Create the compositor
	struct client_gl_macos_compositor *c = U_TYPED_CALLOC(struct client_gl_macos_compositor);
	c->app_context = app_ctx;

	if (!client_gl_compositor_init(
	        &c->base,
	        xcn,
	        client_gl_context_begin_locked,
	        client_gl_context_end_locked,
	        client_gl_iosurface_swapchain_create,
	        NULL)) {
		U_LOG_E("Failed to init parent GL client compositor!");
		free(c);
		return NULL;
	}

	c->base.base.base.destroy = client_gl_macos_compositor_destroy;

	U_LOG_W("macOS CGL GL client compositor created (IOSurface swapchains)");

	return c;
}
