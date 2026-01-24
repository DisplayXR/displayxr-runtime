// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  File logging for SRMonado - writes logs to %LOCALAPPDATA%/LeiaSR/SRMonado
 * @author David Fattal
 * @ingroup aux_util
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Initialize file logging to %LOCALAPPDATA%/LeiaSR/SRMonado.
 * Creates a log file with timestamp: SRMonado_YYYY-MM-DD_HH-MM-SS.log
 *
 * Safe to call multiple times - only initializes once.
 * Automatically called by u_log on first log message on Windows.
 */
void
u_file_logging_init(void);

/*!
 * Close the log file and clean up resources.
 * Called automatically at process exit.
 */
void
u_file_logging_shutdown(void);

#ifdef __cplusplus
}
#endif
