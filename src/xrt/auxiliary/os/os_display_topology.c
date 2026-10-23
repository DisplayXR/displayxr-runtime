// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Topology signature hash — see os_display_topology.h.
 * @ingroup aux_os
 */

#include "os_display_topology.h"

#include <stddef.h>
#include <string.h>

static uint64_t
fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	for (size_t i = 0; i < n; i++) {
		h = (h ^ (uint64_t)b[i]) * 1099511628211ull;
	}
	return h;
}

uint64_t
os_display_topology_hash(const struct os_display_topology_entry *entries, uint32_t count)
{
	uint64_t h = 1469598103934665603ull;
	if (entries == NULL) {
		count = 0;
	}
	h = fnv(h, &count, sizeof(count));
	for (uint32_t i = 0; i < count; i++) {
		const struct os_display_topology_entry *e = &entries[i];
		h = fnv(h, e->key, strnlen(e->key, sizeof(e->key)));
		const uint8_t sep = 0xff; // "ab"+"c" != "a"+"bc"
		h = fnv(h, &sep, 1);
		const int32_t pos[2] = {e->left, e->top};
		const uint32_t rest[3] = {e->state, e->width, e->height};
		h = fnv(h, pos, sizeof(pos));
		h = fnv(h, rest, sizeof(rest));
	}
	return h;
}
