// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671, #2689): the desktop-Linux
 *         backend dispatcher.
 * @ingroup aux_util
 *
 * Desktop Linux has two window systems, so the shared POSIX tracker's backend
 * (u_window_visibility_posix.h) is one of two sets, chosen per tracker by what
 * the caller was handed: an X11 window (u_window_visibility_x11.c, also under
 * XWayland) or a native Wayland surface (u_window_visibility_wayland.c, read
 * from the GNOME Shell extension's window snapshot). Built without libdbus,
 * the Wayland source has no backend and its tracker is not created.
 */

#include "util/u_window_visibility_posix.h"
#include "xrt/xrt_config_have.h"

#include <stdlib.h>

struct u_wv_backend
{
	enum u_wv_source source;
	struct u_wv_x11 *x11;
	struct u_wv_wl *wl;
};

#ifndef XRT_HAVE_DBUS
// No session-bus client in this build: native Wayland windows stay untracked.
struct u_wv_wl *
u_wv_wl_create(void)
{
	return NULL;
}
void
u_wv_wl_set_target(struct u_wv_wl *b, void *native_window)
{
	(void)b;
	(void)native_window;
}
bool
u_wv_wl_check(struct u_wv_wl *b, const char *panel, const char **out_reason)
{
	(void)b;
	(void)panel;
	*out_reason = "uncovered";
	return false;
}
int
u_wv_wl_fd(struct u_wv_wl *b)
{
	(void)b;
	return -1;
}
void
u_wv_wl_drain(struct u_wv_wl *b)
{
	(void)b;
}
void
u_wv_wl_destroy(struct u_wv_wl *b)
{
	(void)b;
}
#endif

struct u_wv_backend *
u_wv_backend_create(enum u_wv_source source)
{
	struct u_wv_backend *b = (struct u_wv_backend *)calloc(1, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	b->source = source;
	if (source == U_WV_SOURCE_WAYLAND) {
		b->wl = u_wv_wl_create();
	} else {
		b->x11 = u_wv_x11_create();
	}
	if (b->wl == NULL && b->x11 == NULL) {
		free(b);
		return NULL;
	}
	return b;
}

void
u_wv_backend_set_target(struct u_wv_backend *b, void *native_window)
{
	if (b->wl != NULL) {
		u_wv_wl_set_target(b->wl, native_window);
	} else {
		u_wv_x11_set_target(b->x11, native_window);
	}
}

bool
u_wv_backend_check(struct u_wv_backend *b, const char *panel, const char **out_reason)
{
	if (b->wl != NULL) {
		return u_wv_wl_check(b->wl, panel, out_reason);
	}
	return u_wv_x11_check(b->x11, panel, out_reason);
}

int
u_wv_backend_fd(struct u_wv_backend *b)
{
	return b->wl != NULL ? u_wv_wl_fd(b->wl) : u_wv_x11_fd(b->x11);
}

void
u_wv_backend_drain(struct u_wv_backend *b)
{
	if (b->wl != NULL) {
		u_wv_wl_drain(b->wl);
	} else {
		u_wv_x11_drain(b->x11);
	}
}

void
u_wv_backend_destroy(struct u_wv_backend *b)
{
	if (b == NULL) {
		return;
	}
	if (b->wl != NULL) {
		u_wv_wl_destroy(b->wl);
	}
	if (b->x11 != NULL) {
		u_wv_x11_destroy(b->x11);
	}
	free(b);
}
