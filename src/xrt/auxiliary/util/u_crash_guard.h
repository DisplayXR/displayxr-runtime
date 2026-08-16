// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Exit / terminate tripwires for long-lived host processes (#950).
 *
 * The service is a single process hosting vendor DP + input-provider DLLs and
 * one IPC thread per client. Two silent-death classes have no forensic record
 * today: an in-process DLL calling `exit()` (the log ends with the atexit
 * banner and zero teardown chatter — #943) and an exception escaping a thread
 * entry with no handler (`std::terminate` / WER, no banner — #930). MSVC's
 * `std::set_terminate` is per-thread and a window-proc throw never reaches a
 * try/catch further up the thread, so the only reliable places to observe both
 * are (a) an `atexit` handler — it runs on the thread that called `exit()`,
 * with that thread's stack intact — and (b) a structured-exception filter at
 * each thread entry, which runs *before* unwinding, on the faulting stack.
 *
 * Neither hook changes behaviour: the exit still exits, the exception still
 * propagates (`EXCEPTION_CONTINUE_SEARCH`), so WER/terminate happen exactly as
 * before — but with a `[EXIT]` / `[TERMINATE]` record naming the thread, the
 * exception, and a module+offset stack that `cdb` can symbolise offline against
 * the deployed PDBs (no dbghelp dependency at runtime).
 *
 * POSIX builds compile the guard to a plain call; the atexit tripwire works
 * everywhere.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_config_os.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Log the calling thread's stack (module+offset per frame) under @p tag with a
 * free-form @p reason. Safe to call from an atexit handler or an exception
 * filter; allocates nothing on the heap.
 */
void
u_crash_guard_log_stack(const char *tag, const char *reason);

/*!
 * Run @p fn(@p arg) under a structured-exception guard. On an exception that
 * no inner handler claims, logs `[TERMINATE] thread '<name>' ...` plus the
 * exception address and the faulting stack, then lets the exception continue
 * to propagate exactly as if the guard were not there. On non-MSVC builds this
 * is a plain call.
 */
void *
u_crash_guard_run(const char *thread_name, void *(*fn)(void *), void *arg);

/*!
 * Mark the process as being on its orderly exit path (right before `main`
 * returns / an intentional `ExitProcess`). Suppresses the atexit tripwire.
 */
void
u_crash_guard_mark_orderly_exit(void);

/*!
 * Install the atexit tripwire: any process exit that was not preceded by
 * u_crash_guard_mark_orderly_exit() logs `[EXIT] unexpected process exit` with
 * the exiting thread's stack. Idempotent. Call *after* the file logger has been
 * initialised (atexit handlers run LIFO, so registering later than the logger's
 * own shutdown handler guarantees the record lands in the log file).
 */
void
u_crash_guard_install_exit_tripwire(void);

#ifdef __cplusplus
}
#endif
