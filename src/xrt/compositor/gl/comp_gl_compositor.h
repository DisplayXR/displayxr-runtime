// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native OpenGL compositor — direct GL rendering, no interop.
 *
 * Pipeline: App (OpenGL) -> native GL compositor -> window
 * Platforms: Windows (WGL), Android (EGL), macOS (CGL)
 *
 * @author David Fattal
 * @ingroup comp_gl
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_system_devices;
struct xrt_system_compositor_info;

/*!
 * Create a native OpenGL compositor.
 *
 * @param xdev                  Head device.
 * @param window_handle         Platform window handle (HWND on Windows, NULL to create own).
 * @param gl_context            Platform GL context (HGLRC on Windows, EGLContext on Android, CGLContextObj on macOS).
 * @param gl_display            Platform display (HDC on Windows, EGLDisplay on Android, NULL on macOS).
 * @param dp_factory_gl         Display processor factory (may be NULL).
 * @param shared_texture_handle D3D11 shared texture HANDLE for offscreen mode (Windows only, may be NULL).
 * @param out_xcn               Output native compositor.
 * @return XRT_SUCCESS or error.
 *
 * @ingroup comp_gl
 */
xrt_result_t
comp_gl_compositor_create(struct xrt_device *xdev,
                          void *window_handle,
                          void *gl_context,
                          void *gl_display,
                          void *dp_factory_gl,
                          void *shared_texture_handle,
                          struct xrt_compositor_native **out_xcn);

/*!
 * Set system devices on a GL compositor (for qwerty debug driver etc.).
 *
 * @ingroup comp_gl
 */
void
comp_gl_compositor_set_system_devices(struct xrt_compositor *xc, struct xrt_system_devices *xsysd);

/*!
 * Set system compositor info (display dimensions etc.).
 *
 * @ingroup comp_gl
 */
void
comp_gl_compositor_set_sys_info(struct xrt_compositor *xc, const struct xrt_system_compositor_info *info);

/*!
 * Request display mode switch (2D/3D) via the GL display processor.
 *
 * @param xc        GL compositor base.
 * @param enable_3d true for 3D mode, false for 2D.
 * @return true if the display processor handled the request.
 *
 * @ingroup comp_gl
 */
bool
comp_gl_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d);

/*!
 * Get predicted eye positions from the GL display processor.
 *
 * @param xc            GL compositor base.
 * @param out_eye_pos   Output eye positions (N-view).
 * @return true if display processor provided eye positions.
 *
 * @ingroup comp_gl
 */
bool
comp_gl_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                               struct xrt_eye_positions *out_eye_pos);

#ifdef __cplusplus
}
#endif
