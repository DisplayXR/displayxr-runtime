// Copyright 2019-2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Win32 client side glue to compositor implementation.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Milan Jaros <milan.jaros@vsb.cz>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_client
 */

#include <stdio.h>
#include <stdlib.h>

#include "client/comp_gl_client.h"
#include "util/u_misc.h"
#include "util/u_logging.h"

#include "xrt/xrt_gfx_win32.h"

#include "client/comp_gl_win32_client.h"
#include "client/comp_gl_memobj_swapchain.h"
#include "client/comp_gl_d3d11_swapchain.h"
#include "client/comp_d3d_transparent_present.h"

#include "ogl/ogl_api.h"
#include "ogl/wgl_api.h"

// #573 — transparent compositing output bridges, defined in
// src/xrt/ipc/client/ipc_client_compositor.c (resolved at link time). Shared
// with the D3D11/D3D12 clients.
extern xrt_result_t
comp_ipc_client_compositor_get_transparent_output(struct xrt_compositor *xc,
                                                  bool *out_have_output,
                                                  uint32_t *out_width,
                                                  uint32_t *out_height,
                                                  uint64_t *out_hwnd,
                                                  xrt_graphics_buffer_handle_t *out_handle);
extern xrt_result_t
comp_ipc_client_compositor_get_transparent_output_fence(struct xrt_compositor *xc,
                                                        bool *out_have_fence,
                                                        xrt_graphics_sync_handle_t *out_handle);

/*
 *
 * OpenGL context helper.
 *
 */

static inline bool
context_matches(const struct client_gl_context *a, const struct client_gl_context *b)
{
	return a->hDC == b->hDC && a->hGLRC == b->hGLRC;
}

static inline void
context_save_current(struct client_gl_context *current_ctx)
{
	current_ctx->hDC = wglGetCurrentDC();
	current_ctx->hGLRC = wglGetCurrentContext();
}

static inline bool
context_make_current(const struct client_gl_context *ctx_to_make_current)
{
	if (wglMakeCurrent(ctx_to_make_current->hDC, ctx_to_make_current->hGLRC)) {
		return true;
	}
	return false;
}

/*!
 * Down-cast helper.
 *
 * @private @memberof client_gl_win32_compositor
 */
static inline struct client_gl_win32_compositor *
client_gl_win32_compositor(struct xrt_compositor *xc)
{
	return (struct client_gl_win32_compositor *)xc;
}

static void
client_gl_win32_compositor_destroy(struct xrt_compositor *xc)
{
	struct client_gl_win32_compositor *c = client_gl_win32_compositor(xc);

	comp_d3d_transparent_presenter_destroy(&c->transparent); // #573

	client_gl_compositor_fini(&c->base);

	FreeLibrary(c->opengl);
	c->opengl = NULL;

	free(c);
}

// #573 — wraps the parent GL client's layer_commit to drive the per-frame
// transparent present after the woven frame is committed + signaled. The shared
// presenter GPU-waits the service→client fence (lockstep counter, no per-frame
// IPC), copies the woven shared output into the DComp swap-chain back buffer, and
// presents so DWM blends the LIVE desktop into the alpha-gate holes. No-op (just
// the base commit) for opaque clients where @ref transparent is NULL.
static xrt_result_t
client_gl_win32_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct client_gl_win32_compositor *c = client_gl_win32_compositor(xc);

	xrt_result_t xret = c->base_layer_commit(xc, sync_handle);
	if (xret == XRT_SUCCESS) {
		comp_d3d_transparent_presenter_present(c->transparent);
	}
	return xret;
}

static xrt_result_t
client_gl_context_begin_locked(struct xrt_compositor *xc, enum client_gl_context_reason reason)
{
	struct client_gl_win32_compositor *c = client_gl_win32_compositor(xc);

	struct client_gl_context *app_ctx = &c->app_context;

	context_save_current(&c->temp_context);

	bool need_make_current = !context_matches(&c->temp_context, app_ctx);

	U_LOG_T("GL Context begin: need makeCurrent: %d (current %p -> app %p)", need_make_current,
	        (void *)c->temp_context.hGLRC, (void *)app_ctx->hGLRC);

	if (need_make_current && !context_make_current(app_ctx)) {
		U_LOG_E("Failed to make WGL context current");
		// No need to restore on failure.
		return XRT_ERROR_OPENGL;
	}

	return XRT_SUCCESS;
}

static void
client_gl_context_end_locked(struct xrt_compositor *xc, enum client_gl_context_reason reason)
{
	struct client_gl_win32_compositor *c = client_gl_win32_compositor(xc);

	struct client_gl_context *app_ctx = &c->app_context;

	struct client_gl_context *current_wgl_context = &c->temp_context;

	bool need_make_current = !context_matches(&c->temp_context, app_ctx);

	U_LOG_T("GL Context end: need makeCurrent: %d (app %p -> current %p)", need_make_current,
	        (void *)app_ctx->hGLRC, (void *)c->temp_context.hGLRC);

	if (need_make_current && !context_make_current(current_wgl_context)) {
		U_LOG_E("Failed to make old WGL context current!");
		// fall through to os_mutex_unlock even if we didn't succeed in restoring the context
	}
}

static GLADapiproc
client_gl_get_proc_addr(void *userptr, const char *name)
{
	GLADapiproc ret = (GLADapiproc)wglGetProcAddress(name);
	if (ret == NULL) {
		ret = (GLADapiproc)GetProcAddress((HMODULE)userptr, name);
	}
	return ret;
}

struct client_gl_win32_compositor *
client_gl_win32_compositor_create(struct xrt_compositor_native *xcn, void *hDC, void *hGLRC)
{
	// Save old GLX context.
	struct client_gl_context current_ctx;
	context_save_current(&current_ctx);

	// The context and drawables given from the app.
	struct client_gl_context app_ctx = {
	    .hDC = hDC,
	    .hGLRC = hGLRC,
	};


	/*
	 * Make given context current if needed.
	 */

	bool need_make_current = !context_matches(&current_ctx, &app_ctx);

	if (need_make_current && !context_make_current(&app_ctx)) {
		U_LOG_E("Failed to make WGL context current");
		// No need to restore on failure.
		return NULL;
	}


	/*
	 * Load functions.
	 */

	HMODULE opengl = LoadLibraryW(L"opengl32.dll");

	int wgl_result = gladLoadWGLUserPtr(hDC, client_gl_get_proc_addr, opengl);
	int gl_result = gladLoadGLUserPtr(client_gl_get_proc_addr, opengl);

	if (glGetString != NULL) {
		U_LOG_D(                      //
		    "OpenGL context:"         //
		    "\n\tGL_VERSION: %s"      //
		    "\n\tGL_RENDERER: %s"     //
		    "\n\tGL_VENDOR: %s",      //
		    glGetString(GL_VERSION),  //
		    glGetString(GL_RENDERER), //
		    glGetString(GL_VENDOR));  //
	}


	/*
	 * Return to app context.
	 */

	if (need_make_current && !context_make_current(&current_ctx)) {
		U_LOG_E("Failed to make old WGL context current!");
	}


	/*
	 * Checking of context.
	 */

	// Only do error checking here.
	if (wgl_result == 0 || gl_result == 0) {
		U_LOG_E("Failed to load GLAD functions gladLoadWGL: 0x%08x, gladLoadGL: 0x%08x", wgl_result, gl_result);
		FreeLibrary(opengl);
		return NULL;
	}

	// Prefer WGL_NV_DX_interop2 for D3D11 shared texture import (IPC/workspace mode).
	// Fall back to GL_EXT_memory_object for VK-exported handles (in-process mode).
	client_gl_swapchain_create_func_t swapchain_create_fn = NULL;
	if (client_gl_d3d11_interop_available()) {
		U_LOG_W("Using WGL_NV_DX_interop2 for swapchain import");
		swapchain_create_fn = client_gl_d3d11_swapchain_create;
	} else if (GLAD_GL_EXT_memory_object && GLAD_GL_EXT_memory_object_win32) {
		U_LOG_W("Using GL_EXT_memory_object for swapchain import");
		swapchain_create_fn = client_gl_memobj_swapchain_create;
	} else {
		U_LOG_E("Neither WGL_NV_DX_interop2 nor GL_EXT_memory_object available");
		FreeLibrary(opengl);
		return NULL;
	}


	/*
	 * Checking complete, create client compositor here.
	 */

	struct client_gl_win32_compositor *c = U_TYPED_CALLOC(struct client_gl_win32_compositor);

	// Move the app context to the struct.
	c->app_context = app_ctx;
	// Same for the opengl library handle
	c->opengl = opengl;

	if (!client_gl_compositor_init(            //
	        &c->base,                          //
	        xcn,                               //
	        client_gl_context_begin_locked,    //
	        client_gl_context_end_locked,      //
	        swapchain_create_fn,               //
	        NULL)) {                           //
		U_LOG_E("Failed to init parent GL client compositor!");
		FreeLibrary(opengl);
		free(c);
		return NULL;
	}

	c->base.base.base.destroy = client_gl_win32_compositor_destroy;

	// #573 — transparent compositing output. If the service created a shared output
	// texture for this (transparent) client, hand the shared texture + service→client
	// fence to the render-API-agnostic presenter, which stands up a transparent
	// DirectComposition swap chain on the app's own window. GL passes NULL device →
	// the helper makes its own D3D11 device for the (pure D3D11+DComp) present. We
	// then wrap the parent's layer_commit so the present runs each frame. Non-fatal:
	// any failure leaves the service's opaque present in effect (presenter stays NULL).
	{
		bool have_out = false, have_fence = false;
		uint32_t tw = 0, th = 0;
		uint64_t hwnd_val = 0;
		xrt_graphics_buffer_handle_t tex_h = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		xrt_graphics_sync_handle_t fence_h = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		xrt_result_t oret = comp_ipc_client_compositor_get_transparent_output(
		    &xcn->base, &have_out, &tw, &th, &hwnd_val, &tex_h);
		if (oret == XRT_SUCCESS && have_out && tex_h != XRT_GRAPHICS_BUFFER_HANDLE_INVALID &&
		    hwnd_val != 0) {
			(void)comp_ipc_client_compositor_get_transparent_output_fence(&xcn->base, &have_fence,
			                                                             &fence_h);
			// The presenter consumes (closes) both handles regardless of outcome.
			c->transparent =
			    comp_d3d_transparent_presenter_create(NULL, hwnd_val, tw, th, tex_h, fence_h);
		} else if (tex_h != XRT_GRAPHICS_BUFFER_HANDLE_INVALID) {
			CloseHandle((HANDLE)tex_h);
		}

		if (c->transparent != NULL) {
			c->base_layer_commit = c->base.base.base.layer_commit;
			c->base.base.base.layer_commit = client_gl_win32_compositor_layer_commit;
		}
	}

	return c;
}
