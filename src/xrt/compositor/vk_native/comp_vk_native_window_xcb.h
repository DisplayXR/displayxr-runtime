// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XCB (X11) window helper for the VK native compositor on Linux.
 *
 * Provides a runtime self-created X11 window for the `_hosted` app class plus a
 * VK_KHR_xcb_surface present path. Modeled on comp_vk_native_window_macos
 * (NSWindow/CAMetalLayer), but XCB needs BOTH an xcb_connection_t* and an
 * xcb_window_t to build a Vulkan surface, so the helper owns the connection and
 * exposes both through @ref comp_vk_native_xcb_handle (handed to the target as
 * the type-erased `hwnd`). See docs/roadmap/linux-support.md (Phase 1).
 *
 * Phase 1 covers the hosted class only (runtime self-creates the window). The
 * handle/texture classes — where the app supplies its own X11 window — need the
 * still-to-be-defined XR_DXR_xlib_window_binding extension (Phase 3).
 *
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Type-erased XCB surface handle passed to comp_vk_native_target_create as
 * `hwnd`. Unlike every other platform (a single pointer), vkCreateXcbSurfaceKHR
 * needs both a connection and a window id, so the target reads both from here.
 *
 * `connection` is an `xcb_connection_t *` and `window` an `xcb_window_t`; kept
 * as a void pointer and a uint32_t so includers don't need the xcb headers.
 */
struct comp_vk_native_xcb_handle
{
	void *connection;
	uint32_t window;
};

/*!
 * App-provided Xlib window (XR_DXR_xlib_window_binding, Phase 3), passed
 * type-erased as the `hwnd` param of comp_vk_native_compositor_create. The
 * compositor copies the fields synchronously during create, so the caller may
 * stack-allocate this.
 *
 * `display` is an Xlib `Display *` and `window` a `Window` (XID); kept as a
 * void pointer and an unsigned long so includers don't need the Xlib headers.
 */
struct comp_vk_native_xlib_handle
{
	void *display;
	unsigned long window;
};

struct comp_vk_native_window_xcb;

/*!
 * Wrap an app-provided Xlib window (XR_DXR_xlib_window_binding) into the XCB
 * surface handle the target consumes. Derives the XCB connection from the Xlib
 * display via XGetXCBConnection() (libX11-xcb) — the connection is borrowed
 * from (and owned by) the app's Display, so nothing is destroyed on teardown.
 *
 * @param xdisplay   Xlib `Display *` supplied by the app.
 * @param xwindow    X11 `Window` (XID) supplied by the app.
 * @param out_handle Receives the connection + window id for the VK target.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 */
xrt_result_t
comp_vk_native_window_xcb_wrap_app_window(void *xdisplay,
                                          unsigned long xwindow,
                                          struct comp_vk_native_xcb_handle *out_handle);

/*!
 * Query the current pixel size of any window reachable through @p handle via
 * xcb_get_geometry (works for app-provided windows, which have no
 * comp_vk_native_window_xcb helper tracking ConfigureNotify).
 *
 * @return true if the geometry could be queried.
 */
bool
comp_vk_native_window_xcb_query_geometry(const struct comp_vk_native_xcb_handle *handle,
                                         uint32_t *out_width,
                                         uint32_t *out_height);

/*!
 * Create a self-owned X11 window (hosted class).
 *
 * @param width                  Requested window width in pixels.
 * @param height                 Requested window height in pixels.
 * @param screen_left            Window left edge in root-window pixels — the
 *                               3D panel position the vendor plug-in reports
 *                               via xsysc->info.display_screen_left. (0, 0)
 *                               means primary monitor (Windows convention,
 *                               comp_d3d11_window.cpp). #715.
 *
 *                               These coordinates now also resolve the target
 *                               RandR MONITOR (the monitor whose origin matches,
 *                               else the one containing the point), and the
 *                               window is fullscreened onto it via EWMH
 *                               _NET_WM_FULLSCREEN_MONITORS. That is the
 *                               WM-cooperative, size-independent placement path:
 *                               mutter/GNOME discard the create-x/y +
 *                               USPosition + ConfigureRequest for an oversized
 *                               toplevel, so raw coordinates alone don't place
 *                               the window (#715). Set DXR_WINDOW_FULLSCREEN=0
 *                               to opt out and keep plain windowed placement.
 * @param screen_top             Window top edge in root-window pixels.
 * @param transparent_background Reserved; X11 ARGB-visual transparency is not
 *                               wired in Phase 1 (desktop WSI usually exposes
 *                               only OPAQUE compositeAlpha). Accepted for API
 *                               parity with the macOS/Windows helpers.
 * @param out_win                Receives the created window handle.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 */
xrt_result_t
comp_vk_native_window_xcb_create(uint32_t width,
                                 uint32_t height,
                                 int32_t screen_left,
                                 int32_t screen_top,
                                 bool transparent_background,
                                 struct comp_vk_native_window_xcb **out_win);

/*!
 * Fill in the surface handle (connection + window) for VkSurfaceKHR creation.
 *
 * The pointed-to struct must outlive the target (store it in the compositor),
 * since the target borrows the connection for the lifetime of the surface.
 */
void
comp_vk_native_window_xcb_get_handle(struct comp_vk_native_window_xcb *win,
                                     struct comp_vk_native_xcb_handle *out_handle);

/*!
 * Get the current window size in pixels (live, tracks resize via the last
 * ConfigureNotify; falls back to the created size).
 */
void
comp_vk_native_window_xcb_get_dimensions(struct comp_vk_native_window_xcb *win,
                                         uint32_t *out_width,
                                         uint32_t *out_height);

/*!
 * Get the window's top-left position in root-window (screen) pixels, top-down.
 * Drives window-relative 3D for the display processor's interlacing phase.
 *
 * @return true if the position could be determined.
 */
bool
comp_vk_native_window_xcb_get_screen_position(struct comp_vk_native_window_xcb *win,
                                              int32_t *out_left_px,
                                              int32_t *out_top_px);

/*!
 * Pump pending X events (resize tracking + WM_DELETE_WINDOW close detection).
 * Cheap; safe to call once per frame.
 */
void
comp_vk_native_window_xcb_pump(struct comp_vk_native_window_xcb *win);

/*!
 * Check whether the window is still open (false once the user closes it).
 */
bool
comp_vk_native_window_xcb_is_valid(struct comp_vk_native_window_xcb *win);

/*!
 * Destroy the window helper and disconnect (sets *win_ptr to NULL).
 */
void
comp_vk_native_window_xcb_destroy(struct comp_vk_native_window_xcb **win_ptr);

#ifdef __cplusplus
}
#endif
