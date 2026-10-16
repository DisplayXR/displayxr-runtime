// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: dlopen of libei.so.1 and libxkbcommon.so.0.
 */

#include "u_desktop_gnome_libs.h"

#include "util/u_logging.h"

#include <dlfcn.h>
#include <pthread.h>
#include <string.h>

struct xkblib g_xkb;
static pthread_once_t g_xkb_once = PTHREAD_ONCE_INIT;

static void
xkblib_load(void)
{
	void *h = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL);
	if (h == NULL) {
		U_LOG_W("desktop/gnome: libxkbcommon.so.0 not found — chords use US key positions");
		return;
	}
	bool ok = true;
#define XKB_SYM(field, name)                                                                                           \
	do {                                                                                                           \
		void *s = dlsym(h, name);                                                                              \
		if (s == NULL) {                                                                                       \
			ok = false;                                                                                    \
		}                                                                                                      \
		memcpy(&g_xkb.field, &s, sizeof(s));                                                                   \
	} while (0)
	XKB_SYM(context_new, "xkb_context_new");
	XKB_SYM(context_unref, "xkb_context_unref");
	XKB_SYM(keymap_new_from_buffer, "xkb_keymap_new_from_buffer");
	XKB_SYM(keymap_unref, "xkb_keymap_unref");
	XKB_SYM(keymap_min_keycode, "xkb_keymap_min_keycode");
	XKB_SYM(keymap_max_keycode, "xkb_keymap_max_keycode");
	XKB_SYM(keymap_num_layouts_for_key, "xkb_keymap_num_layouts_for_key");
	XKB_SYM(keymap_key_get_syms_by_level, "xkb_keymap_key_get_syms_by_level");
	XKB_SYM(keysym_to_utf32, "xkb_keysym_to_utf32");
#undef XKB_SYM
	g_xkb.ok = ok;
	if (!ok) {
		U_LOG_W("desktop/gnome: libxkbcommon.so.0 lacks a symbol — chords use US key positions");
	}
}

struct eilib g_ei;
static pthread_once_t g_ei_once = PTHREAD_ONCE_INIT;

static void
eilib_load(void)
{
	// Never dlclose'd: the process keeps it for its lifetime.
	void *h = dlopen("libei.so.1", RTLD_NOW | RTLD_LOCAL);
	if (h == NULL) {
		U_LOG_I("desktop/gnome: libei.so.1 not found (%s) — no input (Ubuntu 22.04 has no libei)", dlerror());
		return;
	}
	bool ok = true;
#define EI_SYM(field, name)                                                                                            \
	do {                                                                                                           \
		void *s = dlsym(h, name);                                                                              \
		if (s == NULL) {                                                                                       \
			U_LOG_W("desktop/gnome: libei.so.1 lacks %s — no input", name);                                \
			ok = false;                                                                                    \
		}                                                                                                      \
		memcpy(&g_ei.field, &s, sizeof(s));                                                                    \
	} while (0)
	EI_SYM(new_sender, "ei_new_sender");
	EI_SYM(configure_name, "ei_configure_name");
	EI_SYM(setup_backend_fd, "ei_setup_backend_fd");
	EI_SYM(get_fd, "ei_get_fd");
	EI_SYM(dispatch, "ei_dispatch");
	EI_SYM(get_event, "ei_get_event");
	EI_SYM(event_get_type, "ei_event_get_type");
	EI_SYM(event_unref, "ei_event_unref");
	EI_SYM(event_get_seat, "ei_event_get_seat");
	EI_SYM(event_get_device, "ei_event_get_device");
	EI_SYM(seat_bind_capabilities, "ei_seat_bind_capabilities");
	EI_SYM(device_ref, "ei_device_ref");
	EI_SYM(device_unref, "ei_device_unref");
	EI_SYM(device_has_capability, "ei_device_has_capability");
	EI_SYM(device_start_emulating, "ei_device_start_emulating");
	EI_SYM(device_frame, "ei_device_frame");
	EI_SYM(now, "ei_now");
	EI_SYM(motion_absolute, "ei_device_pointer_motion_absolute");
	EI_SYM(button_button, "ei_device_button_button");
	EI_SYM(scroll_discrete, "ei_device_scroll_discrete");
	EI_SYM(keyboard_key, "ei_device_keyboard_key");
	EI_SYM(device_get_region, "ei_device_get_region");
	EI_SYM(region_get_x, "ei_region_get_x");
	EI_SYM(region_get_y, "ei_region_get_y");
	EI_SYM(region_get_width, "ei_region_get_width");
	EI_SYM(region_get_height, "ei_region_get_height");
	EI_SYM(device_get_name, "ei_device_get_name");
	EI_SYM(unref, "ei_unref");
	EI_SYM(device_keyboard_get_keymap, "ei_device_keyboard_get_keymap");
	EI_SYM(keymap_get_fd, "ei_keymap_get_fd");
	EI_SYM(keymap_get_size, "ei_keymap_get_size");
	EI_SYM(keymap_get_type, "ei_keymap_get_type");
#undef EI_SYM
	{
		void *s = dlsym(h, "ei_event_keyboard_get_xkb_group");
		memcpy(&g_ei.event_keyboard_get_xkb_group, &s, sizeof(s));
	}
	g_ei.h = h;
	g_ei.ok = ok;
}

bool
gnome_libs_ei(void)
{
	pthread_once(&g_ei_once, eilib_load);
	return g_ei.ok;
}

bool
gnome_libs_xkb(void)
{
	pthread_once(&g_xkb_once, xkblib_load);
	return g_xkb.ok;
}
