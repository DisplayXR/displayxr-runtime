// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The minimized-window cache of a backend whose OS window list drops a
 *         window the moment it is minimized (macOS CGWindowList on-screen list).
 *
 * The list itself is rebuilt by a slow background snapshot (macOS: an AX sweep of
 * every app). Between snapshots our own window ops PIN their outcome — a MINIMIZE
 * inserts the window at once, a RESTORE / CLOSE / ACTIVATE / MAXIMIZE removes it —
 * so the common layer's "target exists" check (spec §5 step 3) never sees a window
 * we just minimized as lost. A published snapshot that agrees with a pin retires
 * it. One that disagrees re-applies it while the pin is younger than
 * U_DESKTOP_MINI_CACHE_PIN_TTL_NS at the snapshot's start (the sweep began before
 * the op, or skipped a slow app and kept its stale table); after that the snapshot
 * wins (the user changed the window themselves).
 *
 * Platform-neutral, no locking (the backend holds its own lock around every call).
 */

#pragma once

#include "u_desktop.h"

#ifdef __cplusplus
extern "C" {
#endif

#define U_DESKTOP_MINI_CACHE_MAX 64
#define U_DESKTOP_MINI_CACHE_PINS 16
#define U_DESKTOP_MINI_CACHE_PIN_TTL_NS (5000ull * 1000 * 1000)

struct u_desktop_mini_pin
{
	uint64_t id;
	bool present; //!< true: minimized by us (w valid); false: un-minimized / closed by us
	uint64_t at_ns;
	struct u_desktop_window w;
};

struct u_desktop_mini_cache
{
	struct u_desktop_window w[U_DESKTOP_MINI_CACHE_MAX];
	uint32_t n;
	uint64_t published_ns; //!< start time of the last published snapshot (0 = never / invalidated)
	struct u_desktop_mini_pin pin[U_DESKTOP_MINI_CACHE_PINS];
	uint32_t pin_n;
};

//! A window op of ours just minimized @p w (present) or took window @p id out of the
//! minimized state (closed, restored, activated). Applied at once and kept as a pin.
void
u_desktop_mini_cache_pin(struct u_desktop_mini_cache *c,
                         uint64_t id,
                         bool present,
                         const struct u_desktop_window *w,
                         uint64_t now_ns);

//! Replace the list with a snapshot whose sweep STARTED at @p started_ns. A pin the
//! snapshot agrees with is retired; one it contradicts is re-applied while young.
void
u_desktop_mini_cache_publish(struct u_desktop_mini_cache *c,
                             const struct u_desktop_window *snap,
                             uint32_t n,
                             uint64_t started_ns);

//! Copy up to @p cap entries; returns the number copied.
uint32_t
u_desktop_mini_cache_get(const struct u_desktop_mini_cache *c, struct u_desktop_window *out, uint32_t cap);

#ifdef __cplusplus
}
#endif
