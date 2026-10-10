// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service orchestrator configuration (persisted to service.json).
 * @ingroup ipc
 */

#pragma once

#include "service_hotkey.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Lifecycle mode for a managed child process (the workspace controller).
 */
enum service_child_mode
{
	SERVICE_CHILD_ENABLE,  //!< Always running — launched at service startup.
	SERVICE_CHILD_DISABLE, //!< Never running — not launched, hotkeys not registered.
	SERVICE_CHILD_AUTO,    //!< On-demand — launched when triggered (hotkey, client detection).
};

//! Maximum length of the workspace controller binary path.
#define SERVICE_WORKSPACE_BINARY_MAX 260

//! Maximum length of a workspace controller id (registry subkey name), incl. NUL.
#define SERVICE_CONTROLLER_ID_MAX 64

//! Maximum number of per-controller launch-setting entries kept in service.json.
#define SERVICE_CONTROLLERS_MAX 16

/*!
 * Per-controller hotkey setting as stored in service.json.
 */
enum service_hotkey_setting
{
	SERVICE_HOTKEY_SETTING_DEFAULT = 0, //!< key absent → SERVICE_HOTKEY_DEFAULT (Ctrl+Space)
	SERVICE_HOTKEY_SETTING_NONE,        //!< `"hotkey": null` → no hotkey
	SERVICE_HOTKEY_SETTING_COMBO,       //!< `"hotkey": "<combo>"` (canonical spelling)
};

/*!
 * User launch settings for one workspace controller, keyed by its registry id
 * (`service.json` → `"controllers": { "<id>": { "hotkey": …, "mode": … } }`).
 *
 * Entries only exist while they differ from the defaults (Ctrl+Space, auto):
 * every setter prunes an entry that went back to the defaults, so "an entry
 * exists" is what the CLI reports as `"source": "user"`.
 */
struct service_controller_launch
{
	char id[SERVICE_CONTROLLER_ID_MAX];
	bool has_mode;                //!< false → mode not customised
	enum service_child_mode mode; //!< SERVICE_CHILD_AUTO or SERVICE_CHILD_DISABLE
	enum service_hotkey_setting hotkey_setting;
	char hotkey[SERVICE_HOTKEY_MAX]; //!< canonical combo when hotkey_setting == COMBO
};

/*!
 * A controller's launch settings with every default applied.
 */
struct service_launch_resolved
{
	enum service_child_mode mode;         //!< effective lifecycle mode (ENABLE only for the active one)
	bool has_hotkey;                      //!< false → `--no-hotkey`
	struct service_hotkey hotkey;         //!< valid when has_hotkey
	char hotkey_text[SERVICE_HOTKEY_MAX]; //!< canonical combo, "" when !has_hotkey
	bool user;                            //!< customised (JSON `"source": "user"`)
};

/*!
 * Persisted service configuration.
 *
 * Stored in `%LOCALAPPDATA%\DisplayXR\service.json` on Windows,
 * `~/.config/displayxr/service.json` on Linux/macOS.
 *
 * Missing file or missing keys → defaults (auto/true, Ctrl+Space).
 */
struct service_config
{
	enum service_child_mode workspace; //!< ACTIVE workspace controller's lifecycle mode (default AUTO).
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

	//! Per-controller launch settings (display dashboard phase 8). The
	//! top-level `workspace` above keeps meaning "the ACTIVE controller's
	//! mode" for backward compatibility; the setters below keep both in step
	//! and, when they disagree in a hand-edited file, the per-controller entry
	//! wins (see service_config_resolve_launch).
	uint32_t controller_count;
	struct service_controller_launch controllers[SERVICE_CONTROLLERS_MAX];
};

//! Return value of service_config_pick_controller for a dev-path override.
#define SERVICE_PICK_DEV_OVERRIDE (-2)

struct workspace_controller_entry;

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

/*!
 * Reset @p cfg to the defaults (what a missing service.json means).
 */
void
service_config_set_defaults(struct service_config *cfg);

/*!
 * Parse a service.json document into @p cfg (which the caller has set to the
 * defaults). Missing / malformed keys keep their defaults; never fails.
 * Exposed for unit tests — service_config_load reads the file and calls this.
 */
void
service_config_parse_json(struct service_config *cfg, const char *json_text);

/*!
 * Serialise @p cfg as service.json text (malloc'd, caller frees with free()).
 * Returns NULL on OOM. Exposed for unit tests.
 */
char *
service_config_to_json(const struct service_config *cfg);

/*!
 * Full path of service.json (`%LOCALAPPDATA%\DisplayXR\service.json`,
 * `$XDG_CONFIG_HOME/displayxr/service.json`). Returns false when the base
 * directory cannot be resolved.
 */
bool
service_config_path(char *buf, size_t buf_size);

/*!
 * The active-controller selection rule — which registered controller the
 * service spawns. Pure (no registry access):
 *
 *  1. `workspace_binary` containing a path separator → dev-mode absolute-path
 *     override → SERVICE_PICK_DEV_OVERRIDE (the caller launches that path; no
 *     registered entry is active).
 *  2. @p n <= 0 → -1 (no controller).
 *  3. `workspace_binary` a non-empty id matching an entry (case-insensitive on
 *     Windows, exact elsewhere) → that index.
 *  4. Otherwise → 0 (the first enumerated entry).
 */
int
service_config_pick_controller(const struct service_config *cfg,
                               const struct workspace_controller_entry *entries,
                               int n);

/*!
 * Resolve controller @p id's launch settings with every default applied.
 * @p is_active says whether @p id is the active controller: only then does the
 * top-level `workspace` mode take part (and only then can the mode be ENABLE).
 */
void
service_config_resolve_launch(const struct service_config *cfg,
                              const char *id,
                              bool is_active,
                              struct service_launch_resolved *out);

/*!
 * Set controller @p id's hotkey. @p combo NULL → no hotkey; otherwise it must
 * parse (it is stored canonicalised; the default combo prunes back to
 * "default"). Returns false (cfg untouched) when @p combo does not parse, @p id
 * is empty / too long, or the entry table is full.
 */
bool
service_config_set_controller_hotkey(struct service_config *cfg, const char *id, const char *combo);

/*!
 * Set controller @p id's mode (SERVICE_CHILD_AUTO or SERVICE_CHILD_DISABLE).
 * When @p is_active, the top-level `workspace` follows (back-compat).
 * Returns false on a bad mode / id / full table.
 */
bool
service_config_set_controller_mode(struct service_config *cfg,
                                   const char *id,
                                   enum service_child_mode mode,
                                   bool is_active);

/*!
 * Forget every customisation of controller @p id (back to Ctrl+Space, auto).
 * When @p is_active the top-level `workspace` goes back to auto too.
 */
void
service_config_reset_controller(struct service_config *cfg, const char *id, bool is_active);

/*!
 * The tray changed the top-level `workspace` mode: mirror it into the active
 * controller's entry so both spellings agree (DISABLE → `"mode": "disabled"`,
 * anything else → mode not customised). No-op for an empty @p active_id.
 */
void
service_config_sync_active_mode(struct service_config *cfg, const char *active_id);

/*!
 * "auto" / "disabled" — the per-controller mode spelling of the contract
 * (ENABLE, the legacy always-on mode, reports as "auto").
 */
const char *
service_config_launch_mode_str(enum service_child_mode mode);

/*!
 * Live state of a controller as seen by the running service (CLI only).
 */
struct service_controller_live
{
	bool known;     //!< false → JSON nulls (no service reachable / headless)
	bool connected; //!< a CONTROLLER-class client runs this controller's binary
	long pid;       //!< that client's pid (0 when !connected)
};

/*!
 * Build the `workspace list --json` document:
 * `{ "active_id", "dev_override", "controllers": [ … ] }`. @p live may be
 * NULL (headless: `connected` / `pid` null) or an array of @p n. Returns a
 * `cJSON *` (as `void *` so this header stays free of cJSON); the caller owns
 * it.
 */
void *
service_workspace_controllers_to_cjson(const struct service_config *cfg,
                                       const struct workspace_controller_entry *entries,
                                       int n,
                                       const struct service_controller_live *live);

#ifdef __cplusplus
}
#endif
