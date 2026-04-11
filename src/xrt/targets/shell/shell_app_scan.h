// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR shell app-discovery scanner.
 *
 * Walks a fixed set of filesystem locations looking for executables that ship a
 * @c .displayxr.json sidecar. Parses the sidecar, resolves icon paths, and
 * returns an array of scanned apps for the launcher.
 *
 * See docs/specs/displayxr-app-manifest.md for the sidecar contract.
 *
 * @ingroup shell
 */

#pragma once

#ifdef _WIN32
#include <windows.h>
#define SHELL_PATH_MAX MAX_PATH
#else
#define SHELL_PATH_MAX 4096
#endif

#define SHELL_APP_NAME_MAX 128
#define SHELL_APP_TYPE_MAX 8
#define SHELL_APP_CATEGORY_MAX 32
#define SHELL_APP_DESCRIPTION_MAX 256
#define SHELL_APP_DISPLAY_MODE_MAX 16
#define SHELL_APP_ICON_LAYOUT_MAX 8

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * A single app discovered by the scanner. Populated from the sidecar manifest
 * plus filesystem resolution. All string fields are NUL-terminated.
 */
struct shell_scanned_app
{
	char name[SHELL_APP_NAME_MAX];                    // sidecar "name"
	char exe_path[SHELL_PATH_MAX];                    // absolute path to the .exe
	char type[SHELL_APP_TYPE_MAX];                    // "3d" or "2d"
	char category[SHELL_APP_CATEGORY_MAX];            // sidecar "category", default "app"
	char description[SHELL_APP_DESCRIPTION_MAX];      // sidecar "description"
	char display_mode[SHELL_APP_DISPLAY_MODE_MAX];    // sidecar "display_mode", default "auto"
	char icon_path[SHELL_PATH_MAX];                   // absolute path or "" if none
	char icon_3d_path[SHELL_PATH_MAX];                // absolute path or "" if none
	char icon_3d_layout[SHELL_APP_ICON_LAYOUT_MAX];   // "sbs-lr"|"sbs-rl"|"tb"|"bt", empty if no icon_3d
};

/*!
 * Scan the standard DisplayXR app-discovery paths and populate @p out.
 *
 * @p shell_exe_dir is the directory containing the shell binary; relative scan
 * paths are resolved against it (e.g. @c <shell_exe_dir>/../test_apps/).
 *
 * Returns the number of apps written to @p out (at most @p max_out). Rejected
 * manifests (missing required fields, schema_version mismatch, unreadable icon
 * files) are logged and skipped.
 */
int
shell_scan_apps(const char *shell_exe_dir, struct shell_scanned_app *out, int max_out);

#ifdef __cplusplus
}
#endif
