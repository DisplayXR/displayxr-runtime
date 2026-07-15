// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XCB (X11) window helper for the VK native compositor on Linux.
 *
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_window_xcb.h"

#include "util/u_misc.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>    // xcb_randr_get_monitors — resolve the target monitor
                          // INDEX from the plug-in-reported panel position (#715).
#include <X11/Xlib-xcb.h> // XGetXCBConnection (libX11-xcb) — Xlib→XCB bridge for
                          // app-provided windows (XR_DXR_xlib_window_binding).

struct comp_vk_native_window_xcb
{
	xcb_connection_t *connection;
	xcb_window_t window;
	xcb_window_t root;

	//! WM_DELETE_WINDOW atom, so a user close is a clean event not an X error.
	xcb_atom_t atom_wm_delete_window;

	//! Colormap allocated for a 32-bit ARGB visual in transparent-background
	//! mode; XCB_NONE for the opaque root-visual path. Freed on destroy.
	xcb_colormap_t colormap;

	//! Live size, seeded at create and updated on ConfigureNotify.
	uint32_t width;
	uint32_t height;

	//! Cleared when the user closes the window (WM_DELETE_WINDOW / DestroyNotify).
	bool valid;
};

static xcb_atom_t
intern_atom(xcb_connection_t *conn, const char *name)
{
	xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, NULL);
	xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return atom;
}

/*!
 * Resolve the RandR monitor INDEX that owns the panel position
 * (@p screen_left, @p screen_top), for the _NET_WM_FULLSCREEN_MONITORS request.
 *
 * mutter (and other EWMH WMs) discard client-requested window geometry for an
 * oversized toplevel, so #716's create-x/y + USPosition + ConfigureRequest are
 * no-ops on GNOME/XWayland — the window lands on whichever monitor happens to
 * fit it (#715, George's DS1 report). Targeting by monitor index instead is
 * WM-cooperative and size-independent.
 *
 * Prefers a monitor whose origin exactly matches (left, top); falls back to the
 * monitor CONTAINING the point (so a `DXR_WINDOW_POS` override still resolves to
 * the right output). @p out_name_atom receives the monitor's RandR name atom for
 * logging (XCB_ATOM_NONE if unavailable).
 *
 * @return the 0-based monitor index, or -1 if RandR is unavailable / no monitor
 *         resolves (caller then skips fullscreen and keeps windowed placement).
 */
static int
resolve_monitor_index(xcb_connection_t *conn,
                      xcb_window_t root,
                      int32_t screen_left,
                      int32_t screen_top,
                      xcb_atom_t *out_name_atom)
{
	if (out_name_atom != NULL) {
		*out_name_atom = XCB_ATOM_NONE;
	}

	xcb_randr_get_monitors_cookie_t cookie = xcb_randr_get_monitors(conn, root, 1 /* active only */);
	xcb_randr_get_monitors_reply_t *reply = xcb_randr_get_monitors_reply(conn, cookie, NULL);
	if (reply == NULL) {
		return -1; // RandR unavailable / too old.
	}

	int exact_idx = -1;
	int contains_idx = -1;
	xcb_atom_t exact_name = XCB_ATOM_NONE;
	xcb_atom_t contains_name = XCB_ATOM_NONE;

	xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(reply);
	for (int idx = 0; it.rem; xcb_randr_monitor_info_next(&it), idx++) {
		const xcb_randr_monitor_info_t *m = it.data;
		if (exact_idx < 0 && m->x == (int16_t)screen_left && m->y == (int16_t)screen_top) {
			exact_idx = idx;
			exact_name = m->name;
		}
		if (contains_idx < 0 && screen_left >= m->x && screen_left < m->x + (int32_t)m->width &&
		    screen_top >= m->y && screen_top < m->y + (int32_t)m->height) {
			contains_idx = idx;
			contains_name = m->name;
		}
	}
	free(reply);

	int chosen = exact_idx >= 0 ? exact_idx : contains_idx;
	if (out_name_atom != NULL) {
		*out_name_atom = exact_idx >= 0 ? exact_name : contains_name;
	}
	return chosen;
}

/*!
 * Find a 32-bit-depth TrueColor (ARGB) visual on @p screen, for transparent
 * desktop composition (XR_DXR_xlib_window_binding transparentBackgroundEnabled).
 * Rendering into an ARGB visual lets the swapchain advertise a non-opaque
 * compositeAlpha so a compositing window manager blends the surface over the
 * desktop. Returns XCB_NONE if the screen exposes no depth-32 TrueColor visual
 * (no ARGB support) — the caller then falls back to the opaque root visual.
 */
static xcb_visualid_t
find_argb_visual(xcb_screen_t *screen)
{
	xcb_depth_iterator_t depth_it = xcb_screen_allowed_depths_iterator(screen);
	for (; depth_it.rem; xcb_depth_next(&depth_it)) {
		if (depth_it.data->depth != 32) {
			continue;
		}
		xcb_visualtype_iterator_t vis_it = xcb_depth_visuals_iterator(depth_it.data);
		for (; vis_it.rem; xcb_visualtype_next(&vis_it)) {
			if (vis_it.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
				return vis_it.data->visual_id;
			}
		}
	}
	return XCB_NONE;
}

xrt_result_t
comp_vk_native_window_xcb_create(uint32_t width,
                                 uint32_t height,
                                 int32_t screen_left,
                                 int32_t screen_top,
                                 bool transparent_background,
                                 struct comp_vk_native_window_xcb **out_win)
{
	if (width == 0) {
		width = 1280;
	}
	if (height == 0) {
		height = 720;
	}

	int screen_num = 0;
	xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
	if (conn == NULL || xcb_connection_has_error(conn)) {
		U_LOG_E("XCB: xcb_connect failed — is DISPLAY set / an X server running?");
		if (conn != NULL) {
			xcb_disconnect(conn);
		}
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Walk to the requested screen.
	const xcb_setup_t *setup = xcb_get_setup(conn);
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < screen_num; i++) {
		xcb_screen_next(&it);
	}
	xcb_screen_t *screen = it.data;
	if (screen == NULL) {
		U_LOG_E("XCB: no screen available");
		xcb_disconnect(conn);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct comp_vk_native_window_xcb *win = U_TYPED_CALLOC(struct comp_vk_native_window_xcb);
	win->connection = conn;
	win->root = screen->root;
	win->width = width;
	win->height = height;
	win->valid = true;

	win->window = xcb_generate_id(conn);

	// Transparent-background mode (XR_DXR_xlib_window_binding
	// transparentBackgroundEnabled): render into a 32-bit ARGB visual so the
	// swapchain can advertise a non-opaque compositeAlpha and a compositing WM
	// (GNOME/Mutter, KWin, picom) blends the surface over the desktop. A window
	// whose depth differs from its parent's must carry its own colormap and an
	// explicit border-pixel or X raises BadMatch, so both go in the value list.
	uint8_t depth = XCB_COPY_FROM_PARENT;
	xcb_visualid_t visual = screen->root_visual;
	uint32_t value_mask;
	uint32_t value_list[4];

	xcb_visualid_t argb_visual = transparent_background ? find_argb_visual(screen) : XCB_NONE;
	if (argb_visual != XCB_NONE) {
		win->colormap = xcb_generate_id(conn);
		xcb_create_colormap(conn, XCB_COLORMAP_ALLOC_NONE, win->colormap, screen->root, argb_visual);

		depth = 32;
		visual = argb_visual;
		// Values must follow the canonical mask-bit order:
		// BACK_PIXEL, BORDER_PIXEL, EVENT_MASK, COLORMAP.
		value_mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
		value_list[0] = 0; // fully-transparent background fill
		value_list[1] = 0; // border pixel (required with a non-parent colormap)
		value_list[2] = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS;
		value_list[3] = win->colormap;
		U_LOG_I("XCB: transparent-background mode — 32-bit ARGB visual 0x%x", (unsigned)argb_visual);
	} else {
		if (transparent_background) {
			U_LOG_W("XCB: transparentBackgroundEnabled requested but no 32-bit ARGB visual is "
			        "available — falling back to an opaque window (is a compositing WM running?)");
		}
		value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
		value_list[0] = screen->black_pixel;
		value_list[1] = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS;
	}

	// Position on the 3D display. The vendor plug-in publishes the panel's
	// top-left in root-window coordinates through
	// xsysc->info.display_screen_left/top and the compositor forwards it here
	// (Windows reference: comp_d3d11_window.cpp). (0, 0) means primary
	// monitor (sim_display default or unknown panel). #715.
	xcb_create_window(conn,
	                  depth,
	                  win->window, screen->root,
	                  (int16_t)screen_left, (int16_t)screen_top, (uint16_t)width, (uint16_t)height,
	                  0,                                 // border
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
	                  visual,
	                  value_mask, value_list);

	// WM_NORMAL_HINTS with USPosition|PPosition, so the window manager treats
	// the create-time position as intentional instead of auto-placing the
	// window (ICCCM §4.1.2.3; GNOME/Mutter auto-places without this).
	{
		uint32_t hints[18] = {0};
		hints[0] = 1 | 4; // USPosition | PPosition
		hints[1] = (uint32_t)screen_left;
		hints[2] = (uint32_t)screen_top;
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win->window, XCB_ATOM_WM_NORMAL_HINTS,
		                    XCB_ATOM_WM_SIZE_HINTS, 32, 18, hints);
	}

	// Title.
	const char *title = "DisplayXR";
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win->window, XCB_ATOM_WM_NAME,
	                    XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);

	// Wire WM_DELETE_WINDOW so the user closing the window is a CLIENT_MESSAGE
	// we can detect (is_valid → false) rather than a fatal X connection error.
	xcb_atom_t wm_protocols = intern_atom(conn, "WM_PROTOCOLS");
	win->atom_wm_delete_window = intern_atom(conn, "WM_DELETE_WINDOW");
	if (wm_protocols != XCB_ATOM_NONE && win->atom_wm_delete_window != XCB_ATOM_NONE) {
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win->window, wm_protocols,
		                    XCB_ATOM_ATOM, 32, 1, &win->atom_wm_delete_window);
	}

	// EWMH fullscreen-on-monitor: the WM-cooperative, size-independent way to
	// place the weave surface on the 3D panel. mutter discards the create-x/y +
	// USPosition + ConfigureRequest above for an oversized toplevel (#715,
	// George's DS1 report), so target the panel's RandR monitor INDEX instead.
	// DXR_WINDOW_FULLSCREEN=0 opts out (debugging on well-behaved WMs).
	int fullscreen_monitor = -1;
	xcb_atom_t monitor_name = XCB_ATOM_NONE;
	const char *fs_env = getenv("DXR_WINDOW_FULLSCREEN");
	const bool fullscreen_enabled = (fs_env == NULL) || (strcmp(fs_env, "0") != 0);
	if (fullscreen_enabled) {
		fullscreen_monitor = resolve_monitor_index(conn, screen->root, screen_left, screen_top, &monitor_name);
		if (fullscreen_monitor < 0) {
			U_LOG_W("XCB: no RandR monitor at (%d, %d) — skipping fullscreen, using windowed placement",
			        (int)screen_left, (int)screen_top);
		} else {
			// Mark fullscreen BEFORE map so the WM manages it fullscreen from
			// the start (EWMH _NET_WM_STATE, set as a property pre-map).
			xcb_atom_t net_wm_state = intern_atom(conn, "_NET_WM_STATE");
			xcb_atom_t net_wm_state_fullscreen = intern_atom(conn, "_NET_WM_STATE_FULLSCREEN");
			if (net_wm_state != XCB_ATOM_NONE && net_wm_state_fullscreen != XCB_ATOM_NONE) {
				xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win->window, net_wm_state,
				                    XCB_ATOM_ATOM, 32, 1, &net_wm_state_fullscreen);
			}
		}
	}

	xcb_map_window(conn, win->window);

	// Re-assert the position after mapping — many WMs (Mutter included)
	// ignore the create-time x/y of a freshly mapped toplevel, but honor a
	// post-map ConfigureRequest (this is what `xdotool windowmove` sends).
	{
		const uint32_t coords[2] = {(uint32_t)screen_left, (uint32_t)screen_top};
		xcb_configure_window(conn, win->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coords);
	}

	// _NET_WM_FULLSCREEN_MONITORS → root: pin the fullscreen window to the
	// resolved monitor index. Single-monitor fullscreen ⇒ all four edges = the
	// target index; data32[4]=1 is source "application" (EWMH). SUBSTRUCTURE_
	// REDIRECT|NOTIFY is the mask the WM listens on for these root messages.
	if (fullscreen_monitor >= 0) {
		xcb_atom_t net_fs_monitors = intern_atom(conn, "_NET_WM_FULLSCREEN_MONITORS");
		if (net_fs_monitors != XCB_ATOM_NONE) {
			xcb_client_message_event_t ev = {0};
			ev.response_type = XCB_CLIENT_MESSAGE;
			ev.format = 32;
			ev.window = win->window;
			ev.type = net_fs_monitors;
			ev.data.data32[0] = (uint32_t)fullscreen_monitor; // top
			ev.data.data32[1] = (uint32_t)fullscreen_monitor; // bottom
			ev.data.data32[2] = (uint32_t)fullscreen_monitor; // left
			ev.data.data32[3] = (uint32_t)fullscreen_monitor; // right
			ev.data.data32[4] = 1;                            // source: application
			xcb_send_event(conn, 0, screen->root,
			               XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
			               (const char *)&ev);
		}
	}
	xcb_flush(conn);

	if (fullscreen_monitor >= 0) {
		// Resolve the RandR monitor name for a human-readable breadcrumb; WARN
		// so it's visible without XRT_LOG=info (parity with the plug-in's #92
		// "RandR panel position overrides backend" line).
		char name_buf[64] = "?";
		if (monitor_name != XCB_ATOM_NONE) {
			xcb_get_atom_name_reply_t *nr =
			    xcb_get_atom_name_reply(conn, xcb_get_atom_name(conn, monitor_name), NULL);
			if (nr != NULL) {
				int n = xcb_get_atom_name_name_length(nr);
				if (n > (int)sizeof(name_buf) - 1) {
					n = (int)sizeof(name_buf) - 1;
				}
				memcpy(name_buf, xcb_get_atom_name_name(nr), (size_t)n);
				name_buf[n] = '\0';
				free(nr);
			}
		}
		U_LOG_W("XCB: created %ux%u window 0x%08x at (%d, %d), fullscreen on RandR monitor #%d (%s)", width,
		        height, (unsigned)win->window, (int)screen_left, (int)screen_top, fullscreen_monitor,
		        name_buf);
	} else {
		U_LOG_I("XCB: created %ux%u window 0x%08x at (%d, %d)", width, height, (unsigned)win->window,
		        (int)screen_left, (int)screen_top);
	}

	*out_win = win;
	return XRT_SUCCESS;
}

void
comp_vk_native_window_xcb_get_handle(struct comp_vk_native_window_xcb *win,
                                     struct comp_vk_native_xcb_handle *out_handle)
{
	if (win == NULL || out_handle == NULL) {
		return;
	}
	out_handle->connection = win->connection;
	out_handle->window = (uint32_t)win->window;
}

xrt_result_t
comp_vk_native_window_xcb_wrap_app_window(void *xdisplay,
                                          unsigned long xwindow,
                                          struct comp_vk_native_xcb_handle *out_handle)
{
	if (xdisplay == NULL || xwindow == 0 || out_handle == NULL) {
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	xcb_connection_t *conn = XGetXCBConnection((Display *)xdisplay);
	if (conn == NULL || xcb_connection_has_error(conn)) {
		U_LOG_E("XGetXCBConnection failed for app-provided Display %p", xdisplay);
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	out_handle->connection = conn;
	out_handle->window = (uint32_t)xwindow;
	return XRT_SUCCESS;
}

bool
comp_vk_native_window_xcb_query_geometry(const struct comp_vk_native_xcb_handle *handle,
                                         uint32_t *out_width,
                                         uint32_t *out_height)
{
	if (handle == NULL || handle->connection == NULL || handle->window == 0) {
		return false;
	}

	xcb_connection_t *conn = (xcb_connection_t *)handle->connection;
	xcb_get_geometry_cookie_t cookie = xcb_get_geometry(conn, (xcb_drawable_t)handle->window);
	xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(conn, cookie, NULL);
	if (reply == NULL) {
		return false;
	}

	if (out_width != NULL) {
		*out_width = reply->width;
	}
	if (out_height != NULL) {
		*out_height = reply->height;
	}
	free(reply);
	return true;
}

void
comp_vk_native_window_xcb_pump(struct comp_vk_native_window_xcb *win)
{
	if (win == NULL || win->connection == NULL) {
		return;
	}

	xcb_generic_event_t *event = NULL;
	while ((event = xcb_poll_for_event(win->connection)) != NULL) {
		switch (event->response_type & ~0x80) {
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t *cfg = (xcb_configure_notify_event_t *)event;
			if (cfg->width > 0 && cfg->height > 0) {
				win->width = cfg->width;
				win->height = cfg->height;
			}
			break;
		}
		case XCB_CLIENT_MESSAGE: {
			xcb_client_message_event_t *cm = (xcb_client_message_event_t *)event;
			if (win->atom_wm_delete_window != XCB_ATOM_NONE &&
			    cm->data.data32[0] == win->atom_wm_delete_window) {
				win->valid = false;
			}
			break;
		}
		case XCB_DESTROY_NOTIFY: win->valid = false; break;
		default: break;
		}
		free(event);
	}

	if (xcb_connection_has_error(win->connection)) {
		win->valid = false;
	}
}

void
comp_vk_native_window_xcb_get_dimensions(struct comp_vk_native_window_xcb *win,
                                         uint32_t *out_width,
                                         uint32_t *out_height)
{
	if (win == NULL) {
		return;
	}
	comp_vk_native_window_xcb_pump(win);
	if (out_width != NULL) {
		*out_width = win->width;
	}
	if (out_height != NULL) {
		*out_height = win->height;
	}
}

bool
comp_vk_native_window_xcb_get_screen_position(struct comp_vk_native_window_xcb *win,
                                              int32_t *out_left_px,
                                              int32_t *out_top_px)
{
	if (win == NULL || win->connection == NULL) {
		return false;
	}

	// Window origin translated into root (screen) coordinates.
	xcb_translate_coordinates_cookie_t cookie =
	    xcb_translate_coordinates(win->connection, win->window, win->root, 0, 0);
	xcb_translate_coordinates_reply_t *reply =
	    xcb_translate_coordinates_reply(win->connection, cookie, NULL);
	if (reply == NULL) {
		return false;
	}
	if (out_left_px != NULL) {
		*out_left_px = reply->dst_x;
	}
	if (out_top_px != NULL) {
		*out_top_px = reply->dst_y;
	}
	free(reply);
	return true;
}

bool
comp_vk_native_window_xcb_is_valid(struct comp_vk_native_window_xcb *win)
{
	if (win == NULL) {
		return false;
	}
	comp_vk_native_window_xcb_pump(win);
	return win->valid;
}

void
comp_vk_native_window_xcb_destroy(struct comp_vk_native_window_xcb **win_ptr)
{
	if (win_ptr == NULL || *win_ptr == NULL) {
		return;
	}
	struct comp_vk_native_window_xcb *win = *win_ptr;
	if (win->connection != NULL) {
		if (win->window != XCB_NONE) {
			xcb_destroy_window(win->connection, win->window);
		}
		if (win->colormap != XCB_NONE) {
			xcb_free_colormap(win->connection, win->colormap);
		}
		if (win->window != XCB_NONE || win->colormap != XCB_NONE) {
			xcb_flush(win->connection);
		}
		xcb_disconnect(win->connection);
	}
	free(win);
	*win_ptr = NULL;
}
