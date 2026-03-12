// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 compositor self-created window management.
 *
 * This module provides window creation for the D3D11 native compositor
 * when XR_EXT_win32_window_binding is NOT used. This allows apps like Blender
 * that don't provide a window handle to still use the D3D11 native compositor.
 *
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
struct comp_d3d11_window;
struct xrt_system_devices;

/*!
 * Create a self-owned window for the D3D11 compositor.
 *
 * This creates a window on a **dedicated thread**. The window thread
 * handles its own message pump via GetMessage/DispatchMessage. The
 * caller does NOT need to pump messages — the compositor thread
 * continues rendering independently during modal drag/resize.
 *
 * The window is automatically positioned on the Leia display if found,
 * or on a secondary monitor, or on the primary display as fallback.
 *
 * By default, the window starts in fullscreen mode unless the environment
 * variable XRT_COMPOSITOR_START_WINDOWED=1 is set.
 *
 * @param width  Requested window width
 * @param height Requested window height
 * @param out    Pointer to receive the created window handle
 *
 * @return XRT_SUCCESS on success, error code otherwise
 */
xrt_result_t
comp_d3d11_window_create(uint32_t width, uint32_t height, struct comp_d3d11_window **out);

/*!
 * Create a hidden window for the SR weaver in shared-texture mode.
 *
 * Same as comp_d3d11_window_create but the window is never shown.
 * The SR weaver needs a valid HWND for interlacing alignment even
 * when rendering to a shared texture instead of a window surface.
 *
 * @param width  Requested window width
 * @param height Requested window height
 * @param out    Pointer to receive the created window handle
 *
 * @return XRT_SUCCESS on success, error code otherwise
 */
xrt_result_t
comp_d3d11_window_create_hidden(uint32_t width, uint32_t height, struct comp_d3d11_window **out);

/*!
 * Destroy the self-owned window.
 *
 * Posts WM_CLOSE to the window thread and waits for it to exit.
 * Can be called from any thread.
 *
 * @param window Pointer to window handle (set to NULL after destruction)
 */
void
comp_d3d11_window_destroy(struct comp_d3d11_window **window);

/*!
 * Get the Win32 HWND handle from the window.
 *
 * @param window The window object
 *
 * @return The HWND handle, or NULL if window creation failed
 */
void *
comp_d3d11_window_get_hwnd(struct comp_d3d11_window *window);

/*!
 * Check if the window is still valid and not closed by user.
 *
 * @param window The window object
 *
 * @return true if window is valid, false if closed or invalid
 */
bool
comp_d3d11_window_is_valid(struct comp_d3d11_window *window);

/*!
 * Get the current dimensions of the window.
 *
 * @param window     The window object
 * @param out_width  Pointer to receive current width
 * @param out_height Pointer to receive current height
 */
void
comp_d3d11_window_get_dimensions(struct comp_d3d11_window *window,
                                  uint32_t *out_width,
                                  uint32_t *out_height);

/*!
 * Check if the window is currently inside a modal move/size loop.
 *
 * Returns true between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE. Callers
 * should defer expensive operations (swapchain resize, texture
 * reallocation) until the drag finishes.
 *
 * @param window The window object
 *
 * @return true if the user is currently dragging/resizing the window
 */
bool
comp_d3d11_window_is_in_size_move(struct comp_d3d11_window *window);

/*!
 * Wait for a WM_PAINT request during drag. Returns true if compositor
 * should render (drag in progress), false if drag ended.
 * Only blocks during modal size-move. Returns false immediately otherwise.
 *
 * @param window The window object
 *
 * @return true if drag is in progress and compositor should render
 */
bool
comp_d3d11_window_wait_for_paint(struct comp_d3d11_window *window);

/*!
 * Signal that the compositor has finished rendering and presenting.
 * Unblocks the WM_PAINT handler so the modal drag loop can continue.
 *
 * @param window The window object
 */
void
comp_d3d11_window_signal_paint_done(struct comp_d3d11_window *window);

/*!
 * Reposition and resize the window's client area.
 *
 * Used in shared-texture mode to keep the hidden weaver window aligned
 * with the app's actual display rect on screen. The coordinates are
 * screen-space pixels for the desired client area. For WS_POPUP windows
 * the client rect equals the window rect, so this is a direct SetWindowPos.
 *
 * Thread-safe: posts to the window thread.
 *
 * @param window The window object
 * @param x      Client area left in screen pixels
 * @param y      Client area top in screen pixels
 * @param w      Client area width in pixels
 * @param h      Client area height in pixels
 */
void
comp_d3d11_window_set_rect(struct comp_d3d11_window *window,
                           int x, int y, uint32_t w, uint32_t h);

/*!
 * No-op. The dedicated window thread handles its own messages.
 *
 * Retained for API compatibility. Callers do not need to pump messages
 * for the self-owned window.
 *
 * @param window The window object
 */
void
comp_d3d11_window_pump_messages(struct comp_d3d11_window *window);

/*!
 * No-op. With the dedicated window thread, the compositor thread
 * continues rendering during drag/resize without interruption.
 *
 * Retained for API compatibility.
 *
 * @param window    The window object
 * @param callback  Ignored
 * @param userdata  Ignored
 */
void
comp_d3d11_window_set_repaint_callback(struct comp_d3d11_window *window,
                                        void (*callback)(void *userdata),
                                        void *userdata);

/*!
 * Set the system devices for qwerty input handling.
 *
 * When set, the window will forward keyboard/mouse input to the qwerty driver.
 * This allows direct input from the D3D11 window without requiring the SDL debug GUI.
 *
 * @param window The window object
 * @param xsysd  The system devices (can be NULL to disable input handling)
 */
void
comp_d3d11_window_set_system_devices(struct comp_d3d11_window *window,
                                      struct xrt_system_devices *xsysd);

#ifdef __cplusplus
}
#endif
