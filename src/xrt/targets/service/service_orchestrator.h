// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service orchestrator — manages workspace controller and bridge child processes.
 * @ingroup ipc
 */

#pragma once

#include "service_config.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration; full definition in service_workspace_registry.h.
struct workspace_controller_entry;

/*!
 * Initialize the orchestrator. Registers hotkeys and/or spawns children
 * based on the config. Must be called after service_tray_init().
 *
 * @param cfg  Current configuration.
 * @return true on success.
 */
bool
service_orchestrator_init(const struct service_config *cfg);

/*!
 * Called by the tray config-change callback when the user changes
 * workspace/bridge mode via the context menu. Starts, stops, or
 * re-registers hotkeys as needed.
 */
void
service_orchestrator_apply_config(const struct service_config *cfg);

/*!
 * Shut down the orchestrator. Terminates managed children, unregisters
 * hotkeys. Must be called before service_tray_cleanup().
 */
void
service_orchestrator_shutdown(void);

/*!
 * Whether a workspace controller binary was found next to the service exe at
 * init time. When false, the tray hides the Workspace submenu and the
 * Ctrl+Space hotkey is a no-op — the runtime is operating as a standalone
 * OpenXR + WebXR platform with no spatial-desktop features.
 *
 * Detection is one-time at init. Reinstalling a controller while the service
 * is running requires a service restart.
 */
bool
service_orchestrator_is_workspace_available(void);

/*!
 * Display name for the active workspace controller, suitable for tray UI.
 * Sourced from the controller's `DisplayName` registry value at
 * `HKLM\Software\DisplayXR\WorkspaceControllers\<id>`. Returns an empty
 * string if no controller is registered.
 *
 * The returned pointer is owned by the orchestrator and remains valid for
 * the service's lifetime.
 */
const char *
service_orchestrator_get_workspace_display_name(void);

/*!
 * Returns a const pointer to the active workspace controller entry, so
 * callers (the tray) can read DisplayName, the published Actions list,
 * etc. without re-enumerating the registry. Returns NULL when no
 * controller is available.
 *
 * The returned pointer is owned by the orchestrator and remains valid
 * for the service's lifetime.
 */
const struct workspace_controller_entry *
service_orchestrator_get_workspace_entry(void);

/*!
 * Fire-and-forget invocation of the registered workspace controller
 * with `--workspace-action <action_name>` args. The controller is
 * responsible for singleton-aware forwarding (if an instance is
 * already running, the new process hands the action off to it and
 * exits). No-op if no controller is registered.
 *
 * See `docs/specs/runtime/workspace-controller-registration.md` for the
 * `--workspace-action` command-line contract.
 */
void
service_orchestrator_dispatch_controller_action(const char *action_name);

/*!
 * PID of the workspace controller process spawned by this orchestrator, or 0
 * if no orchestrator-spawned workspace is running.
 *
 * Used by the IPC layer to authenticate `workspace_activate` requests: only
 * the process the orchestrator launched may transition the runtime into
 * workspace mode. A return value of 0 means manual mode — first-claim wins.
 *
 * Return type is `unsigned long` (not `DWORD`) so this header stays free of
 * `<windows.h>`; callers cast as needed.
 */
unsigned long
service_orchestrator_get_workspace_pid(void);

#ifdef __cplusplus
}
#endif
