// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux present target for the out-of-process service compositor.
 * @ingroup comp_main
 *
 * The desktop-Linux twin of @ref comp_window_macos (#710, #967): a
 * @ref comp_target_swapchain subclass that presents to a runtime-owned window
 * inside the service process. The service owns the window (the hosted model)
 * because no app window is passed across the process boundary on this path —
 * it is the ONE full-screen surface the comp_multi shared spatial surface
 * composites every client into.
 *
 * Two backends, picked at create time (DXR_SERVICE_WINDOW_BACKEND=x11|wayland
 * overrides):
 *
 * - **Native Wayland** (comp_window_linux_wayland.c, the default when a
 *   compositor offers xdg-shell + wp_viewporter + wp_fractional_scale_v1): an
 *   xdg_toplevel fullscreened on the panel's wl_output, a device-pixel buffer
 *   mapped 1:1 through the viewport, an opaque region for direct scanout. The
 *   only 1:1 path on a fractionally scaled GNOME desktop — XWayland windows are
 *   resampled there, which breaks the weave.
 * - **X11** (comp_window_linux.c; Xorg, or XWayland as a fallback): fullscreen
 *   on the panel's RandR monitor with the WM-cooperative recipe validated for
 *   the in-process hosted window (comp_vk_native_window_xcb.c, #715).
 *
 * Input: the window owns a small event thread. Keyboard / pointer / scroll
 * events on the surface are decoded into @ref comp_window_linux_input (keys
 * as Windows virtual-key codes, the code space the workspace controller
 * expects on every platform) and handed to the process-wide sink installed
 * with @ref comp_window_linux_set_input_sink — the IPC server's workspace
 * input router. The sink is called on the event thread with no compositor
 * lock held.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_compositor;
struct comp_target;

/*!
 * Where the service surface goes, passed type-erased as the
 * `external_window_handle` of comp_target_service_create on desktop Linux (no
 * app window crosses the process boundary on this path, so the slot carries
 * the placement instead). Values come from the multi system compositor's
 * xrt_system_compositor_info, which the target instance fills from the active
 * plug-in after the null compositor is created. The target copies the fields
 * synchronously, so the caller may stack-allocate this.
 */
struct comp_window_linux_placement
{
	int32_t screen_left; //!< 3D panel top-left, virtual-desktop px
	int32_t screen_top;
	uint32_t width; //!< Panel size in px, 0 = unknown (the RandR monitor size wins when it resolves)
	uint32_t height;
};

/*!
 * Create a desktop-Linux present target. @p c is the owning compositor (in the
 * service a @ref null_compositor up-cast to @ref comp_compositor — both start
 * with @ref comp_base, so @p c->base.vk is valid). @p place (may be NULL =
 * the monitor at the desktop origin) picks the output the window is
 * fullscreened on; the target copies it.
 *
 * The window-system connection, window and VkSurfaceKHR are created in
 * @ref comp_target::init_post_vulkan, which also settles the size the first
 * swapchain is created at (X11: the WM's fullscreen geometry; Wayland: the
 * panel output's mode in device pixels).
 *
 * @return the target's @ref comp_target base, or NULL on allocation failure.
 *
 * @ingroup comp_main
 */
struct comp_target *
comp_window_linux_create(struct comp_compositor *c, const struct comp_window_linux_placement *place);

/*!
 * Map (show) or unmap (hide) the full-screen surface window. The service is a
 * persistent daemon: with no workspace controller and no content client the
 * surface is hidden so the normal desktop returns, and shown again when one
 * connects. Mirrors comp_window_macos_set_visible. Thread-safe.
 *
 * @ingroup comp_main
 */
void
comp_window_linux_set_visible(struct comp_target *ct, bool visible);

/*!
 * Hide (or restore) the OS pointer over the surface window. The workspace
 * renders its own cursor sprite while a controller has registered one, and
 * the X pointer would double up with it. Thread-safe; a no-op if unchanged.
 *
 * @ingroup comp_main
 */
void
comp_window_linux_set_cursor_hidden(struct comp_target *ct, bool hidden);

/*!
 * Kind of a decoded surface input event.
 *
 * @ingroup comp_main
 */
enum comp_window_linux_input_type
{
	COMP_WINDOW_LINUX_INPUT_KEY = 0,        //!< vk_code / keysym / is_down / modifiers
	COMP_WINDOW_LINUX_INPUT_BUTTON = 1,     //!< button / is_down / x / y / modifiers
	COMP_WINDOW_LINUX_INPUT_MOTION = 2,     //!< x / y / button_mask / modifiers
	COMP_WINDOW_LINUX_INPUT_SCROLL = 3,     //!< scroll_delta / x / y / modifiers
	COMP_WINDOW_LINUX_INPUT_FOCUS_LOST = 4, //!< the surface lost keyboard focus
};

/*!
 * One decoded input event on the service surface window.
 *
 * Coordinates are window-local framebuffer pixels, top-left origin — with the
 * window fullscreen on the panel, panel pixels, the space
 * comp_multi_workspace_hit_test_window_px and the workspace cursor composite
 * use.
 *
 * @ingroup comp_main
 */
struct comp_window_linux_input
{
	enum comp_window_linux_input_type type;
	uint32_t timestamp_ms; //!< X server timestamp (ms, wraps)
	int32_t x, y;          //!< window-local px (BUTTON / MOTION / SCROLL; last known for KEY)
	uint32_t vk_code;      //!< KEY: Windows virtual-key code, 0 if the keysym has none
	uint32_t keysym;       //!< KEY: X11 keysym at shift level 0 (for diagnostics)
	bool is_down;          //!< KEY / BUTTON
	uint32_t button;       //!< BUTTON: 1 = left, 2 = right, 3 = middle (IPC wire numbering)
	uint32_t button_mask;  //!< MOTION: bit0 = left, bit1 = right, bit2 = middle held
	float scroll_delta;    //!< SCROLL: wheel notches, positive = up / away from the user
	uint32_t modifiers;    //!< bit0 = SHIFT, bit1 = CTRL, bit2 = ALT (IPC wire layout)
};

/*!
 * Receives decoded surface input. Called on the window's event thread.
 */
typedef void (*comp_window_linux_input_fn)(void *userdata, const struct comp_window_linux_input *ev);

/*!
 * Install (or clear, with NULL) the process-wide input sink. There is one
 * service surface per process, so the sink is global; it may be installed
 * before the window exists. Thread-safe.
 *
 * @ingroup comp_main
 */
void
comp_window_linux_set_input_sink(comp_window_linux_input_fn fn, void *userdata);

/*!
 * Map an X11 keysym (shift level 0) to a Windows virtual-key code, 0 if it has
 * none. Exposed for unit tests.
 *
 * @ingroup comp_main
 */
uint32_t
comp_window_linux_keysym_to_vk(uint32_t keysym);

#ifdef __cplusplus
}
#endif
