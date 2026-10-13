// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux twin of the Windows workspace-session window hide.
 *
 * See oxr_workspace_window_linux.c for the full rationale.
 *
 * @ingroup oxr_main
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Called once per xrCreateSession on desktop Linux, before the session is
 * created, with whatever app-window binding the app chained.
 *
 * @param x_display          XrXlibWindowBindingCreateInfoDXR::xDisplay, or NULL.
 * @param x_window           XrXlibWindowBindingCreateInfoDXR::window, or 0.
 * @param have_wayland       True when an XrWaylandSurfaceBindingCreateInfoDXR
 *                           with a non-NULL wl_surface was chained.
 * @param workspace_session  `DISPLAYXR_WORKSPACE_SESSION=1` AND the system
 *                           compositor reports an active workspace — the same
 *                           gate as the Windows `ShowWindow(SW_HIDE)` site.
 *
 * Workspace session + X11 window: the window is withdrawn (unmapped) so the
 * app's own window never shows — the service composites its swapchain into the
 * shared spatial surface instead. Standalone session on a window this module
 * previously withdrew (the app recreated its session after the workspace went
 * away): the window is mapped again. Wayland: logged only, see the .c file.
 */
void
oxr_workspace_linux_app_window_on_session_create(void *x_display,
                                                 unsigned long x_window,
                                                 bool have_wayland,
                                                 bool workspace_session);

#ifdef __cplusplus
}
#endif
