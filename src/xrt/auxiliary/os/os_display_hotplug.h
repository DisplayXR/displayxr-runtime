// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OS display-reconfiguration notification (display hotplug, ADR-047
 *         Amendment 7).
 * @ingroup aux_os
 *
 * A monitor plugged in or unplugged, a mode or arrangement change: the
 * platform says so, the runtime re-runs its per-monitor claim sweep. This is
 * only the trigger. It carries no payload (the sweep re-enumerates), it may
 * fire on any thread and several times per change, and the subscriber debounces.
 *
 * macOS: `CGDisplayRegisterReconfigurationCallback`. Its callbacks are
 * delivered through the main thread's run loop, so a process without one sees
 * none — which is why the runtime also polls a cheap topology signature.
 * Elsewhere: not implemented (returns false); the poll is the trigger.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Called once per OS display-reconfiguration notification (the "end of
 * reconfiguration" edge where the platform has one). Any thread; must not
 * block.
 */
typedef void (*os_display_hotplug_fn)(void *user);

/*!
 * Subscribe @p fn. One subscriber per process (a second call replaces the
 * first).
 *
 * @return false when the platform has no notification (the caller polls).
 */
bool
os_display_hotplug_subscribe(os_display_hotplug_fn fn, void *user);

/*!
 * Unsubscribe. After it returns @p fn is not called again.
 */
void
os_display_hotplug_unsubscribe(void);

#ifdef __cplusplus
}
#endif
