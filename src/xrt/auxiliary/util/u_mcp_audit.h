// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Append-only audit log for MCP write tools (Phase B §5).
 * @ingroup aux_util
 *
 * Every call to u_mcp_audit_append() writes one line:
 *
 *   {ts_ns},{tool},{client_id},{args_hash}\n
 *
 * Location:
 *   %APPDATA%\DisplayXR\mcp-audit.log        (Windows)
 *   ~/.config/displayxr/mcp-audit.log        (POSIX)
 *
 * Size-based rotation kicks in at ~1 MB — the previous file is renamed
 * to @c mcp-audit.log.1 (overwriting any earlier rotation). Only the
 * current and one prior log are kept to bound disk use.
 *
 * Thread-safe: all writes go through a mutex so concurrent tool handlers
 * don't interleave partial lines.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Append one audit entry. @p client_id may be 0 when the tool is not
 * per-client (e.g. @c save_workspace); @p args_hash should be a
 * stable hash of the tool's parameters (callers typically feed a djb2
 * or FNV over the serialized JSON). Non-blocking best-effort; failures
 * are logged via U_LOG_W and otherwise ignored.
 */
void
u_mcp_audit_append(const char *tool, uint32_t client_id, uint64_t args_hash);

/*!
 * Return the absolute path of the current audit log. Thread-safe. Empty
 * string if the audit module has not been initialized yet (first
 * u_mcp_audit_append initializes lazily).
 */
const char *
u_mcp_audit_path(void);

#ifdef __cplusplus
}
#endif
