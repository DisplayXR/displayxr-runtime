// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Workspace controller sidecar manifest (`<binary>.controller.json`).
 * @ingroup ipc
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Metadata describing an installed workspace controller, read from a JSON
 * sidecar dropped next to the controller binary at install time.
 *
 * The manifest is optional — when absent, callers fall back to defaults
 * (e.g. display_name = "Workspace Controller"). Schema is forward-additive:
 * unknown fields are ignored, missing optional fields land as empty strings.
 */
struct workspace_manifest
{
	char display_name[256]; //!< Human-readable name for tray UI etc.
	char vendor[64];        //!< Vendor / publisher.
	char version[32];       //!< Free-form version string.
	char icon_path[260];    //!< Optional path to an icon resource.
};

/*!
 * Load `<workspace_binary_path with .exe stripped>.controller.json` next to
 * the workspace binary, parse it, and populate @p out.
 *
 * @param workspace_binary_path Absolute path to the workspace controller
 *                              binary. The sidecar manifest is found by
 *                              stripping `.exe` (case-insensitive) from
 *                              this path and appending `.controller.json`.
 * @param out                   Populated on success; zeroed on failure.
 *
 * @return true on success (manifest found, parsed, schema_version == 1).
 *         false if the manifest is absent, malformed, or has an unsupported
 *         schema version. The caller should fall back to defaults.
 */
bool
service_workspace_manifest_load(const char *workspace_binary_path,
                                struct workspace_manifest *out);

#ifdef __cplusplus
}
#endif
