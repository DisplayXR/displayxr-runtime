// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XCB (X11) window helper for the VK native compositor on Linux.
 *
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_window_xcb.h"

#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_logging.h"
#include "os/os_threading.h"
#include "xrt/xrt_system.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

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

	//! Serialises the event pump: it runs from several compositor entry points
	//! and owns the keymap below plus the order input reaches qwerty in.
	struct os_mutex pump_lock;

	//! Where decoded input goes (#1727); NULL = input is drained and dropped.
	struct xrt_system_devices *xsysd;

	//! Core keyboard mapping (xcb_get_keyboard_mapping), refreshed on
	//! MappingNotify. keysyms[(keycode - min_keycode) * keysyms_per_keycode]
	//! is the level-0 keysym of a keycode.
	uint8_t min_keycode;
	uint8_t keysyms_per_keycode;
	uint32_t keysym_count;
	uint32_t *keysyms;

	//! Keycodes this window has seen go down and not yet up, for telling
	//! autorepeat apart from a real release/press at the same timestamp.
	uint8_t keys_down[32];
};

#ifdef XRT_BUILD_DRIVER_QWERTY
// [QTRACE] input-path tracer, off unless DXR_QTRACE=1 (docs/reference/debug-logging.md)
DEBUG_GET_ONCE_BOOL_OPTION(xcb_win_qtrace, "DXR_QTRACE", false)

//! Input the window selects when qwerty can consume it (#1727). Only the
//! client owning a window may select ButtonPress on it; the runtime owns this one.
#define XCB_WINDOW_INPUT_EVENT_MASK                                                                                    \
	(XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS |                         \
	 XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_FOCUS_CHANGE)
#else
#define XCB_WINDOW_INPUT_EVENT_MASK (XCB_EVENT_MASK_KEY_PRESS)
#endif

#ifdef XRT_BUILD_DRIVER_QWERTY
/*!
 * (Re)load the core keycode -> keysym table. Core protocol only, so no
 * xcb-keysyms / xkbcommon dependency: the pump needs nothing but the level-0
 * keysym of each keycode.
 */
static void
load_keymap(struct comp_vk_native_window_xcb *win)
{
	const xcb_setup_t *setup = xcb_get_setup(win->connection);
	const uint8_t min_kc = setup->min_keycode;
	const uint8_t count = (uint8_t)(setup->max_keycode - setup->min_keycode + 1);

	xcb_get_keyboard_mapping_reply_t *reply = xcb_get_keyboard_mapping_reply(
	    win->connection, xcb_get_keyboard_mapping(win->connection, min_kc, count), NULL);
	if (reply == NULL) {
		U_LOG_W("XCB: GetKeyboardMapping failed — keyboard input to qwerty disabled");
		return;
	}
	const int n = xcb_get_keyboard_mapping_keysyms_length(reply);
	uint32_t *table = U_TYPED_ARRAY_CALLOC(uint32_t, n > 0 ? (size_t)n : 1);
	if (n > 0) {
		memcpy(table, xcb_get_keyboard_mapping_keysyms(reply), (size_t)n * sizeof(uint32_t));
	}
	free(win->keysyms);
	win->keysyms = table;
	win->keysym_count = (uint32_t)(n > 0 ? n : 0);
	win->min_keycode = min_kc;
	win->keysyms_per_keycode = reply->keysyms_per_keycode;
	free(reply);
}

/*!
 * Level-0 keysym of @p keycode. A letter the server lists only in upper case
 * (a core-protocol "alphabetic pair" with NoSymbol second) is folded to lower
 * case so the qwerty map matches on one spelling, as a Win32 virtual key does.
 */
static uint32_t
keycode_to_keysym(const struct comp_vk_native_window_xcb *win, uint8_t keycode)
{
	if (win->keysyms == NULL || win->keysyms_per_keycode == 0 || keycode < win->min_keycode) {
		return 0;
	}
	const uint32_t idx = (uint32_t)(keycode - win->min_keycode) * win->keysyms_per_keycode;
	if (idx >= win->keysym_count) {
		return 0;
	}
	uint32_t ks = win->keysyms[idx];
	if (ks >= 0x41 && ks <= 0x5a) { // XK_A..XK_Z
		ks += 0x20;
	}
	return ks;
}

//! Hand one decoded event to qwerty. Caller holds pump_lock.
static void
dispatch_input(struct comp_vk_native_window_xcb *win, const struct qwerty_x11_input *in)
{
	if (win->xsysd != NULL) {
		qwerty_process_xcb(win->xsysd->xdevs, win->xsysd->xdev_count, in);
	}
}

//! Release everything qwerty holds (focus loss, unmap, teardown). Caller holds pump_lock.
static void
dispatch_focus_out(struct comp_vk_native_window_xcb *win)
{
	struct qwerty_x11_input in = {0};
	in.type = QWERTY_X11_FOCUS_OUT;
	dispatch_input(win, &in);
}

/*!
 * Decode an input event into qwerty. Returns false if @p event is not input.
 * Caller holds pump_lock.
 */
static bool
handle_input_event(struct comp_vk_native_window_xcb *win, xcb_generic_event_t *event)
{
	struct qwerty_x11_input in = {0};

	switch (event->response_type & ~0x80) {
	case XCB_KEY_PRESS:
	case XCB_KEY_RELEASE: {
		// xcb_key_release_event_t is a typedef of the press layout.
		xcb_key_press_event_t *k = (xcb_key_press_event_t *)event;
		in.type =
		    (event->response_type & ~0x80) == XCB_KEY_PRESS ? QWERTY_X11_KEY_PRESS : QWERTY_X11_KEY_RELEASE;
		in.keysym = keycode_to_keysym(win, k->detail);
		in.state = k->state;
		in.root_x = k->root_x;
		in.root_y = k->root_y;
		break;
	}
	case XCB_BUTTON_PRESS:
	case XCB_BUTTON_RELEASE: {
		xcb_button_press_event_t *b = (xcb_button_press_event_t *)event;
		in.type = (event->response_type & ~0x80) == XCB_BUTTON_PRESS ? QWERTY_X11_BUTTON_PRESS
		                                                             : QWERTY_X11_BUTTON_RELEASE;
		in.button = b->detail;
		in.state = b->state;
		in.root_x = b->root_x;
		in.root_y = b->root_y;
		break;
	}
	case XCB_MOTION_NOTIFY: {
		// Only the position is forwarded. The event's state carries button
		// bits too, and those are exactly what must never become an edge (#1700).
		xcb_motion_notify_event_t *m = (xcb_motion_notify_event_t *)event;
		in.type = QWERTY_X11_MOTION;
		in.root_x = m->root_x;
		in.root_y = m->root_y;
		break;
	}
	case XCB_FOCUS_IN: {
		xcb_focus_in_event_t *f = (xcb_focus_in_event_t *)event;
		if (f->detail == XCB_NOTIFY_DETAIL_INFERIOR) {
			return true; // Focus moved between our own subwindows: no change.
		}
		// Live CTRL/ALT for the Win32 WM_SETFOCUS re-sync; buttons are ignored.
		in.type = QWERTY_X11_FOCUS_IN;
		xcb_query_pointer_reply_t *qp =
		    xcb_query_pointer_reply(win->connection, xcb_query_pointer(win->connection, win->window), NULL);
		if (qp != NULL) {
			in.state = qp->mask;
			in.root_x = qp->root_x;
			in.root_y = qp->root_y;
			free(qp);
		}
		break;
	}
	case XCB_FOCUS_OUT: {
		xcb_focus_out_event_t *f = (xcb_focus_out_event_t *)event;
		if (f->detail == XCB_NOTIFY_DETAIL_INFERIOR) {
			return true;
		}
		in.type = QWERTY_X11_FOCUS_OUT;
		break;
	}
	case XCB_UNMAP_NOTIFY: in.type = QWERTY_X11_FOCUS_OUT; break;
	case XCB_MAPPING_NOTIFY: {
		xcb_mapping_notify_event_t *mn = (xcb_mapping_notify_event_t *)event;
		if (mn->request == XCB_MAPPING_KEYBOARD) {
			load_keymap(win);
		}
		return true;
	}
	default: return false;
	}

	dispatch_input(win, &in);
	return true;
}

/*!
 * X11 key autorepeat arrives as a KeyRelease immediately followed by a KeyPress
 * of the same keycode with the same timestamp. Delivered as-is, a held key
 * would produce a release/press EDGE pair every repeat — a held N (Menu) or V
 * would be clicked over and over. Win32 autorepeat never releases, so the pair
 * is dropped to match.
 */
static bool
is_autorepeat_pair(const struct comp_vk_native_window_xcb *win, xcb_generic_event_t *release, xcb_generic_event_t *next)
{
	if (next == NULL || (next->response_type & ~0x80) != XCB_KEY_PRESS) {
		return false;
	}
	const xcb_key_release_event_t *r = (const xcb_key_release_event_t *)release;
	const xcb_key_press_event_t *p = (const xcb_key_press_event_t *)next;
	// Only a key that is DOWN can repeat. XTEST injectors (xdotool) stamp a
	// genuine release and the following press of the same key with the same
	// millisecond, and those must both be delivered.
	const bool down = (win->keys_down[r->detail >> 3] & (1u << (r->detail & 7))) != 0;
	return down && r->detail == p->detail && r->time == p->time;
}

//! Track key state for is_autorepeat_pair. Caller holds pump_lock.
static void
track_key_state(struct comp_vk_native_window_xcb *win, xcb_generic_event_t *event)
{
	const uint8_t type = event->response_type & ~0x80;
	if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
		const uint8_t kc = ((xcb_key_press_event_t *)event)->detail;
		if (type == XCB_KEY_PRESS) {
			win->keys_down[kc >> 3] |= (uint8_t)(1u << (kc & 7));
		} else {
			win->keys_down[kc >> 3] &= (uint8_t)~(1u << (kc & 7));
		}
	} else if (type == XCB_FOCUS_OUT || type == XCB_UNMAP_NOTIFY || type == XCB_DESTROY_NOTIFY) {
		// No release reaches a window that lost focus; forget what was down.
		memset(win->keys_down, 0, sizeof(win->keys_down));
	}
}

#endif // XRT_BUILD_DRIVER_QWERTY

void
comp_vk_native_window_xcb_set_system_devices(struct comp_vk_native_window_xcb *win, struct xrt_system_devices *xsysd)
{
	if (win == NULL) {
		return;
	}
	os_mutex_lock(&win->pump_lock);
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (win->xsysd != NULL && win->xsysd != xsysd) {
		dispatch_focus_out(win); // Nothing may stay held on the devices we leave.
	}
	win->xsysd = xsysd;
	if (xsysd != NULL) {
		U_LOG_I("XCB: window input routed to qwerty (keyboard, buttons, pointer motion)");
	}
#else
	(void)xsysd;
#endif
	os_mutex_unlock(&win->pump_lock);
}

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
	os_mutex_init(&win->pump_lock);

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
		value_list[2] = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_WINDOW_INPUT_EVENT_MASK;
		value_list[3] = win->colormap;
		U_LOG_I("XCB: transparent-background mode — 32-bit ARGB visual 0x%x", (unsigned)argb_visual);
	} else {
		if (transparent_background) {
			U_LOG_W("XCB: transparentBackgroundEnabled requested but no 32-bit ARGB visual is "
			        "available — falling back to an opaque window (is a compositing WM running?)");
		}
		value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
		value_list[0] = screen->black_pixel;
		value_list[1] = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_WINDOW_INPUT_EVENT_MASK;
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

#ifdef XRT_BUILD_DRIVER_QWERTY
	load_keymap(win);
#endif

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

bool
comp_vk_native_window_xcb_query_screen_position(const struct comp_vk_native_xcb_handle *handle,
                                                int32_t *out_left_px,
                                                int32_t *out_top_px)
{
	if (handle == NULL || handle->connection == NULL || handle->window == 0) {
		return false;
	}

	xcb_connection_t *conn = (xcb_connection_t *)handle->connection;
	const xcb_setup_t *setup = xcb_get_setup(conn);
	if (setup == NULL) {
		return false;
	}
	xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
	if (screen == NULL) {
		return false;
	}

	// App window origin (0,0) translated into root (screen) coordinates.
	xcb_translate_coordinates_cookie_t cookie =
	    xcb_translate_coordinates(conn, (xcb_window_t)handle->window, screen->root, 0, 0);
	xcb_translate_coordinates_reply_t *reply =
	    xcb_translate_coordinates_reply(conn, cookie, NULL);
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
comp_vk_native_window_xcb_query_refresh_hz(const struct comp_vk_native_xcb_handle *handle, float *out_hz)
{
	if (handle == NULL || handle->connection == NULL || handle->window == 0 || out_hz == NULL) {
		return false;
	}

	xcb_connection_t *conn = (xcb_connection_t *)handle->connection;
	const xcb_setup_t *setup = xcb_get_setup(conn);
	if (setup == NULL) {
		return false;
	}
	xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
	if (screen == NULL) {
		return false;
	}
	xcb_window_t root = screen->root;

	// Window centre in root (screen) pixels, so we pick the CRTC the window
	// actually sits on rather than assuming the primary output (the Odyssey is
	// not necessarily primary when a laptop panel is also connected).
	int32_t cx = 0, cy = 0;
	{
		xcb_get_geometry_reply_t *geo =
		    xcb_get_geometry_reply(conn, xcb_get_geometry(conn, (xcb_drawable_t)handle->window), NULL);
		xcb_translate_coordinates_reply_t *pos = xcb_translate_coordinates_reply(
		    conn, xcb_translate_coordinates(conn, (xcb_window_t)handle->window, root, 0, 0), NULL);
		if (geo != NULL && pos != NULL) {
			cx = pos->dst_x + (int32_t)geo->width / 2;
			cy = pos->dst_y + (int32_t)geo->height / 2;
		}
		free(geo);
		free(pos);
	}

	xcb_randr_get_screen_resources_current_reply_t *res = xcb_randr_get_screen_resources_current_reply(
	    conn, xcb_randr_get_screen_resources_current(conn, root), NULL);
	if (res == NULL) {
		return false;
	}

	xcb_randr_mode_info_t *modes = xcb_randr_get_screen_resources_current_modes(res);
	int modes_len = xcb_randr_get_screen_resources_current_modes_length(res);
	xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
	int crtcs_len = xcb_randr_get_screen_resources_current_crtcs_length(res);

	xcb_randr_mode_t chosen_mode = 0;   // CRTC under the window centre
	xcb_randr_mode_t fallback_mode = 0; // first active CRTC (if the centre misses)

	for (int i = 0; i < crtcs_len; i++) {
		xcb_randr_get_crtc_info_reply_t *ci = xcb_randr_get_crtc_info_reply(
		    conn, xcb_randr_get_crtc_info(conn, crtcs[i], res->config_timestamp), NULL);
		if (ci == NULL) {
			continue;
		}
		if (ci->mode != 0 && ci->width > 0 && ci->height > 0) {
			if (fallback_mode == 0) {
				fallback_mode = ci->mode;
			}
			if (cx >= ci->x && cx < ci->x + (int32_t)ci->width && cy >= ci->y &&
			    cy < ci->y + (int32_t)ci->height) {
				chosen_mode = ci->mode;
				free(ci);
				break;
			}
		}
		free(ci);
	}
	if (chosen_mode == 0) {
		chosen_mode = fallback_mode;
	}

	float hz = 0.0f;
	if (chosen_mode != 0) {
		for (int i = 0; i < modes_len; i++) {
			const xcb_randr_mode_info_t *m = &modes[i];
			if (m->id != chosen_mode) {
				continue;
			}
			// Standard RandR vrefresh: dot_clock / (htotal * vtotal), adjusted
			// for interlace / double-scan flags.
			double vtotal = (double)m->vtotal;
			if (m->mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN) {
				vtotal *= 2.0;
			}
			if (m->mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE) {
				vtotal /= 2.0;
			}
			if (m->htotal != 0 && vtotal != 0.0) {
				hz = (float)((double)m->dot_clock / ((double)m->htotal * vtotal));
			}
			break;
		}
	}

	free(res);

	if (hz > 1.0f) {
		*out_hz = hz;
		return true;
	}
	return false;
}

void
comp_vk_native_window_xcb_pump(struct comp_vk_native_window_xcb *win)
{
	if (win == NULL || win->connection == NULL) {
		return;
	}

	os_mutex_lock(&win->pump_lock);

	xcb_generic_event_t *event = NULL;
	xcb_generic_event_t *pending = NULL; // Read ahead by the autorepeat check.
	for (;;) {
		if (pending != NULL) {
			event = pending;
			pending = NULL;
		} else if ((event = xcb_poll_for_event(win->connection)) == NULL) {
			break;
		}
		const uint8_t type = event->response_type & ~0x80;

#ifdef XRT_BUILD_DRIVER_QWERTY
		if (type == XCB_KEY_RELEASE) {
			pending = xcb_poll_for_event(win->connection);
			if (is_autorepeat_pair(win, event, pending)) {
				if (debug_get_bool_option_xcb_win_qtrace()) {
					U_LOG_W("[QTRACE] XCB autorepeat pair dropped keycode=%u",
					        ((xcb_key_press_event_t *)event)->detail);
				}
				free(event);
				free(pending);
				pending = NULL;
				continue;
			}
		}
		track_key_state(win, event);
		if (handle_input_event(win, event)) {
			free(event);
			continue;
		}
#endif

		switch (type) {
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
		case XCB_DESTROY_NOTIFY:
			win->valid = false;
#ifdef XRT_BUILD_DRIVER_QWERTY
			dispatch_focus_out(win); // Nothing may stay held on a window that is gone.
#endif
			break;
		default: break;
		}
		free(event);
	}

	if (xcb_connection_has_error(win->connection)) {
		win->valid = false;
	}

	os_mutex_unlock(&win->pump_lock);
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

	// The window is about to vanish mid-whatever: a button or key held now
	// would never see its release event (#1700).
	os_mutex_lock(&win->pump_lock);
#ifdef XRT_BUILD_DRIVER_QWERTY
	dispatch_focus_out(win);
#endif
	win->xsysd = NULL;
	os_mutex_unlock(&win->pump_lock);

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
	os_mutex_destroy(&win->pump_lock);
	free(win->keysyms);
	free(win);
	*win_ptr = NULL;
}
