// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `dp list` prediction rule: which registered display processor
 *         the loader would select, from the enumeration alone.
 *
 * Pure so it can be pinned by `tests_cli_dp_predict` without a registry or a
 * manifest dir. The rule mirrors `discover_active_plugin`
 * (`target_plugin_loader.c`, N3b / ADR-054): the `PreferredPlugin` override
 * wins when it names a registered, ENABLED plug-in; otherwise the lowest
 * ProbeOrder among the enabled ones (first wins on ties — the loader breaks
 * ties by id, and the enumeration already delivers that order). A disabled
 * plug-in is never predicted, preferred or not: `Enabled=0` is the stronger
 * verb, and the loader falls through to ProbeOrder with one WARN.
 *
 * What it cannot know: whether a plug-in's `probe()` will decline (no panel
 * attached). `*` is "the one the loader tries first and that wins if it
 * claims"; `selftest` reports what actually happened.
 *
 * @ingroup cli
 */

#pragma once

#include "cli_input_predict.h"
#include "target_plugin_loader.h"

#include <stdbool.h>
#include <string.h>


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Index into @p list of the plug-in the loader would select, or -1 when no
 * enabled plug-in is registered. @p preferred may be NULL or "".
 */
static inline int
cli_dp_predict_active(const struct target_plugin_desc *list, int n, const char *preferred)
{
	if (preferred != NULL && preferred[0] != '\0') {
		for (int i = 0; i < n; i++) {
			if (list[i].enabled && strcmp(list[i].id, preferred) == 0) {
				return i;
			}
		}
	}
	int best = -1;
	for (int i = 0; i < n; i++) {
		if (!list[i].enabled) {
			continue;
		}
		if (best < 0 || list[i].probe_order < list[best].probe_order) {
			best = i;
		}
	}
	return best;
}

/*!
 * Index of the registered plug-in @p id in @p list, or -1. The gate behind
 * `dp enable|disable`: a per-user `Enabled` for an id that is not registered
 * (HKLM row / manifest) would be an orphan row `dp list` can never show, so
 * the verb refuses instead of writing it. (`dp use` deliberately does NOT
 * gate on this — a preference ahead of registration is how a mid-install
 * pin has always worked, and `input use` / `conv use` match.)
 */
static inline int
cli_dp_find(const struct target_plugin_desc *list, int n, const char *id)
{
	if (id == NULL) {
		return -1;
	}
	for (int i = 0; i < n; i++) {
		if (strcmp(list[i].id, id) == 0) {
			return i;
		}
	}
	return -1;
}

/*!
 * Is @p id a usable plug-in id for the `enable|disable|use` verbs? The same
 * rule as the input provider's (it names a registry subkey of the same
 * shape): non-empty, shorter than the 64-byte id field, no path separators,
 * not flag-shaped.
 */
static inline bool
cli_dp_id_valid(const char *id)
{
	return cli_input_id_valid(id);
}

#ifdef __cplusplus
}
#endif
