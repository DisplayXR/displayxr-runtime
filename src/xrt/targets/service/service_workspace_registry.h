// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Workspace controller registry enumerator
 *         (HKLM\Software\DisplayXR\WorkspaceControllers).
 * @ingroup ipc
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Don't pull in <windows.h> here — service_orchestrator.c includes
// <winsock2.h> before <windows.h> deliberately and a pre-included
// windows.h breaks that order. We only need MAX_PATH for the struct
// fields, and MAX_PATH on Win32 is 260 (matching this fallback).
#ifndef MAX_PATH
#define MAX_PATH 260
#endif

/*!
 * Single tray-menu action published by a workspace controller.
 * Populated from
 * `HKLM\Software\DisplayXR\WorkspaceControllers\<id>\Actions\<ordering>\`.
 * See `docs/specs/runtime/workspace-controller-registration.md` for the schema.
 */
struct workspace_controller_action
{
	//! Display label for the menu item (UTF-8). Empty entries are
	//! skipped during registry enumeration.
	char label[128];

	//! Action type. Recognized values:
	//!   "lifecycle:enable" / "lifecycle:auto" / "lifecycle:disable"
	//!     — apply the corresponding orchestrator mode.
	//!   "separator"
	//!     — render an `MF_SEPARATOR` and skip the click handler.
	//!   "controller:<action-name>"
	//!     — fire-and-forget `CreateProcess` of the controller binary
	//!       with `--workspace-action <action-name>` args. Controller
	//!       handles singleton-aware forwarding internally.
	//! Unknown types are skipped with a debug log (forward-compat).
	char type[32];
};

#define WORKSPACE_REGISTRY_MAX_ACTIONS 16

/*!
 * Registered workspace controller. Populated from a subkey of
 * `HKLM\Software\DisplayXR\WorkspaceControllers\<id>`. Workspace apps
 * (the DisplayXR shell, third-party verticals, etc.) write these keys
 * from their own installer; the runtime never writes them.
 */
struct workspace_controller_entry
{
	char id[64];                       //!< Subkey name (`shell`, `cockpit-vertical`, …).
	char binary[MAX_PATH];             //!< Absolute path to the controller exe.
	char display_name[256];            //!< Tray label / log strings.
	char vendor[64];                   //!< Optional publisher.
	char version[32];                  //!< Optional free-form version.
	char uninstall_string[MAX_PATH];   //!< Used by runtime uninstaller for cascade.

	//! Optional published tray menu actions. `n_actions == 0` means
	//! the tray falls back to its hardcoded Enable/Auto/Disable
	//! defaults (back-compat for controllers that haven't adopted the
	//! Actions contract yet).
	int n_actions;
	struct workspace_controller_action actions[WORKSPACE_REGISTRY_MAX_ACTIONS];
};

#define WORKSPACE_REGISTRY_MAX_ENTRIES 16

/*!
 * Enumerate all registered workspace controllers.
 *
 * Reads `HKLM\Software\DisplayXR\WorkspaceControllers\*`, skipping
 * entries whose `Binary` value is missing or whose binary file does
 * not exist on disk (stale entries from a removed install).
 *
 * @param out         Caller-supplied array; populated on success.
 * @param max_entries Capacity of @p out. Excess entries are truncated.
 *
 * @return Number of valid entries written to @p out (0 on Windows if
 *         the parent key is absent or empty; 0 on non-Windows always).
 */
int
service_workspace_registry_enumerate(struct workspace_controller_entry *out,
                                     int max_entries);

/*!
 * Look up a single controller by id.
 *
 * @param id  Subkey name to match (e.g. "shell"). Case-insensitive on
 *            Windows.
 * @param out Populated on success.
 *
 * @return true if the controller is registered AND its binary exists
 *         on disk. false otherwise (caller falls back).
 */
bool
service_workspace_registry_lookup(const char *id,
                                  struct workspace_controller_entry *out);

#ifdef __cplusplus
}
#endif
