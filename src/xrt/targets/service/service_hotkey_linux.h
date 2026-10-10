// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Linux workspace-controller launch hotkey: GNOME Shell extension
 *         keybinding (Wayland and X11 GNOME), X11 root-window grab fallback.
 *
 * Linux has no WH_KEYBOARD_LL / RegisterEventHotKey. Two backends, picked at
 * arm time (and re-picked when the extension comes or goes):
 *
 *   1. **GNOME Shell extension** (`window-geometry@displayxr.org` version 13+,
 *      `org.displayxr.WorkspaceHotkey1` on the `org.displayxr.WindowGeometry`
 *      bus name). The service pushes the accelerator it resolved from
 *      service.json (`Configure`); the extension grabs it with
 *      `Meta.Display.grab_accelerator`, caches it, and on every press emits
 *      `Activated`. Because the extension outlives the socket-activated,
 *      idle-exiting service, it also starts the service's systemd user unit on
 *      a press that finds no service registered, and `Configure`'s reply then
 *      carries that press (`pending`). Works under Wayland and X11 GNOME.
 *   2. **X11** (`DISPLAY` set, no `WAYLAND_DISPLAY`, no extension): an
 *      XGrabKey-equivalent `xcb_grab_key` on the root window, for the lifetime
 *      of the service process only.
 *
 * Neither available → one WARN; the controller is still launchable from the
 * dashboard / `displayxr-cli workspace launch <id>`.
 *
 * All the work runs on one worker thread; the functions below only post
 * requests to it, so they are callable from any thread.
 *
 * See docs/specs/runtime/workspace-controller-registration.md § Linux.
 *
 * @ingroup ipc
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Called on the worker thread for every hotkey press (and for a press the
//! extension carried across a service start).
typedef void (*service_hotkey_linux_activate_fn)(void);

/*!
 * Start the worker. @p on_activate must stay valid until
 * service_hotkey_linux_stop(). No grab is made until the first arm.
 */
void
service_hotkey_linux_start(service_hotkey_linux_activate_fn on_activate);

/*!
 * Arm @p combo (canonical service_hotkey grammar, "Ctrl+Space"), or disarm
 * with NULL / "" (no controller, `--no-hotkey`, mode disabled). Idempotent.
 */
void
service_hotkey_linux_arm(const char *combo);

/*!
 * Display dashboard phase 8: take the grab out (true) so a hotkey-capture box
 * sees the combo, or put it back (false). A suspend ends by itself after 60 s.
 */
void
service_hotkey_linux_suspend(bool suspend);

/*!
 * Release every grab, tell the extension nothing (its cached accelerator is
 * what lets the next press start the service again), join the worker.
 */
void
service_hotkey_linux_stop(void);

/*!
 * The systemd user unit this process runs as ("displayxr.service"), parsed
 * from /proc/self/cgroup, or "" when it does not run under a user manager.
 * Exposed for unit tests: service_hotkey_linux_unit_from_cgroup().
 */
bool
service_hotkey_linux_unit_from_cgroup(const char *cgroup_text, char *out, unsigned out_size);

#ifdef __cplusplus
}
#endif
