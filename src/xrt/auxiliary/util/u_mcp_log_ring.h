// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Opt-in MPSC log ring used by the MCP @c tail_log tool.
 *
 * Installs a @ref u_log_set_sink callback that formats each log line
 * into a fixed-size entry in a ring buffer. Readers pull by sequence
 * cursor — entries older than the reader's last-seen cursor by more
 * than the ring size are reported as dropped.
 *
 * @ingroup aux_util
 */

#pragma once

#include "util/u_logging.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_MCP_LOG_MAX_TEXT 512

struct u_mcp_log_entry
{
	uint64_t seq;
	int64_t timestamp_ns;
	enum u_logging_level level;
	char text[U_MCP_LOG_MAX_TEXT];
};

/*!
 * Install the global log sink and start buffering. Idempotent.
 * Safe to call before any logging has happened.
 */
void
u_mcp_log_ring_start(void);

/*!
 * Uninstall the sink, free the ring. Idempotent.
 */
void
u_mcp_log_ring_stop(void);

/*!
 * Read up to @p max_entries entries published with @c seq > @p since.
 * Fills @p out_entries, writes the new cursor to @p out_next_cursor,
 * and reports how many entries were lost (overwrite > @p since cursor).
 */
void
u_mcp_log_ring_read(uint64_t since,
                    struct u_mcp_log_entry *out_entries,
                    size_t max_entries,
                    size_t *out_count,
                    uint64_t *out_next_cursor,
                    uint64_t *out_dropped);

#ifdef __cplusplus
}
#endif
