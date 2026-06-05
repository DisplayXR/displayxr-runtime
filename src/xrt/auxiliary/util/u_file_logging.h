// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  File logging for DisplayXR - writes logs to %LOCALAPPDATA%/DisplayXR
 * @author David Fattal
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_api.h"
#include "util/u_logging.h"

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Initialize file logging to %LOCALAPPDATA%/DisplayXR.
 * Creates a log file with process name, PID, and timestamp:
 * DisplayXR_<exe>.<pid>_YYYY-MM-DD_HH-MM-SS.log
 *
 * Safe to call multiple times - only initializes once.
 * Automatically called by u_log on first log message on Windows.
 */
XRT_API_FUNC void
u_file_logging_init(void);

/*!
 * Write a pre-formatted message to the log file.
 * Used by oxr_logger to route state tracker logs to the file.
 * Safe to call before init or after shutdown (will be a no-op).
 */
XRT_API_FUNC void
u_file_logging_write_raw(const char *msg);

/*!
 * Write a formatted message (va_list form) to the log file with the standard
 * timestamp/level/function prefix. Used by the MCP log sink to tee U_LOG_*
 * messages into the per-process file log while also appending them to the MCP
 * ring (issue #433 — the ring-only sink truncated the file log at instance
 * creation). Safe to call before init or after shutdown (will be a no-op).
 */
XRT_API_FUNC void
u_file_logging_write_va(const char *func, enum u_logging_level level, const char *format, va_list args);

/*!
 * Close the log file and clean up resources.
 * Called automatically at process exit.
 */
XRT_API_FUNC void
u_file_logging_shutdown(void);

#ifdef __cplusplus
}
#endif
