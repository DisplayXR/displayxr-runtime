// Copyright 2026, DisplayXR.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Android Dynamic Performance Framework (ADPF) hint-session wrapper.
 * @ingroup aux_android
 *
 * Thin C wrapper over the NDK @c APerformanceHint API (API 33+). A hint session
 * tells the scheduler that a set of threads must finish their work within a target
 * duration each frame, which keeps the big cores from parking at min clock under a
 * steady, touch-free workload — the ~13 ms "wait" DVFS core-parking stall that the
 * Android OOP present path hits after #646 fixed the vsync phase (#663).
 *
 * All entry points are NULL-safe and degrade gracefully: if the device is below
 * API 33, has no PerformanceHintManager, or session creation fails,
 * android_perf_hint_session_create() returns NULL and every other call is a no-op.
 * Callers therefore never need to branch on availability.
 */

#pragma once

#include <xrt/xrt_config_os.h>

#include <stdint.h>

#ifdef XRT_OS_ANDROID

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Opaque ADPF hint-session handle. NULL means "unavailable" (see file comment).
 * @ingroup aux_android
 */
struct android_perf_hint_session;

/*!
 * Create a hint session for @p tid with an initial target work duration.
 *
 * @param tid                Kernel thread id (gettid()) of the thread doing the
 *                           per-frame work — for the OOP service this is the
 *                           "Multi Client Module" render thread.
 * @param target_duration_ns Target per-frame work duration in ns (e.g. 16.67 ms for
 *                           60 Hz). Values <= 0 fall back to 60 Hz.
 * @return A session handle, or NULL if ADPF is unavailable (always usable with the
 *         other calls — they no-op on NULL).
 * @ingroup aux_android
 */
struct android_perf_hint_session *
android_perf_hint_session_create(int32_t tid, int64_t target_duration_ns);

/*!
 * Update the session's target per-frame work duration (e.g. when the predicted
 * display period changes). No-op on a NULL session or non-positive duration.
 * @ingroup aux_android
 */
void
android_perf_hint_session_update_target(struct android_perf_hint_session *s, int64_t target_duration_ns);

/*!
 * Report the actual work duration measured for the just-finished frame. This is the
 * closed-loop signal the scheduler uses. No-op on a NULL session or non-positive
 * duration.
 * @ingroup aux_android
 */
void
android_perf_hint_session_report_actual(struct android_perf_hint_session *s, int64_t actual_duration_ns);

/*!
 * Destroy the session and free the handle. No-op on NULL.
 * @ingroup aux_android
 */
void
android_perf_hint_session_destroy(struct android_perf_hint_session *s);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_ANDROID
