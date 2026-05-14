// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service orchestrator configuration (persisted to service.json).
 * @ingroup ipc
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Lifecycle mode for a managed child process (workspace, bridge).
 */
enum service_child_mode
{
	SERVICE_CHILD_ENABLE,  //!< Always running — launched at service startup.
	SERVICE_CHILD_DISABLE, //!< Never running — not launched, hotkeys not registered.
	SERVICE_CHILD_AUTO,    //!< On-demand — launched when triggered (hotkey, client detection).
};

//! Maximum length of the workspace controller binary path.
#define SERVICE_WORKSPACE_BINARY_MAX 260

/*!
 * Persisted service configuration.
 *
 * Stored in `%LOCALAPPDATA%\DisplayXR\service.json` on Windows,
 * `~/.config/displayxr/service.json` on Linux/macOS.
 *
 * Missing file or missing keys → defaults (auto/auto/true).
 */
struct service_config
{
	enum service_child_mode workspace; //!< Workspace controller lifecycle mode (default AUTO).
	enum service_child_mode bridge;    //!< WebXR bridge lifecycle mode (default AUTO).
	bool start_on_login;               //!< If false, service exits immediately on auto-start.

	//! Workspace controller selection.
	//!
	//! Empty (default) → orchestrator picks the first entry from
	//!   `HKLM\Software\DisplayXR\WorkspaceControllers\*`.
	//! Bare id (no path separator) → preferred registered controller, e.g.
	//!   `"shell"` matches `WorkspaceControllers\shell`. Falls back to first
	//!   entry if not registered.
	//! Absolute path (contains `\` or `/`) → dev-mode override; the
	//!   orchestrator launches that exact binary without consulting the
	//!   registry. Use for testing freshly-built binaries from `_package/`.
	//!
	//! The runtime owns no specific workspace app — workspace controllers
	//! register themselves from their own installer. See
	//! `docs/specs/runtime/workspace-controller-registration.md`.
	char workspace_binary[SERVICE_WORKSPACE_BINARY_MAX];
};

/*!
 * Load configuration from disk. Returns defaults if the file is absent or
 * contains errors — never fails.
 */
void
service_config_load(struct service_config *cfg);

/*!
 * Save configuration to disk.
 * @return true on success.
 */
bool
service_config_save(const struct service_config *cfg);

#ifdef __cplusplus
}
#endif
