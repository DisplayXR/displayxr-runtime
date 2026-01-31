// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 compositor self-created window management.
 *
 * This module provides window creation for the D3D11 native compositor
 * when XR_EXT_session_target is NOT used. This allows apps like Blender
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

// Forward declaration - opaque handle to window management
struct comp_d3d11_window;

/*!
 * Create a self-owned window for the D3D11 compositor.
 *
 * This creates a window on the calling thread. The caller must pump
 * messages via @ref comp_d3d11_window_pump_messages or its own
 * PeekMessage loop. The window is automatically positioned on the Leia
 * display if found, or on a secondary monitor, or on the primary display
 * as fallback.
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
 * Destroy the self-owned window.
 *
 * Must be called from the thread that created the window.
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
 * Pump Win32 messages for the window (non-blocking).
 *
 * This dispatches pending messages for the window. The caller may also
 * use its own PeekMessage(NULL, ...) loop which will dispatch messages
 * for all windows on the thread, including this one.
 *
 * @param window The window object
 */
void
comp_d3d11_window_pump_messages(struct comp_d3d11_window *window);

/*!
 * Set a callback invoked from WM_PAINT during drag/resize.
 *
 * During a modal move/size loop the normal render loop cannot run.
 * This callback allows the compositor to re-present the last frame
 * so the window contents stay up-to-date.
 *
 * @param window    The window object
 * @param callback  Function to call (NULL to clear)
 * @param userdata  Opaque pointer forwarded to @p callback
 */
void
comp_d3d11_window_set_repaint_callback(struct comp_d3d11_window *window,
                                        void (*callback)(void *userdata),
                                        void *userdata);

#ifdef __cplusplus
}
#endif
