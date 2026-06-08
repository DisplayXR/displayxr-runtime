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
#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_system_devices;
struct xrt_system_compositor_info;
struct xrt_rect;

/*!
 * Create a native OpenGL compositor.
 *
 * @param xdev                   Head device.
 * @param window_handle          Platform window handle (HWND on Windows, NULL to create own).
 * @param gl_context             Platform GL context (HGLRC on Windows, EGLContext on Android, CGLContextObj on macOS).
 * @param gl_display             Platform display (HDC on Windows, EGLDisplay on Android, NULL on macOS).
 * @param dp_factory_gl          Display processor factory (may be NULL).
 * @param shared_texture_handle  D3D11 shared texture HANDLE for offscreen mode (Windows only, may be NULL).
 * @param transparent_background Request transparent desktop composition. On Windows the
 *                               compositor switches to a DComp + WGL_NV_DX_interop2 present
 *                               path; falls back to opaque WGL SwapBuffers if the GPU/driver
 *                               doesn't expose interop.
 * @param chroma_key_color       0x00BBGGRR. Forwarded to the display processor as the
 *                               chroma-key color used by the post-weave alpha-recovery pass.
 *                               Pass 0 to let the DP pick a default (magenta).
 * @param display_screen_left    Display top-left X in OS screen coords (from xsysc->info,
 *                               populated by the vendor plug-in iface). 0 = primary.
 *                               Used only on Windows for self-owned window positioning.
 * @param display_screen_top     Display top-left Y in OS screen coords. 0 = primary.
 * @param out_xcn                Output native compositor.
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
                          bool transparent_background,
                          uint32_t chroma_key_color,
                          int32_t display_screen_left,
                          int32_t display_screen_top,
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
 * Set the output rect within the app's window where the shared texture
 * is displayed. Used for canvas-aware Kooima FOV and view sizing.
 *
 * @param xc  GL compositor base.
 * @param x   Left edge in window client-area pixels.
 * @param y   Top edge in window client-area pixels.
 * @param w   Width in pixels.
 * @param h   Height in pixels.
 *
 * @ingroup comp_gl
 */
void
comp_gl_compositor_set_output_rect(struct xrt_compositor *xc,
                                    int32_t x, int32_t y,
                                    uint32_t w, uint32_t h);

/*!
 * Register a full-window 2D shared texture for the surround region (Spec v6).
 *
 * On Windows the handle is an NT HANDLE bridged via WGL_NV_DX_interop2;
 * on POSIX, an EGL image / dma-buf fd. Pass shared_handle == NULL to clear.
 * See comp_d3d11_compositor_set_surround_2d for the cross-platform semantics.
 *
 * @ingroup comp_gl
 */
void
comp_gl_compositor_set_surround_2d(struct xrt_compositor *xc,
                                    void *shared_handle,
                                    uint32_t w, uint32_t h);

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

/*!
 * Get window metrics for adaptive FOV calculation.
 *
 * @param xc          GL compositor base.
 * @param out_metrics Output window metrics.
 * @return true if valid metrics were computed.
 *
 * @ingroup comp_gl
 */
bool
comp_gl_compositor_get_window_metrics(struct xrt_compositor *xc,
                                      struct xrt_window_metrics *out_metrics);

/*!
 * @name #439 Phase 3 — XR_EXT_local_3d_zone authored-mask API (GL leg).
 * GL R8 mask textures; Tier 1/2 only (acquire_rt returns NOT_IMPLEMENTED).
 * @{
 * @ingroup comp_gl
 */
xrt_result_t
comp_gl_compositor_zone_mask_create(struct xrt_compositor *xc, uint32_t w, uint32_t h, void **out_mask);
xrt_result_t
comp_gl_compositor_zone_mask_set_whole(struct xrt_compositor *xc, void *mask, bool enable_3d);
xrt_result_t
comp_gl_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                       void *mask,
                                       uint32_t count,
                                       const struct xrt_rect *rects);
xrt_result_t
comp_gl_compositor_zone_mask_acquire_rt(
    struct xrt_compositor *xc, void *mask, void **out_texture, uint32_t *out_w, uint32_t *out_h);
xrt_result_t
comp_gl_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask);
void
comp_gl_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask);
/*! @} */

/*!
 * Current recommended per-view render size (#439 Phase 3 Q4). Returns false if
 * unavailable.
 *
 * @ingroup comp_gl
 */
bool
comp_gl_compositor_get_recommended_view_size(struct xrt_compositor *xc, uint32_t *out_w, uint32_t *out_h);

#ifdef __cplusplus
}
#endif
