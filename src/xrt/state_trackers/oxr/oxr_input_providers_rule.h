// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_input_providers event discipline (ADR-034 Amendment 5, #2181):
 *         when may xrSetInputProviderPreferenceDXR queue
 *         XrEventDataInputProvidersChangedDXR? Pure, header-only, pinned by
 *         tests_oxr_input_providers_rule.
 *
 * The rule: ONE event per actual change, none for a no-op. A holder move is
 * announced by the next xrSyncActions (which sees the roles generation move);
 * the setter therefore queues an event only when the stored request / pin
 * changed and NO holder moved — the one change xrSyncActions cannot see.
 *
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_input_selection.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Upper bound on providers one view holds.
#define OXR_INPUT_PROVIDERS_VIEW_MAX 64

/*!
 * What a set can change about this app's view, per provider: the families it
 * holds for this app, the families this app requests it for, whether it is the
 * app's pin. Compared before / after a set to decide the event (below).
 */
struct pref_view
{
	uint32_t n;
	char id[OXR_INPUT_PROVIDERS_VIEW_MAX][XRT_INPUT_PROVIDER_ID_SIZE];
	uint32_t current[OXR_INPUT_PROVIDERS_VIEW_MAX];
	uint32_t requested[OXR_INPUT_PROVIDERS_VIEW_MAX];
	bool pinned[OXR_INPUT_PROVIDERS_VIEW_MAX];
};

//! Families whose HOLDER differs between @p a and @p b; @p out_pref_changed = the request / pin moved.
static inline uint32_t
diff_pref_view(const struct pref_view *a, const struct pref_view *b, bool *out_pref_changed)
{
	uint32_t holder_moved = 0;
	bool pref_changed = a->n != b->n;
	for (uint32_t i = 0; i < a->n && i < b->n; i++) {
		if (strcmp(a->id[i], b->id[i]) != 0) {
			pref_changed = true; // the provider set itself moved
			holder_moved |= XRT_INPUT_ROLE_FAMILY_ALL_BITS;
			continue;
		}
		holder_moved |= a->current[i] ^ b->current[i];
		pref_changed = pref_changed || a->requested[i] != b->requested[i] || a->pinned[i] != b->pinned[i];
	}
	*out_pref_changed = pref_changed;
	return holder_moved;
}

/*!
 * Should the setter queue the event, given the view before and after it?
 * True only when the request / pin changed and no holder moved.
 */
static inline bool
oxr_input_providers_setter_should_notify(const struct pref_view *before, const struct pref_view *after)
{
	bool pref_changed = false;
	const uint32_t holder_moved = diff_pref_view(before, after, &pref_changed);
	return holder_moved == 0 && pref_changed;
}

#ifdef __cplusplus
}
#endif
