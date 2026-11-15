// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime's WS_CHILD present window, for an app window that
 *         already has a DXGI swapchain of its own.
 *
 * DXGI allows one flip-model swapchain per HWND. When the app's window already
 * carries one (an app or injector that renders into its own window and also
 * binds it through XR_DXR_win32_window_binding), CreateSwapChainForHwnd on it
 * fails with E_ACCESSDENIED. The targets then present into a WS_CHILD window of
 * the app's window instead, placed at (0,0) of its client area and sized to it,
 * so the child's client origin and size ARE the app window's client origin and
 * size: anything that anchors on the app's window (the display processor's
 * position/phase anchor, canvas metrics) stays correct without knowing.
 *
 * The child lives on its own thread with its own message loop. Otherwise DXGI
 * Present() deadlocks when the parent enters a modal move/size loop: the
 * parent's UI thread is blocked and Present() sends a message to the window.
 *
 * Shared by the D3D11 target (the hub and the native D3D11 leg) and the D3D12
 * own-legs target. Nothing here moves, resizes, restyles or reorders the app's
 * window, and the child is never topmost.
 *
 * @ingroup comp_util
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_child_window_win;

/*!
 * Create the child window of @p parent_hwnd (an HWND) on a dedicated thread,
 * at (0,0) and @p width x @p height. Pumps sent messages while it waits (the
 * creation can send to the parent's thread), bounded at 5 s.
 *
 * @return The child, or NULL on failure (logged).
 */
struct comp_child_window_win *
comp_child_window_win_create(void *parent_hwnd, uint32_t width, uint32_t height);

//! The child's HWND (never NULL for a created child).
void *
comp_child_window_win_hwnd(const struct comp_child_window_win *child);

//! Resize the child to @p width x @p height at (0,0). NULL child = no-op.
void
comp_child_window_win_resize(struct comp_child_window_win *child, uint32_t width, uint32_t height);

/*!
 * Close the child and join its thread (bounded). Release every swapchain bound
 * to the child first. NULL / *NULL = no-op; sets *@p child_ptr to NULL.
 */
void
comp_child_window_win_destroy(struct comp_child_window_win **child_ptr);

#ifdef __cplusplus
}
#endif
