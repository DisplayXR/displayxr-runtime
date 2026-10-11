// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native-Wayland backend of the desktop-Linux service window (#710).
 * @author David Fattal
 * @ingroup comp_main
 *
 * The service-owned surface the comp_multi shared spatial surface presents to,
 * as a native xdg_toplevel instead of an XWayland window. On a fractionally
 * scaled GNOME desktop this is the only 1:1 path: mutter resamples an XWayland
 * window's buffer, and a resampled weave is a broken weave.
 *
 * The recipe is the one the app-side helper (displayxr-common
 * dxr_linux_window.cpp, hardware-validated on a 3840x2160 panel at 200 %) uses
 * for an app's fullscreen surface, cut down to what a borderless full-screen
 * workspace needs:
 *
 * - **Output**: the panel's wl_output is the one whose device-pixel mode equals
 *   the panel's native size (u_wl_monitor_is_panel; the converted logical
 *   origin breaks ties).
 * - **1:1 buffer**: the swapchain is the output's MODE (device pixels); the
 *   wp_viewport destination is the configured LOGICAL size, so the compositor
 *   maps the whole buffer onto exactly the configured region — 1:1 whenever
 *   buffer = logical x scale, which is what fullscreen on that output gives.
 *   wp_fractional_scale_v1's preferred scale is logged and used to map pointer
 *   coordinates when no configure size is known yet.
 * - **Fullscreen**: mutter discards the output argument of a set_fullscreen
 *   made before the surface has shown a buffer, so the surface maps windowed
 *   (at the panel's logical size) and asks for fullscreen on the panel output
 *   once wl_surface.enter says it is on screen.
 * - **Opaque region** over the whole surface: lets GNOME scan the fullscreen
 *   surface out directly (#1698).
 * - **Input** from wl_seat: keys by evdev code to Windows VK codes (US-layout
 *   key positions; no libxkbcommon), pointer in buffer pixels, wheel notches;
 *   the pointer is hidden (set_cursor NULL) while the workspace draws its own
 *   sprite and restored with cursor-shape-v1.
 * - **Show / hide**: hide attaches a NULL buffer (unmap); show performs a new
 *   initial commit and waits for the configure before the WSI may attach
 *   again, then re-requests fullscreen once mapped.
 *
 * Threads: the event thread owns the default queue (prepare_read / poll /
 * read_events / dispatch_pending), so it coexists with Mesa's WSI, which reads
 * the same wl_display on its own queue from the render thread. Requests are
 * thread-safe in libwayland.
 */

#include "xrt/xrt_compiler.h"

#include "os/os_time.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_wayland_geom.h"

#include "vk/vk_helpers.h"

#include "main/comp_compositor.h"
#include "main/comp_target_swapchain.h"
#include "main/comp_window_linux.h"
#include "main/comp_window_linux_private.h"

#include <wayland-client.h>
#include <vulkan/vulkan_wayland.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"

#include <linux/input-event-codes.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define CWLW_MAX_OUTPUTS 8

struct cwlw_output
{
	uint32_t name;
	struct wl_output *output;
	struct zxdg_output_v1 *xdg_output;
	int32_t mode_w, mode_h;
	int32_t refresh_mhz;
	int32_t logical_x, logical_y;
	int32_t logical_w, logical_h;
	bool have_logical_pos, have_logical_size;
	char label[64];
};

/*!
 * A native-Wayland service window.
 *
 * @implements comp_target_swapchain
 */
struct comp_window_linux_wl
{
	//! Common initial sequence with comp_window_linux_base (keep first, in order).
	struct comp_target_swapchain base;
	const struct comp_window_linux_ops *ops;

	struct comp_window_linux_placement place;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wp_viewporter *viewporter;
	struct wp_fractional_scale_manager_v1 *frac_manager;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct wp_cursor_shape_manager_v1 *cursor_shape_manager;

	struct cwlw_output outputs[CWLW_MAX_OUTPUTS];
	uint32_t output_count;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wp_viewport *viewport;
	struct wp_fractional_scale_v1 *frac;
	struct wp_cursor_shape_device_v1 *cursor_shape_device;

	struct wl_output *panel_output; //!< NULL = compositor's choice.
	int32_t buf_w, buf_h;           //!< Swapchain extent, device px (the panel mode).
	int32_t panel_lw, panel_lh;     //!< Panel output's logical size (windowed map size).

	//! Guards everything below (event thread vs render / IPC threads).
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int32_t cfg_w, cfg_h; //!< Last toplevel configure size, logical (0 = compositor did not say).
	int32_t dst_w, dst_h; //!< Current viewport destination, logical.
	bool configured;      //!< Configure acked since the last (re)map request.
	bool mapped_requested;
	bool fs_pending; //!< Ask for fullscreen on the panel once mapped.
	bool fullscreen;
	bool entered;
	int fs_wait_ticks;
	uint32_t pref_scale_120;
	bool cursor_hidden;
	bool pointer_in;
	uint32_t pointer_serial;
	int32_t ptr_x, ptr_y; //!< Last pointer position, buffer px.
	uint32_t button_mask;
	uint32_t mods;
	bool axis_discrete_seen;
	double axis_accum;

	pthread_t thread;
	bool thread_started;
	volatile bool running;
};


/*
 *
 * evdev -> Windows VK (US-layout key positions).
 *
 */

uint32_t
comp_window_linux_evdev_to_vk(uint32_t k)
{
	static const char row1[] = "1234567890"; // KEY_1 (2) .. KEY_0 (11)
	static const char rowq[] = "QWERTYUIOP"; // KEY_Q (16) .. KEY_P (25)
	static const char rowa[] = "ASDFGHJKL";  // KEY_A (30) .. KEY_L (38)
	static const char rowz[] = "ZXCVBNM";    // KEY_Z (44) .. KEY_M (50)
	if (k >= KEY_1 && k <= KEY_0) {
		return (uint32_t)row1[k - KEY_1];
	}
	if (k >= KEY_Q && k <= KEY_P) {
		return (uint32_t)rowq[k - KEY_Q];
	}
	if (k >= KEY_A && k <= KEY_L) {
		return (uint32_t)rowa[k - KEY_A];
	}
	if (k >= KEY_Z && k <= KEY_M) {
		return (uint32_t)rowz[k - KEY_Z];
	}
	if (k >= KEY_F1 && k <= KEY_F10) {
		return 0x70 + (k - KEY_F1);
	}
	switch (k) {
	case KEY_ESC: return 0x1B;
	case KEY_MINUS: return 0xBD;
	case KEY_EQUAL: return 0xBB;
	case KEY_BACKSPACE: return 0x08;
	case KEY_TAB: return 0x09;
	case KEY_LEFTBRACE: return 0xDB;
	case KEY_RIGHTBRACE: return 0xDD;
	case KEY_ENTER: return 0x0D;
	case KEY_KPENTER: return 0x0D;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL: return 0x11;
	case KEY_SEMICOLON: return 0xBA;
	case KEY_APOSTROPHE: return 0xDE;
	case KEY_GRAVE: return 0xC0;
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT: return 0x10;
	case KEY_BACKSLASH: return 0xDC;
	case KEY_COMMA: return 0xBC;
	case KEY_DOT: return 0xBE;
	case KEY_SLASH: return 0xBF;
	case KEY_LEFTALT:
	case KEY_RIGHTALT: return 0x12;
	case KEY_SPACE: return 0x20;
	case KEY_F11: return 0x7A;
	case KEY_F12: return 0x7B;
	case KEY_HOME: return 0x24;
	case KEY_UP: return 0x26;
	case KEY_PAGEUP: return 0x21;
	case KEY_LEFT: return 0x25;
	case KEY_RIGHT: return 0x27;
	case KEY_END: return 0x23;
	case KEY_DOWN: return 0x28;
	case KEY_PAGEDOWN: return 0x22;
	case KEY_INSERT: return 0x2D;
	case KEY_DELETE: return 0x2E;
	case KEY_LEFTMETA: return 0x5B;
	case KEY_RIGHTMETA: return 0x5C;
	case KEY_KP0: return 0x60;
	case KEY_KP1: return 0x61;
	case KEY_KP2: return 0x62;
	case KEY_KP3: return 0x63;
	case KEY_KP4: return 0x64;
	case KEY_KP5: return 0x65;
	case KEY_KP6: return 0x66;
	case KEY_KP7: return 0x67;
	case KEY_KP8: return 0x68;
	case KEY_KP9: return 0x69;
	case KEY_KPPLUS: return 0x6B;
	case KEY_KPMINUS: return 0x6D;
	case KEY_KPDOT: return 0x2E;
	default: return 0;
	}
}


/*
 *
 * Small helpers.
 *
 */

static inline struct vk_bundle *
get_vk(struct comp_window_linux_wl *w)
{
	return &w->base.base.c->base.vk;
}

static struct cwlw_output *
find_output(struct comp_window_linux_wl *w, struct wl_output *o)
{
	for (uint32_t i = 0; i < w->output_count; i++) {
		if (w->outputs[i].output == o) {
			return &w->outputs[i];
		}
	}
	return NULL;
}

//! Logical surface coordinate -> buffer (device) px. Caller holds lock.
static int32_t
to_buffer_px(const struct comp_window_linux_wl *w, double logical, int32_t buf, int32_t dst)
{
	if (dst > 0) {
		return (int32_t)(logical * (double)buf / (double)dst);
	}
	if (w->pref_scale_120 > 0) {
		return (int32_t)(logical * (double)w->pref_scale_120 / 120.0);
	}
	return (int32_t)logical;
}

//! Opaque region over the whole (logical) surface: direct scanout (#1698).
static void
set_opaque_region(struct comp_window_linux_wl *w, int32_t lw, int32_t lh)
{
	if (w->compositor == NULL || lw <= 0 || lh <= 0) {
		return;
	}
	struct wl_region *r = wl_compositor_create_region(w->compositor);
	wl_region_add(r, 0, 0, lw, lh);
	wl_surface_set_opaque_region(w->surface, r);
	wl_region_destroy(r);
}

//! Ask for fullscreen on the panel output. Caller holds lock.
static void
request_fullscreen_locked(struct comp_window_linux_wl *w, const char *why)
{
	if (w->toplevel == NULL) {
		return;
	}
	xdg_toplevel_set_fullscreen(w->toplevel, w->panel_output);
	w->fs_pending = false;
	struct cwlw_output *o = w->panel_output != NULL ? find_output(w, w->panel_output) : NULL;
	U_LOG_W("comp_window_linux(wayland): set_fullscreen on %s (%s)",
	        o != NULL ? o->label : "the compositor's choice", why);
}

static void
apply_cursor_locked(struct comp_window_linux_wl *w)
{
	if (w->pointer == NULL || !w->pointer_in) {
		return;
	}
	if (w->cursor_hidden) {
		wl_pointer_set_cursor(w->pointer, w->pointer_serial, NULL, 0, 0);
	} else if (w->cursor_shape_device != NULL) {
		wp_cursor_shape_device_v1_set_shape(w->cursor_shape_device, w->pointer_serial,
		                                    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
	}
}


/*
 *
 * Output listeners.
 *
 */

static void
out_geometry(void *data,
             struct wl_output *o,
             int32_t x,
             int32_t y,
             int32_t pw,
             int32_t ph,
             int32_t sub,
             const char *make,
             const char *model,
             int32_t transform)
{
	struct cwlw_output *out = find_output(data, o);
	if (out != NULL && out->label[0] == '\0') {
		snprintf(out->label, sizeof(out->label), "%s %s", make != NULL ? make : "?",
		         model != NULL ? model : "?");
	}
}

static void
out_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	struct cwlw_output *out = find_output(data, o);
	if (out != NULL && (flags & WL_OUTPUT_MODE_CURRENT)) {
		out->mode_w = w;
		out->mode_h = h;
		out->refresh_mhz = refresh;
	}
}

static void
out_done(void *data, struct wl_output *o)
{}

static void
out_scale(void *data, struct wl_output *o, int32_t factor)
{}

static void
out_name(void *data, struct wl_output *o, const char *name)
{
	struct cwlw_output *out = find_output(data, o);
	if (out != NULL && name != NULL) {
		snprintf(out->label, sizeof(out->label), "%s", name);
	}
}

static void
out_description(void *data, struct wl_output *o, const char *desc)
{}

static const struct wl_output_listener output_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_description,
};

struct xdg_out_ctx
{
	struct comp_window_linux_wl *w;
	uint32_t index;
};

static struct cwlw_output *
find_xdg_output(struct comp_window_linux_wl *w, struct zxdg_output_v1 *xo)
{
	for (uint32_t i = 0; i < w->output_count; i++) {
		if (w->outputs[i].xdg_output == xo) {
			return &w->outputs[i];
		}
	}
	return NULL;
}

static void
xout_logical_position(void *data, struct zxdg_output_v1 *xo, int32_t x, int32_t y)
{
	struct cwlw_output *out = find_xdg_output(data, xo);
	if (out != NULL) {
		out->logical_x = x;
		out->logical_y = y;
		out->have_logical_pos = true;
	}
}

static void
xout_logical_size(void *data, struct zxdg_output_v1 *xo, int32_t w, int32_t h)
{
	struct cwlw_output *out = find_xdg_output(data, xo);
	if (out != NULL) {
		out->logical_w = w;
		out->logical_h = h;
		out->have_logical_size = true;
	}
}

static void
xout_done(void *data, struct zxdg_output_v1 *xo)
{}

static void
xout_name(void *data, struct zxdg_output_v1 *xo, const char *name)
{
	struct cwlw_output *out = find_xdg_output(data, xo);
	if (out != NULL && name != NULL) {
		snprintf(out->label, sizeof(out->label), "%s", name);
	}
}

static void
xout_description(void *data, struct zxdg_output_v1 *xo, const char *desc)
{}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    xout_logical_position, xout_logical_size, xout_done, xout_name, xout_description,
};

static void
attach_xdg_output(struct comp_window_linux_wl *w, struct cwlw_output *out)
{
	if (w->xdg_output_manager == NULL || out->xdg_output != NULL || out->output == NULL) {
		return;
	}
	out->xdg_output = zxdg_output_manager_v1_get_xdg_output(w->xdg_output_manager, out->output);
	zxdg_output_v1_add_listener(out->xdg_output, &xdg_output_listener, w);
}


/*
 *
 * Seat: pointer + keyboard.
 *
 */

static void
ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy)
{
	struct comp_window_linux_wl *w = data;
	pthread_mutex_lock(&w->lock);
	w->pointer_in = true;
	w->pointer_serial = serial;
	w->ptr_x = to_buffer_px(w, wl_fixed_to_double(sx), w->buf_w, w->dst_w);
	w->ptr_y = to_buffer_px(w, wl_fixed_to_double(sy), w->buf_h, w->dst_h);
	apply_cursor_locked(w);
	pthread_mutex_unlock(&w->lock);
}

static void
ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	struct comp_window_linux_wl *w = data;
	pthread_mutex_lock(&w->lock);
	w->pointer_in = false;
	pthread_mutex_unlock(&w->lock);
}

static void
ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	struct comp_window_linux_wl *w = data;
	struct comp_window_linux_input in = {0};
	pthread_mutex_lock(&w->lock);
	w->ptr_x = to_buffer_px(w, wl_fixed_to_double(sx), w->buf_w, w->dst_w);
	w->ptr_y = to_buffer_px(w, wl_fixed_to_double(sy), w->buf_h, w->dst_h);
	in.type = COMP_WINDOW_LINUX_INPUT_MOTION;
	in.timestamp_ms = time;
	in.x = w->ptr_x;
	in.y = w->ptr_y;
	in.button_mask = w->button_mask;
	in.modifiers = w->mods;
	pthread_mutex_unlock(&w->lock);
	comp_window_linux_emit_input(&in);
}

static void
ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	struct comp_window_linux_wl *w = data;
	uint32_t b = 0, bit = 0;
	switch (button) {
	case BTN_LEFT: b = 1, bit = 1u << 0; break;
	case BTN_RIGHT: b = 2, bit = 1u << 1; break;
	case BTN_MIDDLE: b = 3, bit = 1u << 2; break;
	default: return;
	}
	const bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
	struct comp_window_linux_input in = {0};
	pthread_mutex_lock(&w->lock);
	if (down) {
		w->button_mask |= bit;
	} else {
		w->button_mask &= ~bit;
	}
	in.type = COMP_WINDOW_LINUX_INPUT_BUTTON;
	in.timestamp_ms = time;
	in.button = b;
	in.is_down = down;
	in.x = w->ptr_x;
	in.y = w->ptr_y;
	in.modifiers = w->mods;
	pthread_mutex_unlock(&w->lock);
	comp_window_linux_emit_input(&in);
}

static void
emit_scroll(struct comp_window_linux_wl *w, uint32_t time, float notches)
{
	struct comp_window_linux_input in = {0};
	pthread_mutex_lock(&w->lock);
	in.type = COMP_WINDOW_LINUX_INPUT_SCROLL;
	in.timestamp_ms = time;
	in.scroll_delta = notches;
	in.x = w->ptr_x;
	in.y = w->ptr_y;
	in.modifiers = w->mods;
	pthread_mutex_unlock(&w->lock);
	comp_window_linux_emit_input(&in);
}

static void
ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	struct comp_window_linux_wl *w = data;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}
	if (w->axis_discrete_seen) {
		return; // this frame's notches already reported by axis_discrete
	}
	// Continuous source (touchpad): 10 units ~ one wheel notch (libinput's
	// convention). Wayland's positive is down; the wire's positive is up.
	w->axis_accum += wl_fixed_to_double(value);
	while (w->axis_accum >= 10.0) {
		w->axis_accum -= 10.0;
		emit_scroll(w, time, -1.0f);
	}
	while (w->axis_accum <= -10.0) {
		w->axis_accum += 10.0;
		emit_scroll(w, time, 1.0f);
	}
}

static void
ptr_frame(void *data, struct wl_pointer *p)
{
	struct comp_window_linux_wl *w = data;
	w->axis_discrete_seen = false;
}

static void
ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{}

static void
ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
	struct comp_window_linux_wl *w = data;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		w->axis_accum = 0.0;
	}
}

static void
ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete)
{
	struct comp_window_linux_wl *w = data;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || discrete == 0) {
		return;
	}
	w->axis_discrete_seen = true;
	emit_scroll(w, 0, (float)-discrete);
}

// Seat bound at v5: no axis_value120 / axis_relative_direction events.
static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

static void
kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
	// Keys are decoded by evdev position (no libxkbcommon); the keymap is unused.
	close(fd);
}

static void
kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{}

static void
kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s)
{
	struct comp_window_linux_wl *w = data;
	pthread_mutex_lock(&w->lock);
	w->mods = 0;
	pthread_mutex_unlock(&w->lock);
	struct comp_window_linux_input in = {0};
	in.type = COMP_WINDOW_LINUX_INPUT_FOCUS_LOST;
	comp_window_linux_emit_input(&in);
}

static void
kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	struct comp_window_linux_wl *w = data;
	const uint32_t vk = comp_window_linux_evdev_to_vk(key);
	if (vk == 0) {
		return;
	}
	// Wayland sends no auto-repeat (the client synthesises it), matching the
	// one-press-one-event contract the X11 backend gets by dropping repeats.
	struct comp_window_linux_input in = {0};
	pthread_mutex_lock(&w->lock);
	in.type = COMP_WINDOW_LINUX_INPUT_KEY;
	in.timestamp_ms = time;
	in.vk_code = vk;
	in.keysym = key; // evdev code (diagnostic)
	in.is_down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	in.modifiers = w->mods;
	in.x = w->ptr_x;
	in.y = w->ptr_y;
	pthread_mutex_unlock(&w->lock);
	comp_window_linux_emit_input(&in);
}

static void
kb_modifiers(void *data,
             struct wl_keyboard *kb,
             uint32_t serial,
             uint32_t depressed,
             uint32_t latched,
             uint32_t locked,
             uint32_t group)
{
	struct comp_window_linux_wl *w = data;
	// The conventional xkb real-modifier bits (Shift 1, Control 4, Mod1 8) —
	// what every common keymap uses; without libxkbcommon there is no keymap
	// lookup of the names.
	const uint32_t m = depressed | latched;
	uint32_t mods = 0;
	if (m & 0x1) {
		mods |= 1u << 0;
	}
	if (m & 0x4) {
		mods |= 1u << 1;
	}
	if (m & 0x8) {
		mods |= 1u << 2;
	}
	pthread_mutex_lock(&w->lock);
	w->mods = mods;
	pthread_mutex_unlock(&w->lock);
}

static void
kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{}

static const struct wl_keyboard_listener keyboard_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat_info,
};

static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	struct comp_window_linux_wl *w = data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && w->pointer == NULL) {
		w->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(w->pointer, &pointer_listener, w);
		if (w->cursor_shape_manager != NULL) {
			w->cursor_shape_device =
			    wp_cursor_shape_manager_v1_get_pointer(w->cursor_shape_manager, w->pointer);
		}
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && w->keyboard == NULL) {
		w->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(w->keyboard, &keyboard_listener, w);
	}
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name)
{}

static const struct wl_seat_listener seat_listener = {seat_capabilities, seat_name};


/*
 *
 * Shell + surface listeners.
 *
 */

static void
wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {wm_base_ping};

static void
toplevel_configure(void *data, struct xdg_toplevel *t, int32_t width, int32_t height, struct wl_array *states)
{
	struct comp_window_linux_wl *w = data;
	bool fs = false;
	const uint32_t *st = states->data;
	for (size_t i = 0; i < states->size / sizeof(uint32_t); i++) {
		fs |= st[i] == XDG_TOPLEVEL_STATE_FULLSCREEN;
	}
	pthread_mutex_lock(&w->lock);
	w->cfg_w = width;
	w->cfg_h = height;
	if (fs != w->fullscreen) {
		w->fullscreen = fs;
		U_LOG_W("comp_window_linux(wayland): %s (configure %dx%d logical)",
		        fs ? "fullscreen" : "not fullscreen", width, height);
	}
	pthread_mutex_unlock(&w->lock);
}

static void
toplevel_close(void *data, struct xdg_toplevel *t)
{
	// The surface is the workspace itself, not an app window; the controller
	// owns its lifecycle (as on X11).
	U_LOG_W("comp_window_linux(wayland): ignoring a compositor close request on the workspace surface");
}

static void
toplevel_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{}

static void
toplevel_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *caps)
{}

static const struct xdg_toplevel_listener toplevel_listener = {
    toplevel_configure,
    toplevel_close,
    toplevel_configure_bounds,
    toplevel_wm_capabilities,
};

static void
xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	struct comp_window_linux_wl *w = data;
	pthread_mutex_lock(&w->lock);
	xdg_surface_ack_configure(s, serial);
	// 0x0 = "you choose": the panel's logical size (the buffer is its mode, so
	// that maps 1:1 once on the panel).
	const int32_t lw = w->cfg_w > 0 ? w->cfg_w : w->panel_lw;
	const int32_t lh = w->cfg_h > 0 ? w->cfg_h : w->panel_lh;
	if (w->viewport != NULL && (lw != w->dst_w || lh != w->dst_h)) {
		// Pending state; the WSI's next present commits it with the buffer.
		wp_viewport_set_destination(w->viewport, lw, lh);
		set_opaque_region(w, lw, lh);
		U_LOG_W("comp_window_linux(wayland): %dx%d buffer -> wp_viewport destination %dx%d logical%s", w->buf_w,
		        w->buf_h, lw, lh, w->fullscreen ? " (fullscreen)" : "");
		w->dst_w = lw;
		w->dst_h = lh;
	}
	w->configured = true;
	pthread_cond_broadcast(&w->cond);
	pthread_mutex_unlock(&w->lock);
}

static const struct xdg_surface_listener xdg_surface_listener = {xdg_surface_configure};

static void
surface_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
	struct comp_window_linux_wl *w = data;
	pthread_mutex_lock(&w->lock);
	w->entered = true;
	struct cwlw_output *out = find_output(w, o);
	U_LOG_I("comp_window_linux(wayland): surface entered %s", out != NULL ? out->label : "?");
	if (w->fs_pending && w->mapped_requested) {
		request_fullscreen_locked(w, "the surface is mapped");
	}
	pthread_mutex_unlock(&w->lock);
}

static void
surface_leave(void *data, struct wl_surface *s, struct wl_output *o)
{}

// wl_compositor bound at v4: no preferred_buffer_scale / _transform events.
static const struct wl_surface_listener surface_listener = {.enter = surface_enter, .leave = surface_leave};

static void
frac_preferred_scale(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale_120)
{
	struct comp_window_linux_wl *w = data;
	pthread_mutex_lock(&w->lock);
	if (scale_120 != w->pref_scale_120) {
		w->pref_scale_120 = scale_120;
		U_LOG_W(
		    "comp_window_linux(wayland): compositor's preferred surface scale %.4f "
		    "(wp_fractional_scale_v1)",
		    (double)scale_120 / 120.0);
	}
	pthread_mutex_unlock(&w->lock);
}

static const struct wp_fractional_scale_v1_listener frac_listener = {frac_preferred_scale};


/*
 *
 * Registry.
 *
 */

static void
registry_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	struct comp_window_linux_wl *w = data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		w->compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		w->wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, version < 4 ? version : 4);
		xdg_wm_base_add_listener(w->wm_base, &wm_base_listener, w);
	} else if (strcmp(iface, wl_seat_interface.name) == 0 && w->seat == NULL) {
		w->seat = wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(w->seat, &seat_listener, w);
	} else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
		w->viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	} else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		w->frac_manager = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		w->cursor_shape_manager = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
	} else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
		w->xdg_output_manager =
		    wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, version < 2 ? version : 2);
		for (uint32_t i = 0; i < w->output_count; i++) {
			attach_xdg_output(w, &w->outputs[i]);
		}
	} else if (strcmp(iface, wl_output_interface.name) == 0 && w->output_count < CWLW_MAX_OUTPUTS) {
		struct cwlw_output *out = &w->outputs[w->output_count++];
		memset(out, 0, sizeof(*out));
		out->name = name;
		out->output = wl_registry_bind(r, name, &wl_output_interface, version < 4 ? version : 4);
		wl_output_add_listener(out->output, &output_listener, w);
		attach_xdg_output(w, out);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
	struct comp_window_linux_wl *w = data;
	pthread_mutex_lock(&w->lock);
	for (uint32_t i = 0; i < w->output_count; i++) {
		if (w->outputs[i].name == name) {
			if (w->outputs[i].output == w->panel_output) {
				w->panel_output = NULL; // a hot-unplugged panel: the compositor's choice from now on
			}
			if (w->outputs[i].xdg_output != NULL) {
				zxdg_output_v1_destroy(w->outputs[i].xdg_output);
			}
			w->outputs[i] = w->outputs[--w->output_count];
			break;
		}
	}
	pthread_mutex_unlock(&w->lock);
}

static const struct wl_registry_listener registry_listener = {registry_global, registry_global_remove};


/*
 *
 * Availability probe.
 *
 */

struct probe_globals
{
	bool wm_base, viewporter, frac;
};

static void
probe_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	struct probe_globals *g = data;
	g->wm_base |= strcmp(iface, "xdg_wm_base") == 0;
	g->viewporter |= strcmp(iface, "wp_viewporter") == 0;
	g->frac |= strcmp(iface, "wp_fractional_scale_manager_v1") == 0;
}

static void
probe_global_remove(void *data, struct wl_registry *r, uint32_t name)
{}

bool
comp_window_linux_wayland_available(void)
{
	const char *wd = getenv("WAYLAND_DISPLAY");
	if (wd == NULL || wd[0] == '\0') {
		U_LOG_I("comp_window_linux: WAYLAND_DISPLAY unset — X11 service surface");
		return false;
	}
	struct wl_display *d = wl_display_connect(NULL);
	if (d == NULL) {
		U_LOG_W("comp_window_linux: WAYLAND_DISPLAY='%s' but no compositor answers — X11 service surface", wd);
		return false;
	}
	struct probe_globals g = {0};
	static const struct wl_registry_listener l = {probe_global, probe_global_remove};
	struct wl_registry *r = wl_display_get_registry(d);
	wl_registry_add_listener(r, &l, &g);
	wl_display_roundtrip(d);
	wl_registry_destroy(r);
	wl_display_disconnect(d);
	const bool ok = g.wm_base && g.viewporter && g.frac;
	U_LOG_W(
	    "comp_window_linux: Wayland compositor on '%s': xdg_wm_base %s, wp_viewporter %s, "
	    "wp_fractional_scale_v1 %s -> %s service surface",
	    wd, g.wm_base ? "yes" : "NO", g.viewporter ? "yes" : "NO", g.frac ? "yes" : "NO",
	    ok ? "native Wayland" : "X11 (XWayland)");
	return ok;
}


/*
 *
 * Event thread.
 *
 */

static void *
event_thread_func(void *ptr)
{
	struct comp_window_linux_wl *w = ptr;
	struct pollfd pfd = {.fd = wl_display_get_fd(w->display), .events = POLLIN};

	while (w->running) {
		while (wl_display_prepare_read(w->display) != 0) {
			wl_display_dispatch_pending(w->display);
		}
		wl_display_flush(w->display);
		int r = poll(&pfd, 1, 50);
		if (r > 0 && (pfd.revents & POLLIN)) {
			wl_display_read_events(w->display);
		} else {
			wl_display_cancel_read(w->display);
		}
		wl_display_dispatch_pending(w->display);

		if (wl_display_get_error(w->display) != 0) {
			U_LOG_E("comp_window_linux(wayland): display error %d — surface input stopped",
			        wl_display_get_error(w->display));
			break;
		}

		// Never mapped within ~4 s (no frame presented?): ask anyway rather
		// than never — the configure log says what the compositor did.
		pthread_mutex_lock(&w->lock);
		if (w->fs_pending && w->mapped_requested && w->configured && ++w->fs_wait_ticks >= 80) {
			request_fullscreen_locked(w, "no wl_surface.enter after ~4 s — requesting anyway");
		}
		pthread_mutex_unlock(&w->lock);
	}
	return NULL;
}


/*
 *
 * comp_target functions.
 *
 */

static bool
cwlw_init_pre_vulkan(struct comp_target *ct)
{
	(void)ct;
	return true;
}

//! Pick the panel's output and the buffer / logical sizes.
static void
resolve_panel(struct comp_window_linux_wl *w)
{
	struct cwlw_output *exact = NULL, *size_match = NULL;
	uint32_t size_matches = 0;
	for (uint32_t i = 0; i < w->output_count; i++) {
		struct cwlw_output *o = &w->outputs[i];
		struct u_wl_monitor mon = {0};
		mon.logical_x = o->logical_x;
		mon.logical_y = o->logical_y;
		mon.logical_w = o->have_logical_size ? o->logical_w : 0;
		mon.logical_h = o->have_logical_size ? o->logical_h : 0;
		mon.mode_w = o->mode_w;
		mon.mode_h = o->mode_h;
		bool origin = false;
		if (!u_wl_monitor_is_panel(&mon, w->place.screen_left, w->place.screen_top, w->place.width,
		                           w->place.height, &origin)) {
			continue;
		}
		size_matches++;
		if (size_match == NULL) {
			size_match = o;
		}
		if (origin && exact == NULL) {
			exact = o;
		}
	}
	struct cwlw_output *picked = exact != NULL ? exact : (size_matches == 1 ? size_match : NULL);
	if (picked == NULL && w->output_count == 1) {
		picked = &w->outputs[0]; // a single output is the panel by elimination
	}
	if (picked != NULL) {
		w->panel_output = picked->output;
		w->buf_w = picked->mode_w;
		w->buf_h = picked->mode_h;
		w->panel_lw = picked->have_logical_size ? picked->logical_w : picked->mode_w;
		w->panel_lh = picked->have_logical_size ? picked->logical_h : picked->mode_h;
		U_LOG_W(
		    "comp_window_linux(wayland): 3D panel output %s: mode %dx%d device px, logical %dx%d at "
		    "(%d, %d)%s",
		    picked->label, picked->mode_w, picked->mode_h, w->panel_lw, w->panel_lh, picked->logical_x,
		    picked->logical_y, exact != NULL ? "" : " (matched by size only)");
	} else {
		U_LOG_W(
		    "comp_window_linux(wayland): no wl_output is the panel's %ux%u device px (%u output(s)) — "
		    "fullscreen on the compositor's choice; the mapping may not be 1:1",
		    w->place.width, w->place.height, w->output_count);
	}
	if (w->buf_w <= 0 || w->buf_h <= 0) {
		w->buf_w = w->place.width > 0 ? (int32_t)w->place.width : 1920;
		w->buf_h = w->place.height > 0 ? (int32_t)w->place.height : 1080;
	}
	if (w->panel_lw <= 0 || w->panel_lh <= 0) {
		w->panel_lw = w->buf_w;
		w->panel_lh = w->buf_h;
	}
}

//! Wait (bounded) for the configure that follows an initial commit.
static bool
wait_configured(struct comp_window_linux_wl *w, int timeout_ms, bool dispatch_here)
{
	const int64_t deadline = os_monotonic_get_ns() + (int64_t)timeout_ms * 1000000;
	if (dispatch_here) {
		// Before the event thread exists: drive the default queue ourselves.
		while (!w->configured && os_monotonic_get_ns() < deadline) {
			if (wl_display_roundtrip(w->display) < 0) {
				return false;
			}
		}
		return w->configured;
	}
	pthread_mutex_lock(&w->lock);
	while (!w->configured) {
		int64_t left = deadline - os_monotonic_get_ns();
		if (left <= 0) {
			break;
		}
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += left % 1000000000;
		ts.tv_sec += left / 1000000000 + ts.tv_nsec / 1000000000;
		ts.tv_nsec %= 1000000000;
		pthread_cond_timedwait(&w->cond, &w->lock, &ts);
	}
	bool ok = w->configured;
	pthread_mutex_unlock(&w->lock);
	return ok;
}

static bool
cwlw_init_post_vulkan(struct comp_target *ct, uint32_t width, uint32_t height)
{
	struct comp_window_linux_wl *w = (struct comp_window_linux_wl *)ct;
	struct vk_bundle *vk = get_vk(w);
	(void)width;
	(void)height;

	w->display = wl_display_connect(NULL);
	if (w->display == NULL) {
		U_LOG_E("comp_window_linux(wayland): wl_display_connect failed");
		return false;
	}
	w->registry = wl_display_get_registry(w->display);
	wl_registry_add_listener(w->registry, &registry_listener, w);
	wl_display_roundtrip(w->display); // globals
	wl_display_roundtrip(w->display); // output modes, xdg_output logical geometry, seat caps
	if (w->compositor == NULL || w->wm_base == NULL) {
		U_LOG_E("comp_window_linux(wayland): compositor lacks wl_compositor / xdg_wm_base");
		return false;
	}
	resolve_panel(w);

	w->surface = wl_compositor_create_surface(w->compositor);
	wl_surface_add_listener(w->surface, &surface_listener, w);
	if (w->viewporter != NULL) {
		w->viewport = wp_viewporter_get_viewport(w->viewporter, w->surface);
	}
	if (w->frac_manager != NULL) {
		w->frac = wp_fractional_scale_manager_v1_get_fractional_scale(w->frac_manager, w->surface);
		wp_fractional_scale_v1_add_listener(w->frac, &frac_listener, w);
	}
	w->xdg_surface = xdg_wm_base_get_xdg_surface(w->wm_base, w->surface);
	xdg_surface_add_listener(w->xdg_surface, &xdg_surface_listener, w);
	w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
	xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);
	xdg_toplevel_set_title(w->toplevel, "DisplayXR Workspace");
	xdg_toplevel_set_app_id(w->toplevel, "displayxr-service");

	// Map windowed at the panel's logical size and ask for fullscreen on the
	// panel output once a buffer is on screen (mutter drops the output of a
	// pre-map set_fullscreen).
	w->mapped_requested = true;
	w->fs_pending = true;
	wl_surface_commit(w->surface); // initial commit, no buffer
	if (!wait_configured(w, 1000, true)) {
		U_LOG_E("comp_window_linux(wayland): no xdg_surface.configure within 1 s");
		return false;
	}

	PFN_vkCreateWaylandSurfaceKHR pfn = (PFN_vkCreateWaylandSurfaceKHR)vk->vkCreateWaylandSurfaceKHR;
	if (pfn == NULL) {
		pfn =
		    (PFN_vkCreateWaylandSurfaceKHR)vk->vkGetInstanceProcAddr(vk->instance, "vkCreateWaylandSurfaceKHR");
	}
	if (pfn == NULL) {
		U_LOG_E(
		    "comp_window_linux(wayland): vkCreateWaylandSurfaceKHR unavailable — VK_KHR_wayland_surface must "
		    "be enabled");
		return false;
	}
	VkWaylandSurfaceCreateInfoKHR info = {
	    .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
	    .display = w->display,
	    .surface = w->surface,
	};
	VkResult ret = pfn(vk->instance, &info, NULL, &w->base.surface.handle);
	if (ret != VK_SUCCESS) {
		U_LOG_E("comp_window_linux(wayland): vkCreateWaylandSurfaceKHR: %s", vk_result_string(ret));
		return false;
	}
	VK_NAME_SURFACE(vk, w->base.surface.handle, "comp_window_linux wayland surface");

	// The swapchain is the panel's MODE: a wl_surface has no intrinsic size
	// (currentExtent = UINT32_MAX), so this IS the buffer size.
	ct->width = (uint32_t)w->buf_w;
	ct->height = (uint32_t)w->buf_h;

	w->running = true;
	if (pthread_create(&w->thread, NULL, event_thread_func, w) == 0) {
		w->thread_started = true;
	} else {
		w->running = false;
		U_LOG_E("comp_window_linux(wayland): failed to start the event thread — input + fullscreen disabled");
	}
	U_LOG_W("comp_window_linux(wayland): service surface %dx%d device px, app_id displayxr-service", w->buf_w,
	        w->buf_h);
	return true;
}

static void
cwlw_set_visible(struct comp_target *ct, bool visible)
{
	struct comp_window_linux_wl *w = (struct comp_window_linux_wl *)ct;
	if (w == NULL || w->surface == NULL) {
		return;
	}
	pthread_mutex_lock(&w->lock);
	if (w->mapped_requested == visible) {
		pthread_mutex_unlock(&w->lock);
		return;
	}
	w->mapped_requested = visible;
	w->configured = false;
	w->entered = false;
	w->fullscreen = false;
	w->fs_wait_ticks = 0;
	w->fs_pending = visible;
	w->dst_w = w->dst_h = 0; // re-sent with the next configure
	if (!visible) {
		// Unmap: a NULL buffer. The WSI is not presenting (the render loop
		// skips a hidden surface), so nothing races this attach.
		wl_surface_attach(w->surface, NULL, 0, 0);
		wl_surface_commit(w->surface);
	} else {
		// A re-map is a new initial commit; no buffer may be attached until
		// its configure is acked (the event thread does that).
		wl_surface_commit(w->surface);
	}
	wl_display_flush(w->display);
	pthread_mutex_unlock(&w->lock);

	if (visible && !wait_configured(w, 1000, false)) {
		U_LOG_W("comp_window_linux(wayland): no configure within 1 s of re-mapping the surface");
	}
}

static void
cwlw_set_cursor_hidden(struct comp_target *ct, bool hidden)
{
	struct comp_window_linux_wl *w = (struct comp_window_linux_wl *)ct;
	if (w == NULL || w->display == NULL) {
		return;
	}
	pthread_mutex_lock(&w->lock);
	if (w->cursor_hidden != hidden) {
		w->cursor_hidden = hidden;
		apply_cursor_locked(w);
		wl_display_flush(w->display);
	}
	pthread_mutex_unlock(&w->lock);
}

static void
cwlw_flush(struct comp_target *ct)
{
	(void)ct;
}

static void
cwlw_set_title(struct comp_target *ct, const char *title)
{
	struct comp_window_linux_wl *w = (struct comp_window_linux_wl *)ct;
	if (w->toplevel != NULL && title != NULL) {
		xdg_toplevel_set_title(w->toplevel, title);
	}
}

static void
cwlw_destroy(struct comp_target *ct)
{
	struct comp_window_linux_wl *w = (struct comp_window_linux_wl *)ct;

	if (w->thread_started) {
		w->running = false;
		pthread_join(w->thread, NULL);
		w->thread_started = false;
	}

	// Swapchain + VkSurfaceKHR first: they borrow the wl_surface / wl_display.
	comp_target_swapchain_cleanup(&w->base);

	if (w->cursor_shape_device != NULL) {
		wp_cursor_shape_device_v1_destroy(w->cursor_shape_device);
	}
	if (w->cursor_shape_manager != NULL) {
		wp_cursor_shape_manager_v1_destroy(w->cursor_shape_manager);
	}
	if (w->pointer != NULL) {
		wl_pointer_destroy(w->pointer);
	}
	if (w->keyboard != NULL) {
		wl_keyboard_destroy(w->keyboard);
	}
	if (w->frac != NULL) {
		wp_fractional_scale_v1_destroy(w->frac);
	}
	if (w->viewport != NULL) {
		wp_viewport_destroy(w->viewport);
	}
	if (w->toplevel != NULL) {
		xdg_toplevel_destroy(w->toplevel);
	}
	if (w->xdg_surface != NULL) {
		xdg_surface_destroy(w->xdg_surface);
	}
	if (w->surface != NULL) {
		wl_surface_destroy(w->surface);
	}
	for (uint32_t i = 0; i < w->output_count; i++) {
		if (w->outputs[i].xdg_output != NULL) {
			zxdg_output_v1_destroy(w->outputs[i].xdg_output);
		}
		if (w->outputs[i].output != NULL) {
			wl_output_destroy(w->outputs[i].output);
		}
	}
	if (w->xdg_output_manager != NULL) {
		zxdg_output_manager_v1_destroy(w->xdg_output_manager);
	}
	if (w->frac_manager != NULL) {
		wp_fractional_scale_manager_v1_destroy(w->frac_manager);
	}
	if (w->viewporter != NULL) {
		wp_viewporter_destroy(w->viewporter);
	}
	if (w->seat != NULL) {
		wl_seat_destroy(w->seat);
	}
	if (w->wm_base != NULL) {
		xdg_wm_base_destroy(w->wm_base);
	}
	if (w->compositor != NULL) {
		wl_compositor_destroy(w->compositor);
	}
	if (w->registry != NULL) {
		wl_registry_destroy(w->registry);
	}
	if (w->display != NULL) {
		wl_display_flush(w->display);
		wl_display_disconnect(w->display);
	}
	pthread_cond_destroy(&w->cond);
	pthread_mutex_destroy(&w->lock);
	free(w);
}

static const struct comp_window_linux_ops wl_ops = {
    .set_visible = cwlw_set_visible,
    .set_cursor_hidden = cwlw_set_cursor_hidden,
};

struct comp_target *
comp_window_linux_wayland_create(struct comp_compositor *c, const struct comp_window_linux_placement *place)
{
	struct comp_window_linux_wl *w = U_TYPED_CALLOC(struct comp_window_linux_wl);
	if (w == NULL) {
		return NULL;
	}
	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);
	w->ops = &wl_ops;
	if (place != NULL) {
		w->place = *place;
	}
	pthread_mutex_init(&w->lock, NULL);
	pthread_cond_init(&w->cond, NULL);

	w->base.base.name = "Linux Wayland";
	w->base.base.destroy = cwlw_destroy;
	w->base.base.flush = cwlw_flush;
	w->base.base.init_pre_vulkan = cwlw_init_pre_vulkan;
	w->base.base.init_post_vulkan = cwlw_init_post_vulkan;
	w->base.base.set_title = cwlw_set_title;
	w->base.base.c = c;
	return &w->base.base;
}
