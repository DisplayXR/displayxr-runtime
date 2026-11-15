// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #206 on a present-owner route: the weave->scanout residual from the
 *         caller's present feedback (XR_DXR_weave spec v21).
 * @ingroup comp_util
 *
 * A present-owner route (xrWeaveSubmitDXR) weaves into a buffer the CALLER
 * presents, so the runtime never sees the frame reach the screen and cannot
 * measure the weave->scanout residual the way a presenting compositor does
 * (comp_vk_native_target.cpp correlates its own presentID with the driver's
 * actualPresentTime). From spec v21 the caller reports it back: "the frame you
 * returned fenceValue F for was presented at time T". This is the bookkeeping
 * that turns those reports into the prediction horizon the display processor's
 * set_predicted_scanout slot wants:
 *
 *  - a ring of the last @ref COMP_WEAVE_PF_RING submits, fenceValue -> weave
 *    start (monotonic ns, stamped where the DP's eye prediction is anchored);
 *  - on a report: residual = present time - weave start, rejected when the
 *    fenceValue is unknown (never returned, already reported, or wrapped out of
 *    the ring) or the residual is implausible (<= 0, or above
 *    @ref COMP_WEAVE_PF_MAX_RESIDUAL_NS);
 *  - the prediction is the MEDIAN of the last @ref COMP_WEAVE_PF_MEDIAN
 *    accepted residuals. This route's weave start is phase-locked to vsync, so
 *    the residual is near-constant between refresh-rate changes; the median
 *    drops the odd late frame and still follows a step (90 <-> 60 Hz) once a
 *    majority of the window is on the new value, i.e. within the window;
 *  - with no accepted report for @ref COMP_WEAVE_PF_STALE_NS the prediction is
 *    0, the slot's "unknown" (the DP then keeps its own default horizon), and
 *    the window restarts with the next report rather than mixing eras.
 *
 * Reports may arrive late, several at once, and out of order: each is resolved
 * against its own submit, and the window is in ARRIVAL order (the newest
 * information the runtime has, whichever frame it describes).
 *
 * Pure logic, no clock and no platform calls — the caller passes "now". Unit
 * tests: tests/tests_comp_weave_present_feedback.cpp.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Submits remembered for a later report. A report describes a frame a few
//! refresh periods old; 64 is > 0.5 s of submits at 120 Hz.
#define COMP_WEAVE_PF_RING 64
//! Accepted residuals the median is taken over.
#define COMP_WEAVE_PF_MEDIAN 9
//! A residual above this is a stalled or mis-correlated frame, not a latency.
#define COMP_WEAVE_PF_MAX_RESIDUAL_NS (500ULL * 1000ULL * 1000ULL)
//! No accepted report for this long => the prediction is unknown (0).
#define COMP_WEAVE_PF_STALE_NS (1000ULL * 1000ULL * 1000ULL)

//! What @ref comp_weave_pf_report did with one report.
enum comp_weave_pf_verdict
{
	COMP_WEAVE_PF_ACCEPTED = 0,
	//! fenceValue 0, never recorded, already reported, or wrapped out of the ring.
	COMP_WEAVE_PF_REJECTED_UNKNOWN,
	//! Present time not after the weave start, or residual above the ceiling.
	COMP_WEAVE_PF_REJECTED_RANGE,
};

struct comp_weave_pf
{
	//! Slot = fence_value % COMP_WEAVE_PF_RING. weave_start_ns 0 = empty/consumed.
	struct
	{
		uint64_t fence_value;
		uint64_t weave_start_ns;
	} ring[COMP_WEAVE_PF_RING];

	//! Last accepted residuals, arrival order, circular.
	uint64_t recent_ns[COMP_WEAVE_PF_MEDIAN];
	uint32_t recent_count;
	uint32_t recent_next;
	//! "now" of the last accepted report. Meaningful when recent_count > 0.
	uint64_t last_accept_ns;

	//! Lifetime totals.
	uint64_t accepted_total;
	uint64_t rejected_total;

	//! Summary window, cleared by @ref comp_weave_pf_take_stats.
	uint64_t win_accepted;
	uint64_t win_rejected;
	uint64_t win_min_ns;
	uint64_t win_max_ns;
};

//! One summary window (see @ref comp_weave_pf_take_stats).
struct comp_weave_pf_stats
{
	uint64_t accepted;
	uint64_t rejected;
	uint64_t min_ns; //!< 0 when accepted == 0
	uint64_t max_ns;
	uint64_t median_ns; //!< the current prediction; 0 = unknown
};

static inline void
comp_weave_pf_reset(struct comp_weave_pf *pf)
{
	if (pf != NULL) {
		memset(pf, 0, sizeof(*pf));
	}
}

/*!
 * Remember when the weave of the submit that returned @p fence_value started.
 * @p fence_value 0 and @p weave_start_ns 0 are not recordable (both mean
 * "none" here) and are ignored.
 */
static inline void
comp_weave_pf_record(struct comp_weave_pf *pf, uint64_t fence_value, uint64_t weave_start_ns)
{
	if (pf == NULL || fence_value == 0 || weave_start_ns == 0) {
		return;
	}
	const uint32_t slot = (uint32_t)(fence_value % COMP_WEAVE_PF_RING);
	pf->ring[slot].fence_value = fence_value;
	pf->ring[slot].weave_start_ns = weave_start_ns;
}

static inline void
comp_weave_pf_reject(struct comp_weave_pf *pf)
{
	pf->rejected_total++;
	pf->win_rejected++;
}

/*!
 * One report: the frame that returned @p fence_value was presented at
 * @p actual_present_ns (same monotonic clock as the recorded weave start).
 * @p now_ns is the time the report arrived, for staleness.
 */
static inline enum comp_weave_pf_verdict
comp_weave_pf_report(struct comp_weave_pf *pf, uint64_t fence_value, int64_t actual_present_ns, uint64_t now_ns)
{
	if (pf == NULL) {
		return COMP_WEAVE_PF_REJECTED_UNKNOWN;
	}
	const uint32_t slot = (uint32_t)(fence_value % COMP_WEAVE_PF_RING);
	if (fence_value == 0 || pf->ring[slot].fence_value != fence_value || pf->ring[slot].weave_start_ns == 0) {
		comp_weave_pf_reject(pf);
		return COMP_WEAVE_PF_REJECTED_UNKNOWN;
	}
	const uint64_t started = pf->ring[slot].weave_start_ns;
	// One report per frame, whatever it says: a repeat is then "unknown".
	pf->ring[slot].weave_start_ns = 0;

	if (actual_present_ns <= 0 || (uint64_t)actual_present_ns <= started ||
	    (uint64_t)actual_present_ns - started > COMP_WEAVE_PF_MAX_RESIDUAL_NS) {
		comp_weave_pf_reject(pf);
		return COMP_WEAVE_PF_REJECTED_RANGE;
	}
	const uint64_t residual = (uint64_t)actual_present_ns - started;

	// Resuming after a silence: what the window holds may describe another
	// refresh rate. Start over rather than let it outvote the new samples.
	if (pf->recent_count > 0 && now_ns > pf->last_accept_ns &&
	    now_ns - pf->last_accept_ns > COMP_WEAVE_PF_STALE_NS) {
		pf->recent_count = 0;
		pf->recent_next = 0;
	}
	pf->recent_ns[pf->recent_next] = residual;
	pf->recent_next = (pf->recent_next + 1) % COMP_WEAVE_PF_MEDIAN;
	if (pf->recent_count < COMP_WEAVE_PF_MEDIAN) {
		pf->recent_count++;
	}
	pf->last_accept_ns = now_ns;

	pf->accepted_total++;
	if (pf->win_accepted == 0 || residual < pf->win_min_ns) {
		pf->win_min_ns = residual;
	}
	if (residual > pf->win_max_ns) {
		pf->win_max_ns = residual;
	}
	pf->win_accepted++;
	return COMP_WEAVE_PF_ACCEPTED;
}

/*!
 * The predicted weave->scanout interval for a weave starting at @p now_ns:
 * the median of the accepted window (the upper middle while it holds an even
 * count), or 0 = unknown when nothing was accepted yet or the last accepted
 * report is older than @ref COMP_WEAVE_PF_STALE_NS.
 */
static inline uint64_t
comp_weave_pf_predict(const struct comp_weave_pf *pf, uint64_t now_ns)
{
	if (pf == NULL || pf->recent_count == 0) {
		return 0;
	}
	if (now_ns > pf->last_accept_ns && now_ns - pf->last_accept_ns > COMP_WEAVE_PF_STALE_NS) {
		return 0;
	}
	uint64_t sorted[COMP_WEAVE_PF_MEDIAN];
	const uint32_t n = pf->recent_count;
	for (uint32_t i = 0; i < n; i++) { // insertion sort, n <= 9
		uint64_t v = pf->recent_ns[i];
		uint32_t j = i;
		while (j > 0 && sorted[j - 1] > v) {
			sorted[j] = sorted[j - 1];
			j--;
		}
		sorted[j] = v;
	}
	return sorted[n / 2];
}

//! Take and clear the summary window. @p now_ns as @ref comp_weave_pf_predict.
static inline struct comp_weave_pf_stats
comp_weave_pf_take_stats(struct comp_weave_pf *pf, uint64_t now_ns)
{
	struct comp_weave_pf_stats st = {0, 0, 0, 0, 0};
	if (pf == NULL) {
		return st;
	}
	st.accepted = pf->win_accepted;
	st.rejected = pf->win_rejected;
	st.min_ns = pf->win_accepted != 0 ? pf->win_min_ns : 0;
	st.max_ns = pf->win_max_ns;
	st.median_ns = comp_weave_pf_predict(pf, now_ns);
	pf->win_accepted = 0;
	pf->win_rejected = 0;
	pf->win_min_ns = 0;
	pf->win_max_ns = 0;
	return st;
}

#ifdef __cplusplus
}
#endif
