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
 * @p fn is invoked inline on the calling thread (graceful fallback).
 *
 * @param fn   the callback to run on the main thread.
 * @param data opaque argument forwarded to @p fn.
 * @return XRT_SUCCESS once @p fn has run (inline or on the main thread).
 * @ingroup aux_android
 */
xrt_result_t
android_run_on_main_thread_blocking(void (*fn)(void *), void *data);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_ANDROID
