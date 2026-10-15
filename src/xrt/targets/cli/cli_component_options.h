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

#ifdef __cplusplus
extern "C" {
#endif

struct target_component_class;
struct target_component_setting_desc;

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
};

//! `<class> options <id> [--json]`: every declared / stored key with type, default, value and source.
int
cli_component_options(const struct cli_component_class *k, const char *id, bool json);

//! `<class> set <id> <KEY> <value> [--machine]`. Refuses an unregistered id, an invalid key; writes nothing then.
int
cli_component_set(
    const struct cli_component_class *k, const char *id, const char *key, const char *value, bool machine);

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

#ifdef __cplusplus
}
#endif
