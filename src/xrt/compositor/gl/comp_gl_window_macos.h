// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS window helper for the native GL compositor.
 *
 * Provides NSWindow/NSOpenGLView management for OpenGL presentation on macOS.
 * Modeled on the VK native compositor's comp_vk_native_window_macos pattern.
 *
 * @author David Fattal
 * @ingroup comp_gl
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_gl_window_macos;

/*!
 * Create a self-owned NSWindow with an NSOpenGLView.
 *
 * @param width      Requested window width in points.
 * @param height     Requested window height in points.
 * @param app_cgl_ctx  App's CGLContextObj for share-group texture sharing (may be NULL).
 * @param out_win    Pointer to receive the created window handle.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 */
xrt_result_t
comp_gl_window_macos_create(uint32_t width,
                            uint32_t height,
                            void *app_cgl_ctx,
                            struct comp_gl_window_macos **out_win);

/*!
 * Set up presentation on an app-provided NSView.
 *
 * Creates an NSOpenGLContext for the view, optionally sharing textures
 * with the app's CGL context.
 *
 * @param ns_view      The app's NSView (as void*).
 * @param app_cgl_ctx  App's CGLContextObj for share-group (may be NULL).
 * @param out_win      Pointer to receive the window handle.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 */
xrt_result_t
comp_gl_window_macos_setup_external(void *ns_view,
                                    void *app_cgl_ctx,
                                    struct comp_gl_window_macos **out_win);

/*!
 * Create an offscreen GL context (no window) for headless rendering.
 *
 * Used for shared-texture mode where the compositor renders into an
 * IOSurface rather than presenting to a window.
 *
 * @param app_cgl_ctx  App's CGLContextObj for share-group texture sharing (may be NULL).
 * @param out_win      Pointer to receive the handle.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 */
xrt_result_t
comp_gl_window_macos_create_offscreen(void *app_cgl_ctx,
                                       struct comp_gl_window_macos **out_win);

/*!
 * Get the CGLContextObj for making current.
 *
 * @param win The window handle.
 *
 * @return The CGLContextObj as void*, or NULL.
 */
void *
comp_gl_window_macos_get_cgl_context(struct comp_gl_window_macos *win);

/*!
 * Make the compositor's GL context current.
 *
 * @param win The window handle.
 *
 * @return true if context was made current successfully.
 */
bool
comp_gl_window_macos_make_current(struct comp_gl_window_macos *win);

/*!
 * Swap buffers (present the rendered frame).
 *
 * @param win The window handle.
 */
void
comp_gl_window_macos_swap_buffers(struct comp_gl_window_macos *win);

/*!
 * Map an IOSurface to a GL_TEXTURE_RECTANGLE in the compositor's context.
 *
 * @param win                The window/context handle.
 * @param iosurface_handle   IOSurfaceRef as void*.
 * @param out_gl_texture     Receives the GL texture name.
 * @param out_width          Receives the IOSurface width.
 * @param out_height         Receives the IOSurface height.
 *
 * @return XRT_SUCCESS on success.
 */
xrt_result_t
comp_gl_window_macos_map_iosurface(struct comp_gl_window_macos *win,
                                    void *iosurface_handle,
                                    uint32_t *out_gl_texture,
                                    uint32_t *out_width,
                                    uint32_t *out_height);

/*!
 * Get the current backing pixel dimensions.
 *
 * @param win        The window handle.
 * @param out_width  Pointer to receive width in pixels.
 * @param out_height Pointer to receive height in pixels.
 */
void
comp_gl_window_macos_get_dimensions(struct comp_gl_window_macos *win,
                                    uint32_t *out_width,
                                    uint32_t *out_height);

/*!
 * Check if the window is still valid (not closed by user).
 *
 * @param win The window handle.
 *
 * @return true if the window is still open.
 */
bool
comp_gl_window_macos_is_valid(struct comp_gl_window_macos *win);

/*!
 * Destroy the window helper and release resources.
 *
 * @param win_ptr Pointer to window handle (set to NULL after destruction).
 */
void
comp_gl_window_macos_destroy(struct comp_gl_window_macos **win_ptr);

/*!
 * Update the HUD overlay text (only for self-owned windows).
 *
 * Dispatches to main thread for NSView update.
 *
 * @param win  The window handle.
 * @param text The HUD text to display.
 */
void
comp_gl_window_macos_update_hud(struct comp_gl_window_macos *win, const char *text);

/*!
 * Hide the HUD overlay (only for self-owned windows).
 *
 * @param win The window handle.
 */
void
comp_gl_window_macos_hide_hud(struct comp_gl_window_macos *win);

#ifdef __cplusplus
}
#endif
