// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux present target for the out-of-process service compositor.
 * @author David Fattal
 * @ingroup comp_main
 *
 * See comp_window_linux.h. Structure mirrors comp_window_macos.m: a
 * comp_target_swapchain subclass whose init_post_vulkan builds the
 * runtime-owned window and the VkSurfaceKHR over it. The window placement
 * recipe (fullscreen on the panel's RandR monitor via EWMH) follows the
 * in-process hosted window, comp_vk_native_window_xcb.c (#715); the input
 * decoding follows its qwerty pump (#1727) but feeds the workspace input
 * router instead of qwerty.
 */

#include "xrt/xrt_compiler.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_debug.h"

#include "vk/vk_helpers.h"

#include "main/comp_compositor.h"
#include "main/comp_target_swapchain.h"
#include "main/comp_window_linux.h"

#include <vulkan/vulkan_xcb.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// DXR_KEY_DEBUG=1: one WARN per decoded key press (same knob as the macOS pump).
DEBUG_GET_ONCE_BOOL_OPTION(linux_win_key_debug, "DXR_KEY_DEBUG", false)

#define CWL_EVENT_MASK                                                                                                 \
	(XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |                     \
	 XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |                 \
	 XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_EXPOSURE)


/*
 *
 * Process-wide input sink.
 *
 */

static pthread_mutex_t g_sink_lock = PTHREAD_MUTEX_INITIALIZER;
static comp_window_linux_input_fn g_sink_fn = NULL;
static void *g_sink_ud = NULL;

void
comp_window_linux_set_input_sink(comp_window_linux_input_fn fn, void *userdata)
{
	pthread_mutex_lock(&g_sink_lock);
	g_sink_fn = fn;
	g_sink_ud = userdata;
	pthread_mutex_unlock(&g_sink_lock);
}

static void
emit_input(const struct comp_window_linux_input *ev)
{
	// Held across the call so a sink being cleared cannot race a delivery that
	// is still using its userdata. The sink must not call back into this file.
	pthread_mutex_lock(&g_sink_lock);
	if (g_sink_fn != NULL) {
		g_sink_fn(g_sink_ud, ev);
	}
	pthread_mutex_unlock(&g_sink_lock);
}


/*
 *
 * Keysym -> Windows virtual-key code.
 *
 */

uint32_t
comp_window_linux_keysym_to_vk(uint32_t ks)
{
	// Letters: the controller matches VK_A..VK_Z (upper-case ASCII). Accept
	// either case; level 0 is normally lower case.
	if (ks >= 0x61 && ks <= 0x7a) { // XK_a..XK_z
		return ks - 0x20;
	}
	if (ks >= 0x41 && ks <= 0x5a) { // XK_A..XK_Z
		return ks;
	}
	if (ks >= 0x30 && ks <= 0x39) { // XK_0..XK_9 == VK_0..VK_9
		return ks;
	}
	if (ks >= 0xffbe && ks <= 0xffc9) { // XK_F1..XK_F12 -> VK_F1..VK_F12
		return 0x70 + (ks - 0xffbe);
	}
	if (ks >= 0xffb0 && ks <= 0xffb9) { // XK_KP_0..XK_KP_9 -> VK_NUMPAD0..9
		return 0x60 + (ks - 0xffb0);
	}
	switch (ks) {
	case 0x0020: return 0x20; // space        -> VK_SPACE
	case 0xff08: return 0x08; // BackSpace    -> VK_BACK
	case 0xff09: return 0x09; // Tab          -> VK_TAB
	case 0xfe20: return 0x09; // ISO_Left_Tab (Shift+Tab) -> VK_TAB
	case 0xff0d: return 0x0D; // Return       -> VK_RETURN
	case 0xff8d: return 0x0D; // KP_Enter     -> VK_RETURN
	case 0xff1b: return 0x1B; // Escape       -> VK_ESCAPE
	case 0xffff: return 0x2E; // Delete       -> VK_DELETE
	case 0xff9f: return 0x2E; // KP_Delete    -> VK_DELETE
	case 0xff63: return 0x2D; // Insert       -> VK_INSERT
	case 0xff50: return 0x24; // Home         -> VK_HOME
	case 0xff57: return 0x23; // End          -> VK_END
	case 0xff55: return 0x21; // Prior        -> VK_PRIOR
	case 0xff56: return 0x22; // Next         -> VK_NEXT
	case 0xff51: return 0x25; // Left         -> VK_LEFT
	case 0xff52: return 0x26; // Up           -> VK_UP
	case 0xff53: return 0x27; // Right        -> VK_RIGHT
	case 0xff54: return 0x28; // Down         -> VK_DOWN
	case 0xffe1:              // Shift_L
	case 0xffe2: return 0x10; // Shift_R      -> VK_SHIFT
	case 0xffe3:              // Control_L
	case 0xffe4: return 0x11; // Control_R    -> VK_CONTROL
	case 0xffe9:              // Alt_L
	case 0xffea: return 0x12; // Alt_R        -> VK_MENU
	case 0xffeb: return 0x5B; // Super_L      -> VK_LWIN
	case 0xffec: return 0x5C; // Super_R      -> VK_RWIN
	case 0x003b: return 0xBA; // semicolon    -> VK_OEM_1
	case 0x003d: return 0xBB; // equal        -> VK_OEM_PLUS
	case 0x002c: return 0xBC; // comma        -> VK_OEM_COMMA
	case 0x002d: return 0xBD; // minus        -> VK_OEM_MINUS
	case 0x002e: return 0xBE; // period       -> VK_OEM_PERIOD
	case 0x002f: return 0xBF; // slash        -> VK_OEM_2
	case 0x0060: return 0xC0; // grave        -> VK_OEM_3
	case 0x005b: return 0xDB; // bracketleft  -> VK_OEM_4
	case 0x005c: return 0xDC; // backslash    -> VK_OEM_5
	case 0x005d: return 0xDD; // bracketright -> VK_OEM_6
	case 0x0027: return 0xDE; // apostrophe   -> VK_OEM_7
	case 0xffab: return 0x6B; // KP_Add       -> VK_ADD
	case 0xffad: return 0x6D; // KP_Subtract  -> VK_SUBTRACT
	default: return 0;
	}
}


/*
 *
 * Target struct.
 *
 */

/*!
 * A desktop-Linux present target.
 *
 * @implements comp_target_swapchain
 */
struct comp_window_linux
{
	struct comp_target_swapchain base;

	//! Panel origin in virtual-desktop px (selects the fullscreen monitor).
	int32_t screen_left, screen_top;

	xcb_connection_t *conn;
	xcb_window_t window;
	xcb_window_t root;
	xcb_cursor_t blank_cursor;

	xcb_atom_t atom_wm_protocols;
	xcb_atom_t atom_wm_delete_window;
	xcb_atom_t atom_net_wm_state;
	xcb_atom_t atom_net_wm_state_fullscreen;
	xcb_atom_t atom_net_wm_fullscreen_monitors;

	//! RandR monitor index the window is fullscreened on, -1 = none resolved.
	int fullscreen_monitor;

	//! Guards the fields below (event thread vs render thread vs IPC threads).
	pthread_mutex_t lock;
	uint32_t width, height; //!< Live size (ConfigureNotify).
	bool mapped_requested;  //!< Last set_visible() state.
	bool cursor_hidden;
	int32_t last_x, last_y; //!< Last pointer position (window px).

	//! Keyboard map (core protocol, level-0 keysyms), event thread only.
	uint8_t min_keycode;
	uint8_t keysyms_per_keycode;
	uint32_t keysym_count;
	uint32_t *keysyms;
	uint8_t keys_down[32];

	//! Event thread.
	pthread_t thread;
	bool thread_started;
	volatile bool thread_running;
};


/*
 *
 * Helpers.
 *
 */

static inline struct vk_bundle *
get_vk(struct comp_window_linux *cwl)
{
	// In the service ct->c is a null_compositor up-cast to comp_compositor; both
	// start with comp_base, so base.vk is the system compositor's bundle.
	return &cwl->base.base.c->base.vk;
}

static xcb_atom_t
intern_atom(xcb_connection_t *conn, const char *name)
{
	xcb_intern_atom_reply_t *reply =
	    xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return atom;
}

/*!
 * RandR monitor INDEX that owns the panel origin (exact origin match first,
 * else the monitor containing it). Same resolution as the hosted window
 * (comp_vk_native_window_xcb.c resolve_monitor_index). Also returns that
 * monitor's size, so the window can be created at the final extent.
 */
static int
resolve_monitor(xcb_connection_t *conn,
                xcb_window_t root,
                int32_t left,
                int32_t top,
                uint32_t *out_w,
                uint32_t *out_h,
                int32_t *out_x,
                int32_t *out_y)
{
	xcb_randr_get_monitors_reply_t *reply =
	    xcb_randr_get_monitors_reply(conn, xcb_randr_get_monitors(conn, root, 1 /* active only */), NULL);
	if (reply == NULL) {
		return -1;
	}
	int exact = -1, contains = -1;
	xcb_randr_monitor_info_t exact_m = {0}, contains_m = {0};
	xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(reply);
	for (int idx = 0; it.rem; xcb_randr_monitor_info_next(&it), idx++) {
		const xcb_randr_monitor_info_t *m = it.data;
		if (exact < 0 && m->x == (int16_t)left && m->y == (int16_t)top) {
			exact = idx;
			exact_m = *m;
		}
		if (contains < 0 && left >= m->x && left < m->x + (int32_t)m->width && top >= m->y &&
		    top < m->y + (int32_t)m->height) {
			contains = idx;
			contains_m = *m;
		}
	}
	free(reply);
	int chosen = exact >= 0 ? exact : contains;
	const xcb_randr_monitor_info_t *m = exact >= 0 ? &exact_m : &contains_m;
	if (chosen >= 0) {
		*out_w = m->width;
		*out_h = m->height;
		*out_x = m->x;
		*out_y = m->y;
	}
	return chosen;
}

static void
load_keymap(struct comp_window_linux *cwl)
{
	const xcb_setup_t *setup = xcb_get_setup(cwl->conn);
	const uint8_t min_kc = setup->min_keycode;
	const uint8_t count = (uint8_t)(setup->max_keycode - setup->min_keycode + 1);
	xcb_get_keyboard_mapping_reply_t *reply =
	    xcb_get_keyboard_mapping_reply(cwl->conn, xcb_get_keyboard_mapping(cwl->conn, min_kc, count), NULL);
	if (reply == NULL) {
		U_LOG_W("comp_window_linux: GetKeyboardMapping failed — keyboard input disabled");
		return;
	}
	const int n = xcb_get_keyboard_mapping_keysyms_length(reply);
	uint32_t *table = U_TYPED_ARRAY_CALLOC(uint32_t, n > 0 ? (size_t)n : 1);
	if (n > 0) {
		memcpy(table, xcb_get_keyboard_mapping_keysyms(reply), (size_t)n * sizeof(uint32_t));
	}
	free(cwl->keysyms);
	cwl->keysyms = table;
	cwl->keysym_count = (uint32_t)(n > 0 ? n : 0);
	cwl->min_keycode = min_kc;
	cwl->keysyms_per_keycode = reply->keysyms_per_keycode;
	free(reply);
}

static uint32_t
keycode_to_keysym(const struct comp_window_linux *cwl, uint8_t keycode)
{
	if (cwl->keysyms == NULL || cwl->keysyms_per_keycode == 0 || keycode < cwl->min_keycode) {
		return 0;
	}
	const uint32_t idx = (uint32_t)(keycode - cwl->min_keycode) * cwl->keysyms_per_keycode;
	return idx < cwl->keysym_count ? cwl->keysyms[idx] : 0;
}

//! X core state mask -> IPC wire modifiers (bit0 SHIFT, bit1 CTRL, bit2 ALT).
static uint32_t
state_to_mods(uint16_t state)
{
	uint32_t mods = 0;
	if (state & XCB_MOD_MASK_SHIFT) {
		mods |= 1u << 0;
	}
	if (state & XCB_MOD_MASK_CONTROL) {
		mods |= 1u << 1;
	}
	if (state & XCB_MOD_MASK_1) {
		mods |= 1u << 2;
	}
	return mods;
}

//! X core state mask -> IPC wire button mask (bit0 L, bit1 R, bit2 M).
static uint32_t
state_to_button_mask(uint16_t state)
{
	uint32_t mask = 0;
	if (state & XCB_BUTTON_MASK_1) {
		mask |= 1u << 0;
	}
	if (state & XCB_BUTTON_MASK_3) {
		mask |= 1u << 1;
	}
	if (state & XCB_BUTTON_MASK_2) {
		mask |= 1u << 2;
	}
	return mask;
}

/*!
 * Ask the window manager to fullscreen the window on the panel's monitor.
 * Sent after map: EWMH _NET_WM_STATE add FULLSCREEN, then
 * _NET_WM_FULLSCREEN_MONITORS pinning all four edges to the monitor index.
 */
static void
request_fullscreen(struct comp_window_linux *cwl)
{
	if (cwl->atom_net_wm_state == XCB_ATOM_NONE || cwl->atom_net_wm_state_fullscreen == XCB_ATOM_NONE) {
		return;
	}
	const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;

	xcb_client_message_event_t ev = {0};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = cwl->window;
	ev.type = cwl->atom_net_wm_state;
	ev.data.data32[0] = 1; // _NET_WM_STATE_ADD
	ev.data.data32[1] = cwl->atom_net_wm_state_fullscreen;
	ev.data.data32[3] = 1; // source: application
	xcb_send_event(cwl->conn, 0, cwl->root, mask, (const char *)&ev);

	if (cwl->fullscreen_monitor >= 0 && cwl->atom_net_wm_fullscreen_monitors != XCB_ATOM_NONE) {
		xcb_client_message_event_t fm = {0};
		fm.response_type = XCB_CLIENT_MESSAGE;
		fm.format = 32;
		fm.window = cwl->window;
		fm.type = cwl->atom_net_wm_fullscreen_monitors;
		for (int i = 0; i < 4; i++) {
			fm.data.data32[i] = (uint32_t)cwl->fullscreen_monitor;
		}
		fm.data.data32[4] = 1; // source: application
		xcb_send_event(cwl->conn, 0, cwl->root, mask, (const char *)&fm);
	}
	xcb_flush(cwl->conn);
}

/*!
 * Map the window onto the panel and request fullscreen there. The order is the
 * one validated on GNOME/XWayland for a panel-sized toplevel (#715, #729):
 * _NET_WM_STATE pre-set, map, post-map move onto the target output, then the
 * fullscreen client messages (mutter fullscreens onto whichever output the
 * window currently occupies).
 */
static void
map_on_panel(struct comp_window_linux *cwl)
{
	if (cwl->atom_net_wm_state != XCB_ATOM_NONE && cwl->atom_net_wm_state_fullscreen != XCB_ATOM_NONE) {
		xcb_change_property(cwl->conn, XCB_PROP_MODE_REPLACE, cwl->window, cwl->atom_net_wm_state,
		                    XCB_ATOM_ATOM, 32, 1, &cwl->atom_net_wm_state_fullscreen);
	}
	xcb_map_window(cwl->conn, cwl->window);
	const uint32_t coords[2] = {(uint32_t)cwl->screen_left, (uint32_t)cwl->screen_top};
	xcb_configure_window(cwl->conn, cwl->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coords);
	xcb_flush(cwl->conn);
	request_fullscreen(cwl);
}


/*
 *
 * Event handling.
 *
 */

/*!
 * X11 autorepeat arrives as KeyRelease immediately followed by a KeyPress of
 * the same keycode with the same timestamp. The controller treats every
 * forwarded key-down as a fresh press (one toggle / cycle per press), exactly
 * as the macOS pump swallows `isARepeat`, so the pair is dropped.
 */
static bool
is_autorepeat_pair(const struct comp_window_linux *cwl, xcb_generic_event_t *release, xcb_generic_event_t *next)
{
	if (next == NULL || (next->response_type & ~0x80) != XCB_KEY_PRESS) {
		return false;
	}
	const xcb_key_release_event_t *r = (const xcb_key_release_event_t *)release;
	const xcb_key_press_event_t *p = (const xcb_key_press_event_t *)next;
	const bool down = (cwl->keys_down[r->detail >> 3] & (1u << (r->detail & 7))) != 0;
	return down && r->detail == p->detail && r->time == p->time;
}

static void
set_last_pointer(struct comp_window_linux *cwl, int32_t x, int32_t y)
{
	pthread_mutex_lock(&cwl->lock);
	cwl->last_x = x;
	cwl->last_y = y;
	pthread_mutex_unlock(&cwl->lock);
}

/*!
 * Decode one X event. Motion is coalesced by the caller (@p pending_motion):
 * X delivers one MotionNotify per pointer sample, and the per-target input
 * rings are 16 deep, so only the latest position per batch is forwarded.
 */
static void
handle_event(struct comp_window_linux *cwl,
             xcb_generic_event_t *event,
             struct comp_window_linux_input *pending_motion,
             bool *have_pending_motion)
{
	const uint8_t type = event->response_type & ~0x80;

	// Anything but motion flushes the coalesced motion first, so ordering
	// (move-then-click) is preserved.
	if (type != XCB_MOTION_NOTIFY && *have_pending_motion) {
		emit_input(pending_motion);
		*have_pending_motion = false;
	}

	struct comp_window_linux_input in = {0};

	switch (type) {
	case XCB_KEY_PRESS:
	case XCB_KEY_RELEASE: {
		xcb_key_press_event_t *k = (xcb_key_press_event_t *)event;
		const bool down = type == XCB_KEY_PRESS;
		if (down) {
			cwl->keys_down[k->detail >> 3] |= (uint8_t)(1u << (k->detail & 7));
		} else {
			cwl->keys_down[k->detail >> 3] &= (uint8_t)~(1u << (k->detail & 7));
		}
		in.type = COMP_WINDOW_LINUX_INPUT_KEY;
		in.timestamp_ms = k->time;
		in.keysym = keycode_to_keysym(cwl, k->detail);
		in.vk_code = comp_window_linux_keysym_to_vk(in.keysym);
		in.is_down = down;
		in.modifiers = state_to_mods(k->state);
		in.x = k->event_x;
		in.y = k->event_y;
		if (in.vk_code == 0) {
			return; // nothing the controller can match
		}
		if (down && debug_get_bool_option_linux_win_key_debug()) {
			U_LOG_W("[key] keysym=0x%04x vk=0x%02x mods=0x%x", in.keysym, in.vk_code, in.modifiers);
		}
		emit_input(&in);
		return;
	}
	case XCB_BUTTON_PRESS:
	case XCB_BUTTON_RELEASE: {
		xcb_button_press_event_t *b = (xcb_button_press_event_t *)event;
		const bool down = type == XCB_BUTTON_PRESS;
		set_last_pointer(cwl, b->event_x, b->event_y);
		in.timestamp_ms = b->time;
		in.x = b->event_x;
		in.y = b->event_y;
		in.modifiers = state_to_mods(b->state);
		if (b->detail == 4 || b->detail == 5) {
			// Wheel notches arrive as a press/release pair; count the press.
			if (!down) {
				return;
			}
			in.type = COMP_WINDOW_LINUX_INPUT_SCROLL;
			in.scroll_delta = b->detail == 4 ? 1.0f : -1.0f;
			emit_input(&in);
			return;
		}
		uint32_t button = 0;
		switch (b->detail) {
		case 1: button = 1; break; // left
		case 2: button = 3; break; // middle
		case 3: button = 2; break; // right
		default: return;           // horizontal wheel / back / forward: not on the wire
		}
		in.type = COMP_WINDOW_LINUX_INPUT_BUTTON;
		in.button = button;
		in.is_down = down;
		emit_input(&in);
		return;
	}
	case XCB_MOTION_NOTIFY: {
		xcb_motion_notify_event_t *m = (xcb_motion_notify_event_t *)event;
		set_last_pointer(cwl, m->event_x, m->event_y);
		pending_motion->type = COMP_WINDOW_LINUX_INPUT_MOTION;
		pending_motion->timestamp_ms = m->time;
		pending_motion->x = m->event_x;
		pending_motion->y = m->event_y;
		pending_motion->button_mask = state_to_button_mask(m->state);
		pending_motion->modifiers = state_to_mods(m->state);
		*have_pending_motion = true;
		return;
	}
	case XCB_FOCUS_OUT: {
		xcb_focus_out_event_t *f = (xcb_focus_out_event_t *)event;
		if (f->detail == XCB_NOTIFY_DETAIL_INFERIOR) {
			return;
		}
		memset(cwl->keys_down, 0, sizeof(cwl->keys_down));
		in.type = COMP_WINDOW_LINUX_INPUT_FOCUS_LOST;
		emit_input(&in);
		return;
	}
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *cfg = (xcb_configure_notify_event_t *)event;
		if (cfg->window == cwl->window && cfg->width > 0 && cfg->height > 0) {
			pthread_mutex_lock(&cwl->lock);
			bool changed = cfg->width != cwl->width || cfg->height != cwl->height;
			cwl->width = cfg->width;
			cwl->height = cfg->height;
			pthread_mutex_unlock(&cwl->lock);
			if (changed) {
				U_LOG_I("comp_window_linux: window now %ux%u", cfg->width, cfg->height);
			}
		}
		return;
	}
	case XCB_MAPPING_NOTIFY: {
		xcb_mapping_notify_event_t *mn = (xcb_mapping_notify_event_t *)event;
		if (mn->request == XCB_MAPPING_KEYBOARD) {
			load_keymap(cwl);
		}
		return;
	}
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *cm = (xcb_client_message_event_t *)event;
		if (cwl->atom_wm_delete_window != XCB_ATOM_NONE && cm->data.data32[0] == cwl->atom_wm_delete_window) {
			// The surface is the workspace itself, not an app window: a WM close
			// (Alt+F4) does not tear it down. The controller owns the lifecycle
			// (Ctrl+Space / its own exit), which hides the surface.
			U_LOG_W("comp_window_linux: ignoring WM close request on the workspace surface");
		}
		return;
	}
	default: return;
	}
}

//! Drain every queued X event (with autorepeat filtering + motion coalescing).
static void
drain_events(struct comp_window_linux *cwl)
{
	struct comp_window_linux_input pending_motion = {0};
	bool have_pending_motion = false;
	xcb_generic_event_t *event = NULL;
	xcb_generic_event_t *lookahead = NULL;

	for (;;) {
		if (lookahead != NULL) {
			event = lookahead;
			lookahead = NULL;
		} else if ((event = xcb_poll_for_event(cwl->conn)) == NULL) {
			break;
		}
		if ((event->response_type & ~0x80) == XCB_KEY_RELEASE) {
			lookahead = xcb_poll_for_event(cwl->conn);
			if (is_autorepeat_pair(cwl, event, lookahead)) {
				free(event);
				free(lookahead);
				lookahead = NULL;
				continue;
			}
		}
		handle_event(cwl, event, &pending_motion, &have_pending_motion);
		free(event);
	}
	if (have_pending_motion) {
		emit_input(&pending_motion);
	}
}

static void *
event_thread_func(void *ptr)
{
	struct comp_window_linux *cwl = (struct comp_window_linux *)ptr;
	struct pollfd pfd = {.fd = xcb_get_file_descriptor(cwl->conn), .events = POLLIN};

	while (cwl->thread_running) {
		// Short timeout so teardown never waits long on the running flag.
		int r = poll(&pfd, 1, 50);
		if (r < 0 && errno != EINTR) {
			break;
		}
		drain_events(cwl);
		if (xcb_connection_has_error(cwl->conn)) {
			U_LOG_E("comp_window_linux: X connection error — surface input stopped");
			break;
		}
	}
	return NULL;
}

/*!
 * Synchronously process events until the window has the expected size (or the
 * deadline passes). Used during init, before the event thread exists, so the
 * first swapchain is created at the WM-settled fullscreen extent.
 */
static void
settle_size(struct comp_window_linux *cwl, uint32_t want_w, uint32_t want_h, int timeout_ms)
{
	const int64_t deadline = os_monotonic_get_ns() + (int64_t)timeout_ms * 1000000;
	struct pollfd pfd = {.fd = xcb_get_file_descriptor(cwl->conn), .events = POLLIN};
	for (;;) {
		drain_events(cwl);
		pthread_mutex_lock(&cwl->lock);
		bool done = cwl->width == want_w && cwl->height == want_h;
		pthread_mutex_unlock(&cwl->lock);
		if (done) {
			return;
		}
		int64_t left_ns = deadline - os_monotonic_get_ns();
		if (left_ns <= 0) {
			return;
		}
		(void)poll(&pfd, 1, (int)(left_ns / 1000000) + 1);
	}
}


/*
 *
 * comp_target functions.
 *
 */

static bool
comp_window_linux_init_pre_vulkan(struct comp_target *ct)
{
	(void)ct;
	return true;
}

static bool
create_window(struct comp_window_linux *cwl, uint32_t req_w, uint32_t req_h)
{
	int screen_num = 0;
	cwl->conn = xcb_connect(NULL, &screen_num);
	if (cwl->conn == NULL || xcb_connection_has_error(cwl->conn)) {
		U_LOG_E("comp_window_linux: xcb_connect failed — is DISPLAY set (Xorg or XWayland)?");
		if (cwl->conn != NULL) {
			xcb_disconnect(cwl->conn);
			cwl->conn = NULL;
		}
		return false;
	}
	const xcb_setup_t *setup = xcb_get_setup(cwl->conn);
	xcb_screen_iterator_t sit = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < screen_num && sit.rem; i++) {
		xcb_screen_next(&sit);
	}
	xcb_screen_t *screen = sit.data;
	if (screen == NULL) {
		U_LOG_E("comp_window_linux: no X screen");
		return false;
	}
	cwl->root = screen->root;

	// Size + origin from the panel's RandR monitor when it resolves; the
	// caller's request (display_pixel_*) otherwise.
	uint32_t w = req_w, h = req_h;
	int32_t mx = cwl->screen_left, my = cwl->screen_top;
	cwl->fullscreen_monitor =
	    resolve_monitor(cwl->conn, cwl->root, cwl->screen_left, cwl->screen_top, &w, &h, &mx, &my);
	if (cwl->fullscreen_monitor < 0) {
		U_LOG_W("comp_window_linux: no RandR monitor at (%d, %d) — plain %ux%u window", cwl->screen_left,
		        cwl->screen_top, w, h);
	} else {
		cwl->screen_left = mx;
		cwl->screen_top = my;
	}
	if (w == 0 || h == 0) {
		w = 1920;
		h = 1080;
	}

	cwl->window = xcb_generate_id(cwl->conn);
	const uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	const uint32_t values[2] = {screen->black_pixel, CWL_EVENT_MASK};
	xcb_create_window(cwl->conn, XCB_COPY_FROM_PARENT, cwl->window, cwl->root, (int16_t)cwl->screen_left,
	                  (int16_t)cwl->screen_top, (uint16_t)w, (uint16_t)h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
	                  screen->root_visual, value_mask, values);

	// ICCCM position hint so a WM that does honour it places us on the panel.
	uint32_t hints[18] = {0};
	hints[0] = 1 | 4; // USPosition | PPosition
	hints[1] = (uint32_t)cwl->screen_left;
	hints[2] = (uint32_t)cwl->screen_top;
	xcb_change_property(cwl->conn, XCB_PROP_MODE_REPLACE, cwl->window, XCB_ATOM_WM_NORMAL_HINTS,
	                    XCB_ATOM_WM_SIZE_HINTS, 32, 18, hints);

	const char *title = "DisplayXR Workspace";
	xcb_change_property(cwl->conn, XCB_PROP_MODE_REPLACE, cwl->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
	                    (uint32_t)strlen(title), title);
	xcb_atom_t net_wm_name = intern_atom(cwl->conn, "_NET_WM_NAME");
	xcb_atom_t utf8 = intern_atom(cwl->conn, "UTF8_STRING");
	if (net_wm_name != XCB_ATOM_NONE && utf8 != XCB_ATOM_NONE) {
		xcb_change_property(cwl->conn, XCB_PROP_MODE_REPLACE, cwl->window, net_wm_name, utf8, 8,
		                    (uint32_t)strlen(title), title);
	}
	// WM_CLASS "instance\0class\0".
	static const char wm_class[] = "displayxr-service\0DisplayXR";
	xcb_change_property(cwl->conn, XCB_PROP_MODE_REPLACE, cwl->window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
	                    sizeof(wm_class), wm_class);

	cwl->atom_wm_protocols = intern_atom(cwl->conn, "WM_PROTOCOLS");
	cwl->atom_wm_delete_window = intern_atom(cwl->conn, "WM_DELETE_WINDOW");
	if (cwl->atom_wm_protocols != XCB_ATOM_NONE && cwl->atom_wm_delete_window != XCB_ATOM_NONE) {
		xcb_change_property(cwl->conn, XCB_PROP_MODE_REPLACE, cwl->window, cwl->atom_wm_protocols,
		                    XCB_ATOM_ATOM, 32, 1, &cwl->atom_wm_delete_window);
	}
	cwl->atom_net_wm_state = intern_atom(cwl->conn, "_NET_WM_STATE");
	cwl->atom_net_wm_state_fullscreen = intern_atom(cwl->conn, "_NET_WM_STATE_FULLSCREEN");
	cwl->atom_net_wm_fullscreen_monitors = intern_atom(cwl->conn, "_NET_WM_FULLSCREEN_MONITORS");

	// Invisible pointer for when the workspace draws its own cursor sprite.
	xcb_pixmap_t pm = xcb_generate_id(cwl->conn);
	xcb_create_pixmap(cwl->conn, 1, pm, cwl->root, 1, 1);
	cwl->blank_cursor = xcb_generate_id(cwl->conn);
	xcb_create_cursor(cwl->conn, cwl->blank_cursor, pm, pm, 0, 0, 0, 0, 0, 0, 0, 0);
	xcb_free_pixmap(cwl->conn, pm);

	cwl->width = w;
	cwl->height = h;
	load_keymap(cwl);

	map_on_panel(cwl);
	cwl->mapped_requested = true;

	// Let the WM apply the fullscreen geometry before the swapchain is sized.
	settle_size(cwl, w, h, 1000);

	U_LOG_W("comp_window_linux: service surface 0x%08x %ux%u at (%d, %d)%s", (unsigned)cwl->window, cwl->width,
	        cwl->height, cwl->screen_left, cwl->screen_top,
	        cwl->fullscreen_monitor >= 0 ? ", fullscreen on the panel's RandR monitor" : "");
	return true;
}

static bool
comp_window_linux_init_post_vulkan(struct comp_target *ct, uint32_t width, uint32_t height)
{
	struct comp_window_linux *cwl = (struct comp_window_linux *)ct;
	struct vk_bundle *vk = get_vk(cwl);

	if (!create_window(cwl, width, height)) {
		return false;
	}

	PFN_vkCreateXcbSurfaceKHR pfn = (PFN_vkCreateXcbSurfaceKHR)vk->vkCreateXcbSurfaceKHR;
	if (pfn == NULL) {
		pfn = (PFN_vkCreateXcbSurfaceKHR)vk->vkGetInstanceProcAddr(vk->instance, "vkCreateXcbSurfaceKHR");
	}
	if (pfn == NULL) {
		U_LOG_E("comp_window_linux: vkCreateXcbSurfaceKHR unavailable — VK_KHR_xcb_surface must be enabled");
		return false;
	}
	VkXcbSurfaceCreateInfoKHR info = {
	    .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
	    .connection = cwl->conn,
	    .window = cwl->window,
	};
	VkResult ret = pfn(vk->instance, &info, NULL, &cwl->base.surface.handle);
	if (ret != VK_SUCCESS) {
		U_LOG_E("comp_window_linux: vkCreateXcbSurfaceKHR: %s", vk_result_string(ret));
		return false;
	}
	VK_NAME_SURFACE(vk, cwl->base.surface.handle, "comp_window_linux surface");

	// Report the settled window size as the target extent; the caller sizes
	// the swapchain from ct->width/height.
	pthread_mutex_lock(&cwl->lock);
	ct->width = cwl->width;
	ct->height = cwl->height;
	pthread_mutex_unlock(&cwl->lock);

	cwl->thread_running = true;
	if (pthread_create(&cwl->thread, NULL, event_thread_func, cwl) == 0) {
		cwl->thread_started = true;
	} else {
		cwl->thread_running = false;
		U_LOG_E("comp_window_linux: failed to start the event thread — surface input disabled");
	}
	return true;
}

void
comp_window_linux_set_visible(struct comp_target *ct, bool visible)
{
	struct comp_window_linux *cwl = (struct comp_window_linux *)ct;
	if (cwl == NULL || cwl->conn == NULL) {
		return;
	}
	pthread_mutex_lock(&cwl->lock);
	bool changed = cwl->mapped_requested != visible;
	cwl->mapped_requested = visible;
	pthread_mutex_unlock(&cwl->lock);
	if (!changed) {
		return;
	}
	if (visible) {
		map_on_panel(cwl);
	} else {
		xcb_unmap_window(cwl->conn, cwl->window);
		xcb_flush(cwl->conn);
	}
}

void
comp_window_linux_set_cursor_hidden(struct comp_target *ct, bool hidden)
{
	struct comp_window_linux *cwl = (struct comp_window_linux *)ct;
	if (cwl == NULL || cwl->conn == NULL) {
		return;
	}
	pthread_mutex_lock(&cwl->lock);
	bool changed = cwl->cursor_hidden != hidden;
	cwl->cursor_hidden = hidden;
	pthread_mutex_unlock(&cwl->lock);
	if (!changed) {
		return;
	}
	const uint32_t cursor = hidden ? cwl->blank_cursor : XCB_CURSOR_NONE;
	xcb_change_window_attributes(cwl->conn, cwl->window, XCB_CW_CURSOR, &cursor);
	xcb_flush(cwl->conn);
}

static void
comp_window_linux_flush(struct comp_target *ct)
{
	(void)ct;
}

static void
comp_window_linux_set_title(struct comp_target *ct, const char *title)
{
	struct comp_window_linux *cwl = (struct comp_window_linux *)ct;
	if (cwl->conn == NULL || title == NULL) {
		return;
	}
	xcb_change_property(cwl->conn, XCB_PROP_MODE_REPLACE, cwl->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
	                    (uint32_t)strlen(title), title);
	xcb_flush(cwl->conn);
}

static void
comp_window_linux_destroy(struct comp_target *ct)
{
	struct comp_window_linux *cwl = (struct comp_window_linux *)ct;

	if (cwl->thread_started) {
		cwl->thread_running = false;
		pthread_join(cwl->thread, NULL);
		cwl->thread_started = false;
	}

	// Swapchain + surface first: the surface borrows the X connection.
	comp_target_swapchain_cleanup(&cwl->base);

	if (cwl->conn != NULL) {
		if (cwl->window != XCB_NONE) {
			xcb_destroy_window(cwl->conn, cwl->window);
		}
		if (cwl->blank_cursor != XCB_NONE) {
			xcb_free_cursor(cwl->conn, cwl->blank_cursor);
		}
		xcb_flush(cwl->conn);
		xcb_disconnect(cwl->conn);
		cwl->conn = NULL;
	}
	free(cwl->keysyms);
	pthread_mutex_destroy(&cwl->lock);
	free(cwl);
}

struct comp_target *
comp_window_linux_create(struct comp_compositor *c, int32_t screen_left, int32_t screen_top)
{
	struct comp_window_linux *w = U_TYPED_CALLOC(struct comp_window_linux);
	if (w == NULL) {
		return NULL;
	}

	// Display timing is not wired on the service path (no VK_GOOGLE_display_timing
	// over XWayland); force fake pacing as the Android / macOS targets do.
	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);

	pthread_mutex_init(&w->lock, NULL);
	w->screen_left = screen_left;
	w->screen_top = screen_top;
	w->fullscreen_monitor = -1;

	w->base.base.name = "Linux X11";
	w->base.base.destroy = comp_window_linux_destroy;
	w->base.base.flush = comp_window_linux_flush;
	w->base.base.init_pre_vulkan = comp_window_linux_init_pre_vulkan;
	w->base.base.init_post_vulkan = comp_window_linux_init_post_vulkan;
	w->base.base.set_title = comp_window_linux_set_title;
	w->base.base.c = c;

	return &w->base.base;
}
