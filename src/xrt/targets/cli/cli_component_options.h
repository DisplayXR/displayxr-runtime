// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `options | set | unset` verbs, one spelling for every component
 *         class (ADR-054 decision 7, N3c): `<class> options <id> [--json]`,
 *         `<class> set <id> <KEY> <value> [--machine]`,
 *         `<class> unset <id> <KEY> [--machine]`.
 *
 * `dp`, `input` and `conv` each describe themselves in a
 * @ref cli_component_class row and hand their argv to these; the logic —
 * the registered-id gate, key validation, the store write, the "effective
 * now" read-back and the messages — lives here once, so the three verbs
 * cannot drift the way `enable|disable` had to be written three times.
 *
 * The store is the shared component loader's
 * (`target_component_get_setting` / `set` / `unset`,
 * `docs/specs/runtime/plugin-discovery.md` § Component settings): per user
 * by default (HKCU `<Root>\<id>\Settings\<KEY>` / the POSIX
 * `<id>.settings.json`), machine-wide with `--machine` (HKLM 64-bit view,
 * admin; refused on POSIX, where the manifest's `settings` object is edited
 * by hand). The environment `DXR_<CLASS>_<ID>_<KEY>` of the hosting process
 * always wins, and `options` says so when it does. A component reads its
 * settings at init through its host iface, so a write applies to the next
 * process that loads it — each class states exactly when in its
 * @ref cli_component_class::note_pickup.
 *
 * @ingroup cli
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct target_component_class;
struct target_component_setting_desc;
struct xrt_component_license_state;
struct xrt_component_setting_status;
struct cJSON;

//! One component class, as the shared verbs see it.
struct cli_component_class
{
	//! The CLI noun: "dp" / "input" / "conv" (usage lines, JSON `class`).
	const char *verb;
	//! Human name: "display processor" / "input provider" / "conversion provider".
	const char *noun;
	//! The class's row in the shared component loader (the store is keyed on it).
	const struct target_component_class *cls;
	//! Is @p id registered (HKLM row / manifest), enabled or not? Enumeration only.
	bool (*registered)(const char *id);
	/*!
	 * The settings @p id declares (manifest `settings_schema` + the binary's
	 * `describe_settings`), into @p out (up to @p max); may be NULL. May load
	 * the registered binary (the same exposure as `list`'s status snapshot).
	 */
	int (*describe)(const char *id, struct target_component_setting_desc *out, int max);
	//! Print the class's own "when does this apply" note after a write.
	void (*note_pickup)(void);
	//! POSIX: where the machine value lives instead ("the manifest's \"settings\" object (NNN-<id>.json)").
	const char *posix_machine_hint;
	/*!
	 * ADR-054 N5: hand an activation token to @p id's `activate` slot and
	 * return an `enum target_component_activate_result` (the class loader's
	 * `target_*_activate`). NULL = the class has no `activate` verb.
	 */
	int (*activate)(const char *id,
	                const char *token,
	                struct xrt_component_license_state *out_state,
	                char *err,
	                size_t err_cap);
	/*!
	 * Whether the value in force of each of @p keys takes effect on this
	 * machine (`xrt_component_get_setting_status_fn_t`): fill @p out[i] for
	 * @p keys[i], one load of the component for the whole list. NULL = the
	 * class has no such slot yet (every setting reads `unknown`).
	 */
	bool (*setting_status)(const char *id,
	                       const char *const *keys,
	                       int count,
	                       struct xrt_component_setting_status *out);
};

/*!
 * `<class> options <id> [--json]`: every declared / stored key with type,
 * default, value and source, plus — when the component reports it through
 * @ref cli_component_class::setting_status — whether the value in force
 * takes effect here (`status`: `applies | not_supported | restart_required |
 * unknown`, the value used instead, the component's note).
 */
int
cli_component_options(const struct cli_component_class *k, const char *id, bool json);

//! `<class> set <id> <KEY> <value> [--machine]`. Refuses an unregistered id, an invalid key; writes nothing then.
int
cli_component_set(
    const struct cli_component_class *k, const char *id, const char *key, const char *value, bool machine);

/*!
 * `<class> set <id> <KEY> --stdin [--machine]`: the value is ONE line of
 * standard input (the trailing newline stripped; empty or longer than the
 * store takes is refused, nothing written). Never echoed; wiped after the write.
 */
int
cli_component_set_stdin(const struct cli_component_class *k, const char *id, const char *key, bool machine);

//! `<class> unset <id> <KEY> [--machine]`. Absent is success.
int
cli_component_unset(const struct cli_component_class *k, const char *id, const char *key, bool machine);

/*!
 * Dispatch `options | set | unset` from a class's argv (argv[2] is the verb,
 * argv[3..] its arguments; `--json` / `--machine` anywhere after). Returns
 * the verb's exit code, or -1 when argv[2] is none of the three (the caller
 * goes on to its own verbs). Prints the usage line on a malformed call.
 */
int
cli_component_options_dispatch(const struct cli_component_class *k, int argc, const char **argv);

//! The three usage lines, for a class's own `print_usage` ("  options <id> [--json] ...").
void
cli_component_options_usage(const struct cli_component_class *k);

/*
 *
 * Licence state (ADR-054 N5, ADR-064 §5): the `licence` object and column of
 * every class's `list`, and the `activate` verb.
 *
 */

/*!
 * Add the `licence` object to a `list --json` row:
 * `{"state", "source", "expires_unix", "expires", "message", "purchase_url",
 * "activation": {"key", "deep_link"}, "blocks"}`. `state` is one of
 * `not_applicable | licensed | trial | expired | unlicensed`; `source` is
 * `hardware | key | trial | oem` or null; `expires_unix` (seconds, UTC) and
 * `expires` (ISO 8601) are null when the component states none; `blocks` is
 * true when selection skips the component (`unlicensed`, `expired`).
 */
void
cli_component_license_json(struct cJSON *row, const struct xrt_component_license_state *lic);

/*!
 * The human cell of a `list` row, into @p buf: "-" (nothing to report),
 * "licensed (hardware)", "trial until 2026-12-31", "UNLICENSED",
 * "EXPIRED 2026-10-01". Returns @p buf.
 */
const char *
cli_component_license_cell(const struct xrt_component_license_state *lic, char *buf, size_t cap);

/*!
 * `<class> activate <id> <token | @file> [--json]`: read the token (the
 * argument itself, or the first line of `@file`), hand it to the component's
 * `activate` slot through @ref cli_component_class::activate, and print the
 * new licence state or the component's error line. The token is never
 * printed. Exit 0 = accepted.
 */
int
cli_component_activate(const struct cli_component_class *k, const char *id, const char *token_arg, bool json);

#ifdef __cplusplus
}
#endif
