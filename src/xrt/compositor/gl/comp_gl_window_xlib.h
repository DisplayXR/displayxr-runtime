// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Xlib/GLX window helper for the native GL compositor on desktop Linux.
 *
 * Mirrors comp_gl_window_macos (CGL/NSOpenGLView) and the VK native
 * compositor's comp_vk_native_window_xcb pattern, but for OpenGL via GLX. The
 * runtime self-creates an X11 window and a GLX context that SHARES the app's
 * GLX context (so the compositor samples the app's swapchain textures directly
 * — no external-memory FD interop, which is what makes the software (llvmpipe)
 * path viable where the Vulkan native path needs external_fd support).
 *
 * The app's Display* and GLXContext arrive through the OpenXR Xlib GL binding
 * (XrGraphicsBindingOpenGLXlibKHR); GLX context sharing requires both contexts
 * live on the SAME Display, so this helper BORROWS the app's Display (it does
 * not open or close its own connection).
 *
 * @ingroup comp_gl
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_gl_window_xlib;

/*!
 * Create the compositor's presentation target: a GLX context that shares
 * textures with the app's GLX context, presenting into either a self-created
 * X11 window (hosted class) or the app's own window (handle class).
 *
 * @param app_display     The app's Display* (from XrGraphicsBindingOpenGLXlibKHR.xDisplay).
 * @param app_glx_context The app's GLXContext for share-group texture sharing (may be NULL).
 * @param app_window      The app's X11 Window (XID) to render into (handle class,
 *                        XR_EXT_xlib_window_binding). 0 (None) → self-create a
 *                        window (hosted class). When provided, the window is NOT
 *                        mapped/renamed here and NOT destroyed on teardown — the
 *                        app owns its lifecycle.
 * @param width           Requested window width in pixels (ignored for handle class).
 * @param height          Requested window height in pixels (ignored for handle class).
 * @param out_win         Pointer to receive the created window handle.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 */
xrt_result_t
comp_gl_window_xlib_create(void *app_display,
                           void *app_glx_context,
                           unsigned long app_window,
                           uint32_t width,
                           uint32_t height,
                           struct comp_gl_window_xlib **out_win);

/*!
 * Get the compositor's GLXContext (as void*) for making current.
 */
void *
comp_gl_window_xlib_get_glx_context(struct comp_gl_window_xlib *win);

/*!
 * Make the compositor's GL context current on the compositor's window drawable.
 *
 * @return true if the context was made current successfully.
 */
bool
comp_gl_window_xlib_make_current(struct comp_gl_window_xlib *win);

/*!
 * Swap buffers (present the rendered frame) via glXSwapBuffers.
 */
void
comp_gl_window_xlib_swap_buffers(struct comp_gl_window_xlib *win);

/*!
 * Snapshot the currently-current GLX context/drawable/display, so the compositor
 * can restore the app's context after doing its own GL work. Kept here (not in
 * the compositor TU) so GLX/Xlib headers stay out of comp_gl_compositor.cpp.
 * Values are opaque: dpy/ctx as void*, drawable as unsigned long (GLXDrawable).
 */
void
comp_gl_window_xlib_save_current(void **out_dpy, unsigned long *out_drawable, void **out_ctx);

/*!
 * Restore a context snapshot from comp_gl_window_xlib_save_current (no-op if the
 * snapshot had no current context).
 */
void
comp_gl_window_xlib_restore_current(void *dpy, unsigned long drawable, void *ctx);

/*!
 * Get the current window pixel dimensions.
 */
void
comp_gl_window_xlib_get_dimensions(struct comp_gl_window_xlib *win,
                                   uint32_t *out_width,
                                   uint32_t *out_height);

/*!
 * Get the window's on-screen position in root (screen) coordinates, for the
 * display processor's interlace phase (#524 parity with the macOS/VK helpers).
 *
 * @return true if the position could be determined.
 */
bool
comp_gl_window_xlib_get_screen_position(struct comp_gl_window_xlib *win,
                                        int32_t *out_left_px,
                                        int32_t *out_top_px);

/*!
 * Check if the window is still valid (not closed by the user / WM).
 */
bool
comp_gl_window_xlib_is_valid(struct comp_gl_window_xlib *win);

//! Callback for a captured input XEvent (passed as void* to avoid Xlib headers
//! in the caller). @p ctx is the value handed to comp_gl_window_xlib_pump_input.
typedef void (*comp_gl_window_xlib_input_cb)(void *ctx, void *xevent);

/*!
 * Drain this window's pending keyboard/mouse events and hand each to @p cb.
 * Only events for the compositor's own window are pulled (the app's Display is
 * shared), so the app's own events are never consumed. Call once per frame.
 */
void
comp_gl_window_xlib_pump_input(struct comp_gl_window_xlib *win,
                               comp_gl_window_xlib_input_cb cb,
                               void *ctx);

/*!
 * Destroy the window helper and release resources. Does NOT close the borrowed
 * app Display connection.
 *
 * @param win_ptr Pointer to window handle (set to NULL after destruction).
 */
void
comp_gl_window_xlib_destroy(struct comp_gl_window_xlib **win_ptr);

#ifdef __cplusplus
}
#endif
