// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host logging (ADR-053 roadmap §3.4 "Logs").
 *
 * Every line goes to `DisplayXR_conversion-host_<provider>.<pid>_<ts>.log`
 * (Windows: %LOCALAPPDATA%\DisplayXR, falling back to %TEMP%; macOS:
 * ~/Library/Logs/DisplayXR; Linux: $XDG_STATE_HOME/displayxr or
 * ~/.local/state/displayxr; /tmp as the last resort). WARN and ERROR lines are
 * also queued for the control channel, which forwards them to the service log
 * as `[conv:<provider>] ...`. The queue is bounded; overflow drops the oldest
 * line and counts it.
 *
 * The provider's own u_log calls reach the same sink: on Windows and macOS the
 * provider imports u_log* from the runtime library, so the host points that
 * library's sink here too (@ref conv_host_log_adopt_provider_logger).
 *
 * @ingroup conv_host
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Bound on WARN+ lines waiting for the channel.
#define CONV_HOST_LOG_FORWARD_MAX 64u
//! Longest forwarded line (bytes incl. NUL); longer lines are truncated.
#define CONV_HOST_LOG_LINE_MAX 480u

/*!
 * Open the log file and install the u_log sink. Safe to call once; logging
 * before it goes to the platform default (stderr / debugger).
 * @return true when a file was opened.
 */
bool
conv_host_log_init(const char *provider_id);

//! The file being written ("" when none).
const char *
conv_host_log_path(void);

/*!
 * Route the logger of the runtime library the provider imports u_log* from
 * (Windows: DisplayXRClient.dll; macOS: the runtime dylib) to this sink, so
 * the provider's lines land in this file and are forwarded too. A no-op when
 * the provider links its own copy (ELF builds) or none is loaded.
 */
void
conv_host_log_adopt_provider_logger(void *provider_module);

/*!
 * Pop queued WARN+ lines, oldest first, calling @p fn for each (no lock held
 * during the call). Returns how many were delivered; *out_dropped (optional)
 * gets the lines lost to overflow since the previous drain.
 */
uint32_t
conv_host_log_drain(void (*fn)(void *ctx, uint32_t level, const char *line), void *ctx, uint32_t *out_dropped);

//! Write the footer and flush (registered with atexit; the file stays open).
void
conv_host_log_shutdown(void);

#ifdef __cplusplus
}
#endif
