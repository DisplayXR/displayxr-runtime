// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671): the backend interface
 *         behind the shared POSIX tracker (u_window_visibility_posix.c).
 * @ingroup aux_util
 *
 * Private to aux_util. One backend per window system: X11 on desktop Linux
 * (u_window_visibility_x11.c), native Wayland on desktop Linux through the
 * GNOME Shell extension's window snapshot (u_window_visibility_wayland.c,
 * #2689), AppKit on macOS (u_window_visibility_macos.m). Desktop Linux has
 * two, so there the u_wv_backend_* entry points are a small dispatcher
 * (u_window_visibility_linux.c) over the u_wv_x11_* / u_wv_wl_* sets below.
 * The shared tracker owns the thread, the debounce, the logging and the
 * public API; a backend only answers "is this window hidden right now" and,
 * optionally, offers a file descriptor that becomes readable when something
 * worth re-checking happened.
 *
 * Every backend call except create/destroy runs on the tracker's thread.
 */

#pragma once

#include "xrt/xrt_config_os.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_wv_backend;

//! What kind of window handle the tracker is given (#2689).
enum u_wv_source
{
	//! The platform's own handle: an X11 XID on desktop Linux, an
	//! NSView / NSWindow on macOS.
	U_WV_SOURCE_NATIVE = 0,
	//! A native Wayland `wl_surface *` (desktop Linux only), tracked through
	//! the GNOME Shell extension's per-window "hidden" (extension v20+).
	U_WV_SOURCE_WAYLAND,
};

/*!
 * Open the backend for @p source (calling thread). NULL when the platform
 * cannot track windows here at all (no X display, no X libraries, no session
 * bus, not GNOME, a source the platform does not have): the tracker is then
 * not created and the window counts as always visible.
 */
struct u_wv_backend *
u_wv_backend_create(enum u_wv_source source);

/*!
 * Track @p native_window (see u_window_visibility_create for its meaning per
 * platform), or none (NULL). Tracker thread.
 */
void
u_wv_backend_set_target(struct u_wv_backend *b, void *native_window);

/*!
 * One raw check of the current target: true when it is hidden now. @p panel
 * names the 3D screen (may be ""). Sets @p out_reason to a static string.
 * Tracker thread.
 */
bool
u_wv_backend_check(struct u_wv_backend *b, const char *panel, const char **out_reason);

/*!
 * A file descriptor that becomes readable when the backend has native events
 * to drain, or -1 for a polling-only backend. Stable for the backend's life.
 */
int
u_wv_backend_fd(struct u_wv_backend *b);

//! Consume pending native events (after @ref u_wv_backend_fd polled readable). Tracker thread.
void
u_wv_backend_drain(struct u_wv_backend *b);

//! Close the backend (any thread, after the tracker thread has been joined).
void
u_wv_backend_destroy(struct u_wv_backend *b);

#ifdef XRT_OS_LINUX_DESKTOP
/*
 * Desktop Linux: the two backend sets behind the dispatcher. Same contract
 * as the u_wv_backend_* function of the same suffix.
 */
struct u_wv_x11;
struct u_wv_x11 *
u_wv_x11_create(void);
void
u_wv_x11_set_target(struct u_wv_x11 *b, void *native_window);
bool
u_wv_x11_check(struct u_wv_x11 *b, const char *panel, const char **out_reason);
int
u_wv_x11_fd(struct u_wv_x11 *b);
void
u_wv_x11_drain(struct u_wv_x11 *b);
void
u_wv_x11_destroy(struct u_wv_x11 *b);

//! NULL when there is no session bus or the desktop is not GNOME (built
//! without libdbus: always NULL).
struct u_wv_wl;
struct u_wv_wl *
u_wv_wl_create(void);
void
u_wv_wl_set_target(struct u_wv_wl *b, void *native_window);
bool
u_wv_wl_check(struct u_wv_wl *b, const char *panel, const char **out_reason);
int
u_wv_wl_fd(struct u_wv_wl *b);
void
u_wv_wl_drain(struct u_wv_wl *b);
void
u_wv_wl_destroy(struct u_wv_wl *b);
#endif

#ifdef __cplusplus
}
#endif
