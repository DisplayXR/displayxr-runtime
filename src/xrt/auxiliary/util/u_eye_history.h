// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Short timestamped ring of viewer eye-position sets, sampled at a
 *         target time (ADR-054 §4.1 / N6 Part 1, #2144).
 * @ingroup aux_util
 *
 * ## Why this exists
 *
 * Every eye consumer in the trunk used to ask the display processor for its
 * "predicted" eyes — a latest value with a horizon only the vendor knew. The
 * N6 eye contract makes the HORIZON the caller's: `xrLocateViews` wants the
 * eyes at `displayTime`, a compositor wants them at the weave / scan-out
 * instant, a status page wants them now. The DP may answer a target time
 * directly (`get_eye_positions_at`, the ADR-020 append-only slot on every DP
 * vtable); when it cannot — an older plug-in, a provider that only pushes
 * samples — the trunk keeps this ring per eye SOURCE, fed by whoever already
 * queries that source once per frame, and samples it at the target time the
 * way @ref m_relation_history samples a pose: exact, interpolated between the
 * two neighbouring samples, or extrapolated a bounded distance past the
 * latest.
 *
 * ## Sampling rules (pinned by tests/tests_u_eye_history.cpp)
 *
 * - **Exact** — a sample at `target_ns` is returned as is.
 * - **Interpolated** — between two samples: positions lerp per eye;
 *   `is_tracking` is the AND of the neighbours (a tracking-lost edge is never
 *   interpolated away — the untracked side wins); when the neighbours'
 *   `count` differ (a mode switch) the NEWER sample is returned unchanged,
 *   there is no meaningful lerp across a layout change.
 * - **Extrapolated** — past the latest sample: the last velocity (the newest
 *   two samples, both tracking, same count) carries the positions forward by
 *   at most @ref U_EYE_HISTORY_MAX_EXTRAPOLATE_NS; a farther target holds the
 *   capped prediction (continuous, never a jump back to the latest sample).
 *   Without a usable velocity the latest sample is returned.
 * - **Stale** — the latest sample is older than @ref U_EYE_HISTORY_MAX_AGE_NS
 *   at the target: the last-known positions are returned (the eye-tracking
 *   contract never hands out zeros — eye-tracking-modes.md "Tracking loss")
 *   with `is_tracking = false` and `valid` kept.
 * - **Before the oldest** — the oldest sample is returned unchanged (no
 *   reverse extrapolation; a target in the past is a diagnostic, not a frame).
 *
 * `out->timestamp_ns` is the time the answer is for (the target) on an
 * exact / interpolated / extrapolated hit, and the latest sample's own time on
 * a stale or clamped answer — so a consumer can tell how old its data is.
 *
 * Thread safe (one mutex; writers and readers are different threads: the
 * render thread pushes, an IPC / app thread samples). Only VALID samples are
 * kept: a `valid == false` answer carries nothing to interpolate and is
 * rejected by @ref u_eye_history_push, as is a sample older than the latest
 * (a same-time push replaces the latest).
 */

#pragma once

#include "os/os_threading.h"
#include "xrt/xrt_compiler.h"
#include "xrt/xrt_display_metrics.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Ring capacity: ~half a second of 60 Hz samples, more than the max age needs.
#define U_EYE_HISTORY_SIZE 32

//! Furthest a sample is carried forward with its velocity (50 ms, ~3 frames).
#define U_EYE_HISTORY_MAX_EXTRAPOLATE_NS ((int64_t)50 * 1000 * 1000)

//! Latest-sample age at which the answer is "last known, not tracking" (500 ms).
#define U_EYE_HISTORY_MAX_AGE_NS ((int64_t)500 * 1000 * 1000)

/*!
 * How @ref u_eye_history_get produced its answer.
 * @relates u_eye_history
 */
enum u_eye_history_result
{
	U_EYE_HISTORY_RESULT_EMPTY = 0,     //!< No sample: @p out untouched, nothing to report.
	U_EYE_HISTORY_RESULT_EXACT,         //!< A sample at the target time.
	U_EYE_HISTORY_RESULT_INTERPOLATED,  //!< Between two samples (or the newer one across a count change).
	U_EYE_HISTORY_RESULT_EXTRAPOLATED,  //!< Past the latest, carried by the last velocity (capped).
	U_EYE_HISTORY_RESULT_LATEST,        //!< Past the latest, no usable velocity: the latest sample.
	U_EYE_HISTORY_RESULT_STALE,         //!< Latest older than the max age: last known, is_tracking=false.
	U_EYE_HISTORY_RESULT_BEFORE_OLDEST, //!< Target before the oldest sample: the oldest sample.
};

/*!
 * One kept sample.
 * @relates u_eye_history
 */
struct u_eye_history_entry
{
	struct xrt_eye_positions eyes;
	int64_t ts_ns; //!< When the sample was taken (monotonic, os_monotonic_get_ns() clock).
};

/*!
 * The ring. @ref u_eye_history_init before use, @ref u_eye_history_destroy
 * after; zero-init is not enough (the mutex).
 * @ingroup aux_util
 */
struct u_eye_history
{
	struct os_mutex mutex;
	struct u_eye_history_entry ring[U_EYE_HISTORY_SIZE];
	uint32_t count; //!< Samples kept (≤ U_EYE_HISTORY_SIZE).
	uint32_t head;  //!< Index of the NEXT write.
};

/*!
 * Initialise an empty ring.
 * @public @memberof u_eye_history
 */
void
u_eye_history_init(struct u_eye_history *h);

/*!
 * Release the ring's mutex. The ring is unusable afterwards.
 * @public @memberof u_eye_history
 */
void
u_eye_history_destroy(struct u_eye_history *h);

/*!
 * Drop every sample (a source change, a tracking reset).
 * @public @memberof u_eye_history
 */
void
u_eye_history_clear(struct u_eye_history *h);

/*!
 * Keep a sample taken at @p ts_ns. The oldest sample is dropped when full.
 *
 * @return false when the sample was NOT kept: `eyes->valid == false`, or
 *         @p ts_ns older than the latest kept sample. A push at the latest
 *         sample's own time replaces it and returns true.
 * @public @memberof u_eye_history
 */
bool
u_eye_history_push(struct u_eye_history *h, const struct xrt_eye_positions *eyes, int64_t ts_ns);

/*!
 * The eyes at @p target_ns, per the sampling rules in the file comment.
 * Read-only on the ring (the mutex is taken). @p out is untouched on
 * @ref U_EYE_HISTORY_RESULT_EMPTY.
 * @public @memberof u_eye_history
 */
enum u_eye_history_result
u_eye_history_get(struct u_eye_history *h, int64_t target_ns, struct xrt_eye_positions *out);

/*!
 * As @ref u_eye_history_get, but NEVER predicts: a target past the newest
 * sample returns that sample (@ref U_EYE_HISTORY_RESULT_LATEST, or STALE
 * past the max age), never an extrapolation. Interpolation between samples
 * and the before-oldest clamp are unchanged. This is what the compositors use
 * when a DP's target-time slot fails — its ring holds already-predicted
 * answers, so predicting them again would double the vendor's horizon.
 * @public @memberof u_eye_history
 */
enum u_eye_history_result
u_eye_history_get_clamped(struct u_eye_history *h, int64_t target_ns, struct xrt_eye_positions *out);

/*!
 * The newest kept sample, if any.
 * @param[out] out_ts_ns  Its time (may be NULL).
 * @param[out] out        Its eyes (may be NULL).
 * @return false when the ring is empty.
 * @public @memberof u_eye_history
 */
bool
u_eye_history_get_latest(struct u_eye_history *h, int64_t *out_ts_ns, struct xrt_eye_positions *out);

/*!
 * Samples kept.
 * @public @memberof u_eye_history
 */
uint32_t
u_eye_history_get_size(struct u_eye_history *h);

/*!
 * Human-readable name for a result, for logs.
 * @relates u_eye_history
 */
const char *
u_eye_history_result_str(enum u_eye_history_result r);

#ifdef __cplusplus
}
#endif
