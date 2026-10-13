// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: the per-OS process duties (ADR-053
 *         roadmap §3.4, §3.6).
 *
 *  - Integrity: the host inherits the service token (medium IL) and refuses
 *    to run elevated (exit 14). Windows: TokenElevation. POSIX: effective
 *    root, or a set-uid launch (euid != ruid).
 *  - Crash forensics: Windows writes an in-host minidump from an unhandled
 *    exception filter (not DIE_ON_UNHANDLED_EXCEPTION, which bypasses dumps)
 *    to %LOCALAPPDATA%\DisplayXR\dumps; POSIX raises RLIMIT_CORE so the
 *    system core pattern applies.
 *  - Session end (Windows): a hidden TOP-LEVEL window on its own pumping
 *    thread answers WM_QUERYENDSESSION (Restart Manager's close request,
 *    ENDSESSION_CLOSEAPP) / WM_ENDSESSION / WM_CLOSE by asking the host to
 *    exit 20, with a bounded backstop.
 *  - Parent death (POSIX): Linux PR_SET_PDEATHSIG, macOS a kqueue NOTE_EXIT
 *    watch. Windows relies on the supervisor's KILL_ON_JOB_CLOSE job object
 *    (the host never holds a handle to the service process) and on the
 *    control channel breaking.
 *
 * @ingroup conv_host
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Is this process elevated (see above)? @p out_detail (optional, >= 64 bytes)
 * says what was seen, for the log line.
 */
bool
conv_host_os_is_elevated(char *out_detail, uint32_t detail_size);

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

/*!
 * POSIX: die with the parent (@p parent_pid): Linux PR_SET_PDEATHSIG(SIGKILL)
 * plus a getppid check for the spawn race, macOS a kqueue NOTE_EXIT thread
 * that _exit()s. Returns false when the parent is already gone. Windows: true
 * (the job object does it).
 */
bool
conv_host_os_watch_parent(uint64_t parent_pid);

//! This process's id.
uint64_t
conv_host_os_pid(void);

#ifdef __cplusplus
}
#endif
