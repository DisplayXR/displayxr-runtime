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
 * Missing file or missing keys → defaults (auto/auto/true,
 * workspace_binary = "displayxr-shell.exe"). The `workspace`
 * JSON key is read with backwards-compat fallback to the legacy
 * `shell` key from earlier installs.
 */
struct service_config
{
	enum service_child_mode workspace; //!< Workspace controller lifecycle mode (default AUTO).
	enum service_child_mode bridge;    //!< WebXR bridge lifecycle mode (default AUTO).
	bool start_on_login;               //!< If false, service exits immediately on auto-start.

	//! Filename or path of the workspace controller binary that the orchestrator
	//! spawns (resolved as a sibling of the service exe if it has no path
	//! separator). Default "displayxr-shell.exe" — DisplayXR Shell is the
	//! reference workspace controller. OEMs / third parties drop a different
	//! name here to ship their own controller.
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
