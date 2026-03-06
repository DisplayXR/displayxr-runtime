// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS CGL OpenGL client compositor header.
 * @author David Fattal
 * @ingroup comp_client
 */

#pragma once

#include "client/comp_gl_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * CGL context state for save/restore.
 */
struct client_gl_cgl_context
{
	void *cglContext; //!< CGLContextObj
};

/*!
 * @class client_gl_macos_compositor
 * @extends client_gl_compositor
 *
 * macOS OpenGL client compositor using CGL for context management
 * and IOSurface-backed textures for Metal interop.
 *
 * @ingroup comp_client
 */
struct client_gl_macos_compositor
{
	struct client_gl_compositor base;

	//! The app's CGL context (used for rendering).
	struct client_gl_cgl_context app_context;

	//! Saved CGL context (for save/restore on context switch).
	struct client_gl_cgl_context temp_context;
};

/*!
 * Create a macOS CGL OpenGL client compositor.
 *
 * @param xcn          Native compositor to wrap (takes ownership).
 * @param cglContext   CGLContextObj from the app.
 *
 * @ingroup comp_client
 */
struct client_gl_macos_compositor *
client_gl_macos_compositor_create(struct xrt_compositor_native *xcn, void *cglContext);

#ifdef __cplusplus
}
#endif
