// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `input list` prediction rule: which registered provider the
 *         loader would rank first, from the enumeration alone.
 *
 * Pure so it can be pinned by `tests_cli_input_predict` without a registry
 * or a manifest dir. The rule mirrors `input_discover_all`
 * (`target_input_plugin_loader.c`, N3a / ADR-054): the `PreferredPlugin`
 * override wins when it names a registered, enabled provider; otherwise the
 * lowest ProbeOrder among the enabled ones (first wins on ties — the loader
 * breaks ties by id, and the enumeration already delivers that order). A
 * disabled provider is never predicted, preferred or not: `Enabled=0` is
 * the stronger verb.
 *
 * @ingroup cli
 */

#pragma once

#include "target_input_plugin_loader.h"

#include <string.h>


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Index into @p list of the provider the loader would rank first, or -1 when
 * no enabled provider is registered. @p preferred may be NULL or "".
 */
static inline int
cli_input_predict_active(const struct target_input_plugin_desc *list, int n, const char *preferred)
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
 * Is @p id a usable provider id for the `enable|disable|use` verbs: non-empty,
 * shorter than the 64-byte id field, no path separators (it names a registry
 * subkey), not flag-shaped.
 */
static inline bool
cli_input_id_valid(const char *id)
{
	if (id == NULL || id[0] == '\0' || id[0] == '-' || strlen(id) >= 64) {
		return false;
	}
	for (const char *c = id; *c != '\0'; c++) {
		if (*c == '\\' || *c == '/') {
			return false;
		}
	}
	return true;
}

#ifdef __cplusplus
}
#endif
