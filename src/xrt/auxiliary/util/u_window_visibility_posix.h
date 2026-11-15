// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671): the backend interface
 *         behind the shared POSIX tracker (u_window_visibility_posix.c).
 * @ingroup aux_util
 *
 * Private to aux_util. One backend per platform: X11 on desktop Linux
 * (u_window_visibility_x11.c), AppKit on macOS (u_window_visibility_macos.m).
 * The shared tracker owns the thread, the debounce, the logging and the
 * public API; a backend only answers "is this window hidden right now" and,
 * optionally, offers a file descriptor that becomes readable when something
 * worth re-checking happened.
 *
 * Every backend call except create/destroy runs on the tracker's thread.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_wv_backend;

/*!
 * Open the backend (calling thread). NULL when the platform cannot track
 * windows here at all (no X display, no X libraries): the tracker is then not
 * created and the window counts as always visible.
 */
struct u_wv_backend *
u_wv_backend_create(void);

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

#ifdef __cplusplus
}
#endif
