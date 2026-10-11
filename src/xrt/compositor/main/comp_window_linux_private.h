// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Internals shared by the desktop-Linux service window backends.
 * @ingroup comp_main
 *
 * comp_window_linux has two backends behind one public API
 * (comp_window_linux.h): X11 (Xorg or XWayland, comp_window_linux.c) and native
 * Wayland (comp_window_linux_wayland.c). Both targets begin with
 * @ref comp_window_linux_base, so the public toggles dispatch through @ref ops.
 */

#pragma once

#include "main/comp_target_swapchain.h"
#include "main/comp_window_linux.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_compositor;

//! Per-backend implementations of the public toggles.
struct comp_window_linux_ops
{
	void (*set_visible)(struct comp_target *ct, bool visible);
	void (*set_cursor_hidden)(struct comp_target *ct, bool hidden);
};

/*!
 * Common initial sequence of both backend targets: every backend struct starts
 * with these two members, in this order.
 */
struct comp_window_linux_base
{
	struct comp_target_swapchain base;
	const struct comp_window_linux_ops *ops;
};

//! Hand one decoded event to the process-wide sink (comp_window_linux.c).
void
comp_window_linux_emit_input(const struct comp_window_linux_input *ev);

//! The X11 backend (comp_window_linux.c).
struct comp_target *
comp_window_linux_x11_create(struct comp_compositor *c, const struct comp_window_linux_placement *place);

#ifdef XRT_HAVE_COMP_LINUX_WINDOW_WAYLAND
/*!
 * Is a native-Wayland service window possible here: a compositor answers on
 * WAYLAND_DISPLAY and advertises xdg_wm_base, wp_viewporter and
 * wp_fractional_scale_v1 (the 1:1 mapping on a scaled output needs both).
 * Logs the verdict once.
 */
bool
comp_window_linux_wayland_available(void);

//! The native-Wayland backend (comp_window_linux_wayland.c).
struct comp_target *
comp_window_linux_wayland_create(struct comp_compositor *c, const struct comp_window_linux_placement *place);

/*!
 * evdev keycode (linux/input-event-codes.h) to a Windows virtual-key code, by
 * US-layout key position; 0 if none. Exposed for unit tests.
 */
uint32_t
comp_window_linux_evdev_to_vk(uint32_t evdev_code);
#endif

#ifdef __cplusplus
}
#endif
