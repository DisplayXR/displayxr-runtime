// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header defining macOS OpenGL graphics provider.
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create an OpenGL compositor client for macOS using CGL.
 *
 * The client uses IOSurface-backed textures for zero-copy sharing
 * with the Metal native compositor.
 *
 * @param xcn         Native compositor to wrap (takes ownership).
 * @param cglContext  CGLContextObj — the app's CGL rendering context.
 *
 * @ingroup xrt_iface
 * @public @memberof xrt_compositor_native
 */
struct xrt_compositor_gl *
xrt_gfx_provider_create_gl_macos(struct xrt_compositor_native *xcn, void *cglContext);

#ifdef __cplusplus
}
#endif
