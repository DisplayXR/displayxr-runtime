// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Glue code to macOS CGL OpenGL client side code.
 * @author David Fattal
 * @ingroup comp_client
 */

#include <stdio.h>
#include <stdlib.h>

#include "xrt/xrt_gfx_macos_gl.h"
#include "client/comp_gl_macos_client.h"


struct xrt_compositor_gl *
xrt_gfx_provider_create_gl_macos(struct xrt_compositor_native *xcn, void *cglContext)
{
	struct client_gl_macos_compositor *xcc = client_gl_macos_compositor_create(xcn, cglContext);
	if (xcc == NULL) {
		return NULL;
	}

	return &xcc->base.base;
}
