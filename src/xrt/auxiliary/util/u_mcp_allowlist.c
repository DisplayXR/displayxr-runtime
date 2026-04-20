// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Allowlist implementation (Phase B §5).
 * @ingroup aux_util
 */

#include "u_mcp_allowlist.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 64

static struct
{
	bool initialized;
	bool has_policy;
	uint32_t entries[MAX_ENTRIES];
	size_t n;
} g_state;


void
u_mcp_allowlist_init(void)
{
	if (g_state.initialized) {
		return;
	}
	g_state.initialized = true;
	g_state.n = 0;

	const char *env = getenv("DISPLAYXR_MCP_ALLOW");
	if (env == NULL) {
		g_state.has_policy = false;
		return;
	}

	// Env set but empty → policy that allows nothing.
	g_state.has_policy = true;

	// Parse comma-separated ids. Silently skip malformed tokens; the
	// intent of the allowlist is defensive, not self-diagnostic.
	const char *p = env;
	while (*p != '\0' && g_state.n < MAX_ENTRIES) {
		while (*p == ' ' || *p == ',') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		char *end = NULL;
		long v = strtol(p, &end, 10);
		if (end == p || v < 0 || v > (long)UINT32_MAX) {
			if (end == p) {
				p++;
			} else {
				p = end;
			}
			continue;
		}
		g_state.entries[g_state.n++] = (uint32_t)v;
		p = end;
	}
}

bool
u_mcp_allowlist_has_policy(void)
{
	return g_state.initialized && g_state.has_policy;
}

bool
u_mcp_allowlist_allows(uint32_t client_id)
{
	if (!g_state.initialized || !g_state.has_policy) {
		return true;
	}
	for (size_t i = 0; i < g_state.n; i++) {
		if (g_state.entries[i] == client_id) {
			return true;
		}
	}
	return false;
}
