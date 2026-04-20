// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-client_id allowlist for MCP write tools (Phase B §5).
 * @ingroup aux_util
 *
 * Source of truth: the @c DISPLAYXR_MCP_ALLOW environment variable, read
 * once at @c u_mcp_allowlist_init(). Format: comma-separated decimal
 * client_ids, or empty/unset to allow all clients. Examples:
 *
 * @code
 *   DISPLAYXR_MCP_ALLOW=1,2,3     # only these three
 *   DISPLAYXR_MCP_ALLOW=          # allow nothing
 *   (unset)                       # allow all (default)
 * @endcode
 *
 * This is deliberately a trivial data structure — Phase B ships the
 * safety-model scaffolding; richer policies (per-tool, per-role) land
 * when the §5 audit review is completed.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Parse @c DISPLAYXR_MCP_ALLOW into the process-wide allowlist. Idempotent.
 * Safe to call before the MCP server starts.
 */
void
u_mcp_allowlist_init(void);

/*!
 * Is the env var set to an explicit list? When false, every client_id is
 * allowed. Callers may use this to skip allowlist checks entirely in the
 * common case where no policy is configured.
 */
bool
u_mcp_allowlist_has_policy(void);

/*!
 * Is the given @p client_id allowed by the current policy? Always returns
 * true when @c u_mcp_allowlist_has_policy() is false.
 */
bool
u_mcp_allowlist_allows(uint32_t client_id);

#ifdef __cplusplus
}
#endif
