// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service orchestrator — manages the workspace controller child process.
 * @ingroup ipc
 */

#pragma once

#include "service_config.h"

#include <stdbool.h>
#include <stdint.h>

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
 * the workspace mode via the context menu. Starts, stops, or
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
 * Whether a workspace controller binary is currently registered. The tray
 * code calls service_orchestrator_refresh_workspace_controller() before
 * reading this so installing/uninstalling a controller while the service
 * is running takes effect on the next menu open — no service restart
 * required.
 *
 * When false, the tray hides the Workspace submenu and the Ctrl+Space
 * hotkey is a no-op — the runtime is operating as a standalone OpenXR +
 * WebXR platform with no spatial-desktop features.
 */
bool
service_orchestrator_is_workspace_available(void);

/*!
 * Re-enumerate HKLM\Software\DisplayXR\WorkspaceControllers\* and update
 * the cached workspace-controller state. Cheap (one registry walk).
 * Intended to be called by the tray before each menu open so a shell
 * installed after service startup shows up without a service restart.
 */
void
service_orchestrator_refresh_workspace_controller(void);

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

/*!
 * Whether the active workspace controller advertises Tier 1 file-dialog
 * support (registry value `SupportsFileDialog = REG_DWORD 1` under its
 * `HKLM\Software\DisplayXR\WorkspaceControllers\<id>` key). Used by the
 * IPC server to gate `session_request_file_picker` dispatch so apps
 * fall back to a flat OS dialog when the controller has no picker.
 *
 * Returns false when no controller is registered (POSIX: the manifest's
 * `supports_file_dialog`).
 */
bool
service_orchestrator_get_workspace_supports_file_dialog(void);

/*!
 * Summon (spawn-if-absent) the registered workspace controller on demand.
 * Idempotent — a no-op when the controller is already running. Registered as
 * the IPC server's workspace-summon provider so macOS UI surfaces (the menu-bar
 * status item / Ctrl+Space hotkey, #61) launch the controller through the same
 * registry-discovery + crash-respawn path as startup.
 *
 * Defined in the macOS / desktop-Linux orchestrator branch. The Windows
 * orchestrator summons via its own Ctrl+Space trampoline and never references
 * this symbol; Android has no orchestrator (stubs only).
 */
void
service_orchestrator_summon_workspace(void);

/*!
 * Outcome of service_orchestrator_request_launch. Numerically identical to
 * `enum ipc_workspace_launch_status` (the `system_workspace_launch` wire
 * value; main.c maps it explicitly) — kept separate so this header stays free
 * of the IPC headers.
 */
enum service_launch_result
{
	SERVICE_LAUNCH_STARTED = 0,
	SERVICE_LAUNCH_NOT_ACTIVE = 1,
	SERVICE_LAUNCH_DISABLED = 2,
	SERVICE_LAUNCH_ALREADY_RUNNING = 3,
	SERVICE_LAUNCH_NO_CONTROLLER = 4,
	SERVICE_LAUNCH_UNSUPPORTED = 5,
};

/*!
 * Display dashboard phase 8 (`system_reload_service_config`): re-read
 * service.json and apply it through service_orchestrator_apply_config — new
 * launch hotkey (hook re-installed), per-controller mode — without restarting
 * a running controller. Windows marshals it onto the tray thread (the one
 * that owns the hook and the tray's config copy) and returns once posted.
 * Callable from any thread. Returns false when it could not be scheduled.
 */
bool
service_orchestrator_request_config_reload(void);

/*!
 * Display dashboard phase 8 (`system_workspace_launch`): spawn controller
 * @p controller_id now, through the same path as the launch hotkey. Refuses
 * (without side effects) when @p controller_id is not the active controller,
 * its mode is disabled, or it is already running. Callable from any thread.
 * Returns an `enum service_launch_result`.
 */
uint32_t
service_orchestrator_request_launch(const char *controller_id);

/*!
 * Canonical launch combo of the active controller ("Ctrl+Space" by default),
 * or "" when it has none (`--no-hotkey`) or no controller is registered. For
 * tray UI text. The pointer stays valid until the next config apply.
 */
const char *
service_orchestrator_get_launch_hotkey(void);

/*!
 * Display dashboard phase 8 (`system_workspace_hotkey_suspend`): @p suspend
 * true takes the launch-hotkey hook out of the input pipeline so a
 * hotkey-capture box sees the current combo — config untouched, a running
 * controller untouched; it comes back on suspend=false or after a 60 s safety
 * timeout (re-armed by every suspend=true). One WARN per suspend / resume (with
 * the reason). Windows marshals onto the tray thread. Callable from any thread.
 */
bool
service_orchestrator_request_hotkey_suspend(bool suspend);

/*!
 * The active controller's effective lifecycle mode (per-controller entry over
 * the top-level `workspace` spelling — service_config_resolve_launch).
 */
enum service_child_mode
service_orchestrator_get_workspace_mode(void);

/*!
 * Desktop Linux only: SERVICE_HOTKEY_MOD_* bits → the controller-key modifier
 * bits of `ipc_server_input_queue_push_controller_key` (bit0 Shift, bit1 Ctrl,
 * bit2 Alt; no Win bit), used to forward the launch chord to a running
 * controller. Exposed for unit tests.
 */
uint32_t
service_orchestrator_controller_key_mods(uint32_t hotkey_mods);

#ifdef __cplusplus
}
#endif
