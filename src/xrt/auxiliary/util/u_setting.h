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

/*
 *
 * Per-screen display-processor preference (display dashboard phase 7).
 *
 * Not a scalar option, so not on the allow-list above: it is allow-listed by
 * construction — this accessor reads exactly one JSON object, one environment
 * variable and one registry key, and nothing else can be read through it.
 *
 *   1. environment   DXR_PREFERRED_PLUGIN_PER_SCREEN="<key>=<id>;<key>=<id>"
 *   2. per-user file "preferred_plugin_per_screen": { "<key>": "<id>" }
 *                    (what the dashboard / `displayxr-cli dp use --screen` write)
 *   3. machine       HKLM\Software\DisplayXR\DisplayProcessors\PreferredPlugin,
 *                    value "<key>" (REG_SZ, admin; Windows only)
 *
 * `<key>` is the stable screen key (`target_screen_keys_build`). Every failure
 * is "not set". The stores are read once per process; a long-lived process
 * that must see a newer write (the service, on a display re-probe) calls
 * @ref u_setting_per_screen_reload.
 *
 */

//! Environment variable of the per-screen preference (tier 1).
#define U_SETTING_PER_SCREEN_ENV "DXR_PREFERRED_PLUGIN_PER_SCREEN"
//! Object name in the per-user settings file (tier 2).
#define U_SETTING_PER_SCREEN_JSON_KEY "preferred_plugin_per_screen"
//! Size of a screen key, incl. the NUL (= TARGET_SCREEN_KEY_MAX).
#define U_SETTING_SCREEN_KEY_MAX 64

/*!
 * The plug-in id preferred for the screen @p key, through the chain above.
 *
 * @return @p buf when a tier names a plug-in for @p key, else NULL with
 *         @p out_source (may be NULL) = @ref U_SETTING_SOURCE_DEFAULT.
 */
const char *
u_setting_get_preferred_plugin_for_screen(const char *key, char *buf, size_t cap, enum u_setting_source *out_source);

//! Drop the cached per-screen stores so the next lookup re-reads them.
void
u_setting_per_screen_reload(void);

/*!
 * Set (or, with @p plugin_id NULL/"", remove) the per-user preference for the
 * screen @p key. Refuses an empty key, one too long, or one containing `=` or
 * `;`. Returns false on refusal or I/O failure.
 */
bool
u_setting_user_set_preferred_plugin_for_screen(const char *key, const char *plugin_id);

//! Remove every per-user per-screen preference (the whole JSON object). Absent is success.
bool
u_setting_user_clear_preferred_plugin_per_screen(void);

//! Outcome of a machine-tier write.
enum u_setting_write_result
{
	U_SETTING_WRITE_OK = 0,
	U_SETTING_WRITE_DENIED = 1,      //!< Needs an elevated (admin) process.
	U_SETTING_WRITE_FAILED = 2,      //!< Any other failure (bad key, I/O).
	U_SETTING_WRITE_UNSUPPORTED = 3, //!< No machine tier on this platform.
};

/*!
 * Set (or, with @p plugin_id NULL/"", remove) the machine-wide preference for
 * the screen @p key (Windows HKLM; admin). @p key NULL removes every one.
 */
enum u_setting_write_result
u_setting_machine_set_preferred_plugin_for_screen(const char *key, const char *plugin_id);

/*!
 * @name Pure helpers behind the chain (unit-tested)
 * @{
 */
//! Look @p key up in a `"<key>=<id>;..."` spec (whitespace trimmed, key case-insensitive).
bool
u_setting_per_screen_env_lookup(const char *spec, const char *key, char *buf, size_t cap);

//! Look @p key up in the `preferred_plugin_per_screen` object of a settings-file text.
bool
u_setting_per_screen_json_lookup(const char *json_text, const char *key, char *buf, size_t cap);

/*!
 * The chain over already-read tiers: @p env_spec (tier 1), @p user_json (the
 * settings-file text, tier 2), @p machine_value (tier 3's value for @p key);
 * each may be NULL. Same contract as
 * @ref u_setting_get_preferred_plugin_for_screen.
 */
const char *
u_setting_per_screen_resolve(const char *key,
                             const char *env_spec,
                             const char *user_json,
                             const char *machine_value,
                             char *buf,
                             size_t cap,
                             enum u_setting_source *out_source);
/*! @} */

#ifdef __cplusplus
}
#endif
