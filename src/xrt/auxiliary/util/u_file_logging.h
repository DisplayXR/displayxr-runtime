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
 * Close the log file and clean up resources.
 * Called automatically at process exit.
 */
XRT_API_FUNC void
u_file_logging_shutdown(void);

#ifdef __cplusplus
}
#endif
