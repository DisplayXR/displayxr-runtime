// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: the per-OS process duties os/os_process.h
 *         does not cover (ADR-053 roadmap §3.4, §3.6). Elevation
 *         (os_process_is_elevated), the parent-death watch
 *         (os_process_watch_parent), the channel and the wake signals come
 *         from os_process.h.
 *
 *  - Crash forensics: Windows writes an in-host minidump from an unhandled
 *    exception filter (not DIE_ON_UNHANDLED_EXCEPTION, which bypasses dumps)
 *    to %LOCALAPPDATA%\DisplayXR\dumps; POSIX raises RLIMIT_CORE so the
 *    system core pattern applies.
 *  - Session end (Windows): a hidden TOP-LEVEL window on its own pumping
 *    thread answers WM_QUERYENDSESSION (Restart Manager's close request,
 *    ENDSESSION_CLOSEAPP) / WM_ENDSESSION / WM_CLOSE by asking the host to
 *    exit 20, with a bounded backstop.
 *
 * @ingroup conv_host
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Install the crash writer (minidump / core). Call once, early.
void
conv_host_os_install_crash_handler(const char *provider_id);

/*!
 * Windows: start the hidden session-end window. @p on_close runs (once, on the
 * window's thread, must not block) when Restart Manager / logoff / WM_CLOSE
 * asks the process to close; if the process is still alive @p backstop_ms
 * later it exits with code 20 itself. POSIX: a no-op returning true.
 */
bool
conv_host_os_start_session_end_watch(void (*on_close)(void *ctx), void *ctx, uint32_t backstop_ms);

//! Windows: the session-end window, for tests (NULL elsewhere / before start).
void *
conv_host_os_session_end_hwnd(void);


#ifdef __cplusplus
}
#endif
