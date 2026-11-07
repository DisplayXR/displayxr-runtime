// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime's own X11 window declared full side by side: following
 *         the window manager's doubling. Header-only pure logic.
 *
 * A window declared full side by side (`_KDE_NET_WM_STEREO_CONTENT` = 3) on a
 * window manager that announces version 3 or later of the declaration
 * (`_KDE_NET_WM_STEREO_CONTENT_SUPPORTED` on the root window; Stereo KWin) keeps
 * its place on screen, one view, while the window manager makes the X window
 * hold both views: twice its width. The window manager does so when it reads
 * the declaration, which can be seconds after the client wrote it.
 *
 * Vulkan creates an X11 swapchain only at the window's current size (Vulkan
 * 1.3, VK_KHR_xlib_surface / VK_KHR_xcb_surface: "it is only possible to create
 * a new swapchain on this platform with imageExtent being equal to the current
 * size of the window"), so the runtime presents both views only once the X
 * window holds them, and until then the window's own size.
 *
 * The X window's size is the one in the X server's ConfigureNotify. While the
 * window is declared, a synthetic ConfigureNotify (ICCCM 4.1.5, sent by the
 * window manager) gives a stereo window manager's size of the window in the
 * program's own units: one view. It is never the X window's size then, and it
 * tells that the window manager holds the window at both views' width when it
 * is half the X window's width. Undeclared, every ConfigureNotify counts as
 * before.
 *
 * @ingroup comp_vk_native
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A window manager announcing this version makes a declared window hold both views.
#define COMP_VK_NATIVE_XCB_FULL_SBS_MIN_SUPPORTED 3u

//! `_KDE_NET_WM_STEREO_CONTENT` value for full side by side, left view first.
#define COMP_VK_NATIVE_XCB_STEREO_CONTENT_SBS_FULL 3u

struct comp_vk_native_xcb_full_sbs
{
	//! `_KDE_NET_WM_STEREO_CONTENT` = 3 is set on the window.
	bool declared;
	//! The window manager holds the declared window at both views' width.
	bool doubled;
	//! The X window's width when the declaration was written: one view.
	uint32_t view_width;
	//! The X window's own size, from the X server's ConfigureNotify.
	uint32_t width;
	uint32_t height;
};

/*!
 * The window was declared (@p declare true) or the declaration removed. The X
 * window's current size is @ref comp_vk_native_xcb_full_sbs::width.
 */
static inline void
comp_vk_native_xcb_full_sbs_set_declared(struct comp_vk_native_xcb_full_sbs *s, bool declare)
{
	s->declared = declare;
	s->doubled = false;
	s->view_width = declare ? s->width : 0;
}

/*!
 * A ConfigureNotify for the window: @p synthetic when it came from SendEvent
 * (the response type's top bit). Returns true when this event made the window
 * manager's doubling known.
 */
static inline bool
comp_vk_native_xcb_full_sbs_configure(struct comp_vk_native_xcb_full_sbs *s,
                                      bool synthetic,
                                      uint32_t width,
                                      uint32_t height)
{
	if (width == 0 || height == 0) {
		return false;
	}
	if (s->declared && synthetic) {
		// One view, in the program's own units: never the X window's size.
		if (!s->doubled && 2u * width == s->width) {
			s->doubled = true;
			return true;
		}
		return false;
	}
	s->width = width;
	s->height = height;
	if (s->declared && !s->doubled && s->view_width > 0 && width == 2u * s->view_width) {
		s->doubled = true;
		return true;
	}
	return false;
}

/*!
 * The window's size for everything but the swapchain: one view while the
 * window manager holds the window at both views' width, else the X window's.
 */
static inline void
comp_vk_native_xcb_full_sbs_view_size(const struct comp_vk_native_xcb_full_sbs *s,
                                      uint32_t *out_width,
                                      uint32_t *out_height)
{
	*out_width = (s->declared && s->doubled) ? s->width / 2u : s->width;
	*out_height = s->height;
}

#ifdef __cplusplus
}
#endif
