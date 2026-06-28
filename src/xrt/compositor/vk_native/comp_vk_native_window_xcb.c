// Copyright 2026, Leia Inc.
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

struct comp_vk_native_window_xcb
{
	xcb_connection_t *connection;
	xcb_window_t window;
	xcb_window_t root;

	//! WM_DELETE_WINDOW atom, so a user close is a clean event not an X error.
	xcb_atom_t atom_wm_delete_window;

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

xrt_result_t
comp_vk_native_window_xcb_create(uint32_t width,
                                 uint32_t height,
                                 bool transparent_background,
                                 struct comp_vk_native_window_xcb **out_win)
{
	(void)transparent_background; // X11 ARGB-visual transparency not wired in Phase 1.

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

	uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	uint32_t value_list[] = {
	    screen->black_pixel,
	    XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS,
	};

	xcb_create_window(conn,
	                  XCB_COPY_FROM_PARENT, // depth
	                  win->window, screen->root,
	                  0, 0, (uint16_t)width, (uint16_t)height,
	                  0,                                 // border
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
	                  screen->root_visual,
	                  value_mask, value_list);

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

	xcb_map_window(conn, win->window);
	xcb_flush(conn);

	U_LOG_I("XCB: created %ux%u window 0x%08x", width, height, (unsigned)win->window);

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
			xcb_flush(win->connection);
		}
		xcb_disconnect(win->connection);
	}
	free(win);
	*win_ptr = NULL;
}
