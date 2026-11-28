// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The process-wide lock around the runtime's own Xlib calls (desktop
 *         Linux).
 * @ingroup aux_os
 *
 * ## Why a lock, and not XInitThreads
 *
 * Xlib is thread-safe only after `XInitThreads()`, and only a process's FIRST
 * Xlib call may make it. The runtime runs inside host processes (an OpenXR app,
 * the service, the CLI) that it does not control and that almost never call it,
 * so the runtime cannot rely on it and must not call it late. Without it,
 * Xlib's process-global state is unlocked:
 *
 * - `XOpenDisplay` / `XCloseDisplay`: the display list, the resource-manager
 *   quark table, the error-handler chain, per-extension bookkeeping;
 * - the first extension request on a connection (`XRR*`, through libXext's
 *   per-extension display list), and its teardown in `XCloseDisplay`.
 *
 * Two runtime threads doing any of those at once corrupt the heap: the display
 * hotplug sweep and a session's registry build did exactly that (#2820,
 * `os_display_desktop_x11.c`). Every runtime site that touches that state takes
 * this one lock.
 *
 * ## What this lock does NOT cover
 *
 * - **The app's own Xlib use.** The host's threads never take this lock. A
 *   private connection keeps the runtime off the app's Display, and an app
 *   that hands the runtime its Display (XR_DXR_xlib_window_binding) and touches
 *   it from several threads must have called XInitThreads (the binding's
 *   contract).
 * - **Ordinary core requests on a private connection used by one thread at a
 *   time** (e.g. `XQueryPointer`). They touch only that connection's state, so
 *   they need the connection's owner lock, not this one.
 * - **Another DSO's copy of aux_os.** aux_os is a static library: a plug-in
 *   that links it (sim-display) carries its own copy of this lock.
 *
 * ## Rules for a critical section
 *
 * Short; no blocking wait on anything but the X server's reply; never across a
 * Vulkan call (a WSI or display-acquire call may use the same libX11 from the
 * driver); never calls back into another locked aux_os entry point (the lock is
 * not recursive: @ref os_display_desktop_enumerate and friends take it
 * themselves).
 */

#pragma once

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_LINUX_DESKTOP

#ifdef __cplusplus
extern "C" {
#endif

//! Take the process-wide Xlib lock (see the file comment). Not recursive.
void
os_xlib_lock(void);

//! Release the lock taken by @ref os_xlib_lock.
void
os_xlib_unlock(void);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_LINUX_DESKTOP
