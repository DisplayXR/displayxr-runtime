// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Persisted runtime settings — the store the Control Panel writes and
 *         the runtime reads *inside the app's own process*.
 * @ingroup aux_util
 *
 * ## Why this exists
 *
 * Every runtime tuning lever is an environment variable, and the Control Panel
 * cannot set one for an app it does not launch — which is every app. The panel
 * is a separate, non-elevated process; the runtime DLL lives in the app's. So
 * the only channel that reaches an arbitrary app is a store the runtime reads
 * from inside that process. This is that store.
 *
 * ## Resolution order
 *
 * ```
 *   1. environment      getenv_s / getenv / debug.xrt.<NAME>   <-- ALWAYS WINS
 *   2. per-user file    %LOCALAPPDATA%\DisplayXR\settings.json
 *                       $XDG_CONFIG_HOME/displayxr/settings.json
 *   3. machine default  HKLM\Software\DisplayXR\Settings       (Windows only)
 *   4. nothing          NULL -> the caller keeps its own compiled default
 * ```
 *
 * **The environment outranking the stores is deliberate and load-bearing.**
 * `scripts/perf-ladder/` sets these per A/B arm in the launcher environment; if
 * a stale panel setting could outrank that, every measurement would silently
 * lie. It also means a launcher, a `.bat` or a harness can always take control
 * back from a setting somebody left behind.
 *
 * ## Only allow-listed names are resolvable from the stores
 *
 * @ref u_setting_is_managed gates steps 2 and 3. A name that is not on the list
 * resolves from the environment alone, exactly as it did before this file
 * existed. That is a safety property, not tidiness: several `DXR_*` variables
 * gate code loading or authorization (`DXR_ALLOW_UNVERIFIED_CONTROLLER`, and
 * `DXR_ALLOW_DEV_PLUGIN_PATHS`, which disables the #943 plug-in-path guard and
 * deliberately does not even read through this path). A per-user file that
 * could flip one of those would be a privilege-escalation vector. Adding a name
 * to the list is a deliberate act — see the list in `u_setting.c`.
 *
 * Design + the census behind it:
 * `docs/roadmap/control-panel-performance-settings.md`.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Where a resolved value came from. Reported so a UI can say *"this is the
 * default"* rather than presenting a configured value as a machine fact.
 */
enum u_setting_source
{
	U_SETTING_SOURCE_DEFAULT = 0, //!< Nothing set it; the caller's own default stands.
	U_SETTING_SOURCE_ENV,         //!< Environment variable / Android system property.
	U_SETTING_SOURCE_USER,        //!< Per-user settings file (what the Control Panel writes).
	U_SETTING_SOURCE_MACHINE,     //!< Machine-wide default (HKLM; needs admin to write).
};

//! Short lowercase label for @p source ("default", "env", "user", "machine").
const char *
u_setting_source_str(enum u_setting_source source);

/*!
 * Resolve one option through the chain above.
 *
 * @param      name       Option name, e.g. `"DXR_WEAVE_REPAINT"`.
 * @param[out] buf        Storage for the value.
 * @param      cap        Size of @p buf.
 * @param[out] out_source Where the value came from. May be NULL.
 *
 * @return @p buf when something set the option, NULL when nothing did (in which
 *         case @p out_source is @ref U_SETTING_SOURCE_DEFAULT and the caller
 *         must keep its own compiled default).
 */
const char *
u_setting_get_raw(const char *name, char *buf, size_t cap, enum u_setting_source *out_source);

/*!
 * May @p name be resolved from the persisted stores? Unmanaged names are
 * environment-only. See the file comment for why this gate exists.
 */
bool
u_setting_is_managed(const char *name);

//! Number of allow-listed names, for tooling that enumerates them.
uint32_t
u_setting_managed_count(void);

//! Allow-listed name @p index, or NULL when out of range.
const char *
u_setting_managed_name(uint32_t index);

/*
 *
 * Writing the per-user store. Used by `displayxr-cli perf`; the Control Panel
 * goes through the CLI rather than writing the file itself, so there is one
 * writer (the same shape as the `PreferredPlugin` override).
 *
 */

/*!
 * Set one allow-listed option in the per-user file, creating it if needed.
 * Refuses names that are not managed. Returns false on refusal or I/O failure.
 */
bool
u_setting_user_set(const char *name, const char *value);

//! Remove one option from the per-user file. Absent is success.
bool
u_setting_user_clear(const char *name);

//! Remove every option from the per-user file.
bool
u_setting_user_clear_all(void);

//! Read back what the per-user file holds for @p name, or NULL.
const char *
u_setting_user_get(const char *name, char *buf, size_t cap);

//! Full path of the per-user file (whether or not it exists yet).
bool
u_setting_user_path(char *buf, size_t cap);

/*!
 * ISO-8601 date the per-user file was last written, or NULL when it has never
 * been written. A UI uses this to say how long a setting has been in force —
 * the cheapest defence against somebody being pinned by a setting they left
 * behind months ago.
 */
const char *
u_setting_user_written(char *buf, size_t cap);

/*!
 * Drop the cached parse so the next @ref u_setting_get_raw re-reads the stores.
 * Only a process that WRITES needs this; readers latch their options anyway.
 */
void
u_setting_reload(void);

#ifdef __cplusplus
}
#endif
