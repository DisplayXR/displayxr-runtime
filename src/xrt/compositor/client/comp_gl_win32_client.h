// Copyright 2019-2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL on Win32 client side glue to compositor header.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_client
 */

#pragma once

#include "xrt/xrt_gfx_win32.h"
#include "client/comp_gl_client.h"

#ifdef __cplusplus
extern "C" {
#endif

//! #573 — render-API-agnostic transparent DComp presenter (client/comp_d3d_transparent_present.h).
struct comp_d3d_transparent_presenter;

struct client_gl_context
{
	HDC hDC;
	HGLRC hGLRC;
};

/*!
 * @class client_gl_win32_compositor
 * A client facing win32 OpenGL base compositor.
 *
 * @ingroup comp_client
 * @extends client_gl_compositor
 */
struct client_gl_win32_compositor
{
	//! OpenGL compositor wrapper base.
	struct client_gl_compositor base;

	/*!
	 * Temporary storage for "current" OpenGL context while app_context is
	 * made current using context_begin/context_end. We only need one because
	 * app_context can only be made current in one thread at a time too.
	 */
	struct client_gl_context temp_context;

	//! GL context provided in graphics binding.
	struct client_gl_context app_context;

	//! The OpenGL library
	HMODULE opengl;

	/*!
	 * #573 — render-API-agnostic transparent DComp presenter. For a forced-IPC
	 * transparent client the service weaves (with alpha-gate holes) into a shared
	 * texture; this presenter imports it + the service→client fence and presents it
	 * via a transparent DirectComposition swap chain on the app's own window so DWM
	 * blends the LIVE desktop into the holes. NULL for opaque clients. The GL client
	 * passes NULL device → the helper makes its own D3D11 device (the present is pure
	 * D3D11 + DComp on shared handles, independent of the app's render API).
	 */
	struct comp_d3d_transparent_presenter *transparent;

	//! #573 — saved parent GL layer_commit, wrapped to drive the per-frame present.
	xrt_result_t (*base_layer_commit)(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle);
};

/*!
 * Create a new client_gl_win32_compositor.
 *
 * @public @memberof client_gl_win32_compositor
 * @see xrt_compositor_native
 */
struct client_gl_win32_compositor *
client_gl_win32_compositor_create(struct xrt_compositor_native *xcn, void *hDC, void *hGLRC);


#ifdef __cplusplus
}
#endif
