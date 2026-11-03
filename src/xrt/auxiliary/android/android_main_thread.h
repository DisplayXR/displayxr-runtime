// Copyright 2026, DisplayXR.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Run a native callback on the Android service main thread (main Looper).
 * @ingroup aux_android
 *
 * Some vendor display-processor plug-ins (notably the Leia CNSDK) require their
 * async init to be *kicked off* from a thread that owns a pumped Android Looper —
 * `leia_core_init_async()` posts to the calling thread's Looper, and on a bare
 * worker thread (no Looper) the init immediately tears down and hangs (#510 M2).
 *
 * The out-of-process service creates the per-session display processor on the
 * comp_multi render worker thread, which has no Looper. This helper marshals a
 * callback onto the service main thread (which owns the Java main Looper) using an
 * NDK self-pipe + `ALooper_addFd`: the Java `MessageQueue.next()` services the fd
 * callback (same underlying `android::Looper`), so the callback runs on the real
 * main thread (tid==pid). No JNI is performed by the transport — only the marshaled
 * callback (already on the JNI-attached main thread) touches JNI/CNSDK. This also
 * avoids the multi-classloader JNI-registration unreliability seen in #507.
 */

#pragma once

#include <xrt/xrt_config_os.h>
#include <xrt/xrt_results.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef XRT_OS_ANDROID

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Initialize main-thread dispatch. MUST be called once on the Android main thread
 * (e.g. from the service JNI `nativeStartServer`, which runs on the main thread).
 * Captures the main Looper and registers a self-pipe fd callback on it. Idempotent.
 * @ingroup aux_android
 */
void
android_main_thread_dispatch_init(void);

/*!
 * Run @p fn (@p data) on the Android main thread and block the caller until it
 * completes.
 *
 * If dispatch has not been initialized, or the caller is already the main thread,
 * @p fn is invoked inline on the calling thread (graceful fallback). Code that
 * needs a Looper in every configuration wants @ref android_run_on_looper_thread_timeout.
 *
 * @param fn   the callback to run on the main thread.
 * @param data opaque argument forwarded to @p fn.
 * @return XRT_SUCCESS once @p fn has run (inline or on the main thread).
 * @ingroup aux_android
 */
xrt_result_t
android_run_on_main_thread_blocking(void (*fn)(void *), void *data);

/*!
 * Like @ref android_run_on_main_thread_blocking, but gives up after
 * @p timeout_ns (#1007) if the main thread has not STARTED the callback by then
 * (a busy or wedged main Looper). An abandoned callback is never run later.
 * A callback that has already started cannot be abandoned — it is using
 * @p data — so the caller keeps waiting for it (with a WARN per period).
 *
 * @param timeout_ns 0 or negative = wait forever (same as the blocking form).
 * @return XRT_SUCCESS once @p fn has run, XRT_TIMEOUT if it was abandoned.
 * @ingroup aux_android
 */
xrt_result_t
android_run_on_main_thread_timeout(void (*fn)(void *), void *data, int64_t timeout_ns);

/*!
 * Whether @ref android_main_thread_dispatch_init succeeded in this process
 * (true in the out-of-process service, false in-process).
 * @ingroup aux_android
 */
bool
android_main_thread_dispatch_is_initialized(void);

/*!
 * Run @p fn (@p data) on a thread that owns a pumped ALooper, and wait for it.
 * Use this for anything (notably a vendor display-processor factory) whose
 * init posts to the calling thread's Looper (#510 M2, #2490).
 *
 * Where it runs, in order:
 *  1. main-thread dispatch initialized (service): on the Android main thread,
 *     exactly like @ref android_run_on_main_thread_timeout;
 *  2. the caller already owns an ALooper (native_app_glue, a UI thread):
 *     inline;
 *  3. otherwise (a bare render thread, the compositor worker): on a
 *     runtime-owned looper thread, started on first use, attached to the VM
 *     when one is published, and pumped for the life of the process.
 *
 * @param timeout_ns same contract as @ref android_run_on_main_thread_timeout
 *                   (0 or negative = wait forever).
 * @return XRT_SUCCESS once @p fn has run; XRT_TIMEOUT if it was abandoned;
 *         XRT_ERROR_THREADING_INIT_FAILURE if no looper thread could be made
 *         (@p fn has not run).
 * @ingroup aux_android
 */
xrt_result_t
android_run_on_looper_thread_timeout(void (*fn)(void *), void *data, int64_t timeout_ns);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_ANDROID
