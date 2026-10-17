// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: libei and libxkbcommon, both dlopen'd so the
 *         runtime gains no DT_NEEDED (ADR-056; .deb STABLE_SONAMES rule).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libei, dlopen'd. The enum values are libei 1.x's (libei.so.1), verified
 * against libei-1.0/libei.h 1.2.1.
 */

struct ei;
struct ei_event;
struct ei_seat;
struct ei_device;
struct ei_region;

enum
{
	EIX_EVENT_CONNECT = 1,
	EIX_EVENT_DISCONNECT = 2,
	EIX_EVENT_SEAT_ADDED = 3,
	EIX_EVENT_SEAT_REMOVED = 4,
	EIX_EVENT_DEVICE_ADDED = 5,
	EIX_EVENT_DEVICE_REMOVED = 6,
	EIX_EVENT_DEVICE_PAUSED = 7,
	EIX_EVENT_DEVICE_RESUMED = 8,
	EIX_EVENT_KEYBOARD_MODIFIERS = 9,
};

enum
{
	EIX_KEYMAP_TYPE_XKB = 1,
};

struct ei_keymap;

enum
{
	EIX_CAP_POINTER = 1 << 0,
	EIX_CAP_POINTER_ABSOLUTE = 1 << 1,
	EIX_CAP_KEYBOARD = 1 << 2,
	EIX_CAP_TOUCH = 1 << 3,
	EIX_CAP_SCROLL = 1 << 4,
	EIX_CAP_BUTTON = 1 << 5,
};

struct eilib
{
	bool ok;
	void *h;
	struct ei *(*new_sender)(void *user_data);
	void (*configure_name)(struct ei *ei, const char *name);
	int (*setup_backend_fd)(struct ei *ei, int fd);
	int (*get_fd)(struct ei *ei);
	void (*dispatch)(struct ei *ei);
	struct ei_event *(*get_event)(struct ei *ei);
	int (*event_get_type)(struct ei_event *ev);
	struct ei_event *(*event_unref)(struct ei_event *ev);
	struct ei_seat *(*event_get_seat)(struct ei_event *ev);
	struct ei_device *(*event_get_device)(struct ei_event *ev);
	void (*seat_bind_capabilities)(struct ei_seat *seat, ...);
	struct ei_device *(*device_ref)(struct ei_device *d);
	struct ei_device *(*device_unref)(struct ei_device *d);
	bool (*device_has_capability)(struct ei_device *d, int cap);
	void (*device_start_emulating)(struct ei_device *d, uint32_t sequence);
	void (*device_frame)(struct ei_device *d, uint64_t time);
	uint64_t (*now)(struct ei *ei);
	void (*motion_absolute)(struct ei_device *d, double x, double y);
	void (*button_button)(struct ei_device *d, uint32_t button, bool is_press);
	void (*scroll_discrete)(struct ei_device *d, int32_t x, int32_t y);
	void (*keyboard_key)(struct ei_device *d, uint32_t keycode, bool is_press);
	struct ei_region *(*device_get_region)(struct ei_device *d, size_t index);
	uint32_t (*region_get_x)(struct ei_region *r);
	uint32_t (*region_get_y)(struct ei_region *r);
	uint32_t (*region_get_width)(struct ei_region *r);
	uint32_t (*region_get_height)(struct ei_region *r);
	const char *(*device_get_name)(struct ei_device *d);
	struct ei *(*unref)(struct ei *ei);
	struct ei_keymap *(*device_keyboard_get_keymap)(struct ei_device *d);
	int (*keymap_get_fd)(struct ei_keymap *k);
	size_t (*keymap_get_size)(struct ei_keymap *k);
	int (*keymap_get_type)(struct ei_keymap *k);
	//! Optional (NULL: assume layout group 0).
	uint32_t (*event_keyboard_get_xkb_group)(struct ei_event *ev);
};

/*
 * libxkbcommon, dlopen'd too ("libxkbcommon.so.0" — on every supported Ubuntu,
 * but dlopen keeps the .deb's DT_NEEDED list untouched). Only used to find, on
 * the session keymap libei hands out, the key that produces a character.
 */
struct xkb_context;
struct xkb_keymap;

struct xkblib
{
	bool ok;
	struct xkb_context *(*context_new)(int flags);
	void (*context_unref)(struct xkb_context *ctx);
	struct xkb_keymap *(*keymap_new_from_buffer)(
	    struct xkb_context *ctx, const char *buf, size_t len, int format, int flags);
	void (*keymap_unref)(struct xkb_keymap *km);
	uint32_t (*keymap_min_keycode)(struct xkb_keymap *km);
	uint32_t (*keymap_max_keycode)(struct xkb_keymap *km);
	uint32_t (*keymap_num_layouts_for_key)(struct xkb_keymap *km, uint32_t key);
	int (*keymap_key_get_syms_by_level)(
	    struct xkb_keymap *km, uint32_t key, uint32_t layout, uint32_t level, const uint32_t **syms_out);
	uint32_t (*keysym_to_utf32)(uint32_t keysym);
};

#define XKBX_KEYMAP_FORMAT_TEXT_V1 1


//! The process-wide tables, filled by gnome_libs_ei() / gnome_libs_xkb().
extern struct eilib g_ei;
extern struct xkblib g_xkb;

//! Load libei.so.1 once; true when every symbol is there.
bool
gnome_libs_ei(void);

//! Load libxkbcommon.so.0 once; true when every symbol is there.
bool
gnome_libs_xkb(void);

#ifdef __cplusplus
}
#endif
