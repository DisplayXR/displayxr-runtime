// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  FIFO present-queue drain for the grid late-weave tier (#2629):
 *         skip one vblank when the queue is provably deep, check that it
 *         helped, learn the pipeline floor when it did not, back off.
 * @ingroup comp_util
 *
 * ## Why
 *
 * The grid tier (Android, VK_GOOGLE_display_timing) sleeps toward the next
 * predicted vblank but never waits for a previous present, so nothing bounds
 * the FIFO queue depth except the image count. When the panel drops to a
 * slower rate (90 -> 60 Hz) the period is re-read only every ~500 ms; until
 * then the tier weaves at the old, faster cadence and fills the queue. Once
 * the cadence matches, producer and consumer run at the same rate and the
 * queue stays exactly as full as it got: measured weave->present ~4 periods
 * at 60 Hz against ~2.2 at 90 Hz on the same device.
 *
 * Producing at the display rate can never shrink the queue. Only producing
 * one frame fewer than the display consumes can: a DRAIN is one extra
 * refresh period of sleep before one weave, which lowers the queue by one
 * slot if there was a slot to lower.
 *
 * ## Triggers (each arms one request; the caller takes it before a weave)
 *
 *  - RATE_DROP: the period got more than 20 % longer. The fill above is
 *    certain to have happened, so no evidence is waited for.
 *  - RESIDUAL: the measured weave->scanout residual sat above a threshold for
 *    @ref COMP_QD_TRIGGER_SAMPLES consecutive samples. The threshold is the
 *    learned floor + @ref COMP_QD_FLOOR_MARGIN, or
 *    @ref COMP_QD_DEFAULT_THRESHOLD periods while no floor is known.
 *  - ACQUIRE_BLOCK: acquire blocked for more than a quarter period
 *    @ref COMP_QD_TRIGGER_SAMPLES times in a row. On a FIFO swapchain that
 *    means the queue is full. A GPU-bound app's acquire blocks naturally, so
 *    once a floor is known this trigger also needs the residual above the
 *    floor threshold — otherwise every backoff expiry would cost such an app
 *    a skipped frame for nothing.
 *
 * ## Verdict
 *
 * The residual feed is several presents stale, so a drain is judged on the
 * first sample whose present sequence is at or after the drained present
 * (@ref comp_queue_drain::settle_seq); everything older predates it and is
 * ignored. Residual down by >= half a period: DRAINED, and the next trigger
 * may fire at once, so a deep queue drains one slot per settle. Otherwise
 * NOT_QUEUE: what we were at is the pipeline's floor, not queueing; it is
 * learned (threshold = floor + margin) and the machine backs off 5 s,
 * doubling to 60 s. The floor is forgotten on any period change.
 *
 * Guards: a drain with no verdict sample for 2 s returns to idle without a
 * verdict (a stalled feed cannot wedge it), and at most
 * @ref COMP_QD_MAX_SUCCESS_RUN drains in a row may "succeed" without one
 * residual sample at or below the threshold in between (a mis-correlated feed
 * would otherwise skip a vblank per settle forever).
 *
 * Pure logic, no clocks or graphics types: unit-tested on the host
 * (tests/tests_comp_queue_drain.cpp).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A period this much longer than the previous one is a rate drop.
#define COMP_QD_RATE_DROP_RATIO 1.2
//! Residual threshold in periods while no floor is known.
#define COMP_QD_DEFAULT_THRESHOLD 2.5
//! Threshold above a learned floor, in periods.
#define COMP_QD_FLOOR_MARGIN 0.75
//! A drain worked when the residual fell by at least this many periods.
#define COMP_QD_SUCCESS_DROP 0.5
//! Consecutive over-threshold samples (or blocking acquires) that arm a drain.
#define COMP_QD_TRIGGER_SAMPLES 3
//! An acquire longer than this many periods blocked on a full queue.
#define COMP_QD_ACQUIRE_BLOCK 0.25
//! "Successful" drains allowed in a row without a sample at/below threshold.
#define COMP_QD_MAX_SUCCESS_RUN 8

#define COMP_QD_BACKOFF_BASE_NS (5ULL * 1000ULL * 1000ULL * 1000ULL)
#define COMP_QD_BACKOFF_MAX_NS (60ULL * 1000ULL * 1000ULL * 1000ULL)
//! A drain without a verdict sample for this long returns to idle.
#define COMP_QD_RESULT_TIMEOUT_NS (2ULL * 1000ULL * 1000ULL * 1000ULL)

enum comp_queue_drain_state
{
	COMP_QD_IDLE = 0,
	COMP_QD_WAIT_RESULT, //!< a drain was taken; waiting for its first sample
	COMP_QD_BACKOFF,     //!< a drain did not help (or the run cap hit)
};

enum comp_queue_drain_reason
{
	COMP_QD_REASON_NONE = 0,
	COMP_QD_REASON_RATE_DROP,
	COMP_QD_REASON_RESIDUAL,
	COMP_QD_REASON_ACQUIRE_BLOCK,
};

enum comp_queue_drain_verdict
{
	COMP_QD_VERDICT_NONE = 0,
	COMP_QD_VERDICT_DRAINED,   //!< residual fell: it was queue
	COMP_QD_VERDICT_LENIENT,   //!< no pre-drain residual to judge against: counted as drained
	COMP_QD_VERDICT_NOT_QUEUE, //!< residual did not fall: floor learned, backing off
	COMP_QD_VERDICT_CAPPED,    //!< too many drains in a row: backing off
};

struct comp_queue_drain
{
	enum comp_queue_drain_state state;

	//! A request is armed; @ref comp_queue_drain_take hands it out.
	bool pending;
	enum comp_queue_drain_reason pending_reason;

	//! Learned pipeline floor in periods; 0 = unknown.
	double floor_periods;

	//! Samples for presents before this sequence predate the last drain.
	uint64_t settle_seq;
	//! Residual when the drain was taken; 0 = judge leniently.
	uint64_t pre_drain_residual_ns;
	//! When the drain was taken (for the no-sample timeout).
	uint64_t wait_start_ns;

	uint64_t backoff_until_ns;
	//! The backoff the next NOT_QUEUE/CAPPED verdict applies.
	uint64_t backoff_ns;
	//! The backoff the last verdict applied (for the caller's log line).
	uint64_t applied_backoff_ns;

	//! Consecutive valid samples above the threshold (IDLE only).
	uint32_t over_count;
	//! Consecutive acquires that blocked.
	uint32_t block_count;
	//! Drains judged DRAINED/LENIENT since the last sample at/below threshold.
	uint32_t success_run;

	//! Last valid residual sample and the period it was read against.
	uint64_t last_residual_ns;
	uint64_t last_period_ns;
	//! No residual sample since the last period change (or reset).
	bool no_sample_since_change;

	//! Totals, for diagnostics.
	uint32_t drains_taken;
	uint32_t timeouts;
};

static inline void
comp_queue_drain_reset(struct comp_queue_drain *d)
{
	if (d == NULL) {
		return;
	}
	memset(d, 0, sizeof(*d));
	d->state = COMP_QD_IDLE;
	d->backoff_ns = COMP_QD_BACKOFF_BASE_NS;
	d->no_sample_since_change = true;
}

//! The residual threshold in periods.
static inline double
comp_queue_drain_threshold(const struct comp_queue_drain *d)
{
	return d->floor_periods > 0.0 ? d->floor_periods + COMP_QD_FLOOR_MARGIN : COMP_QD_DEFAULT_THRESHOLD;
}

static inline void
comp_queue_drain_arm_(struct comp_queue_drain *d, enum comp_queue_drain_reason reason)
{
	if (!d->pending || reason == COMP_QD_REASON_RATE_DROP) {
		d->pending_reason = reason;
	}
	d->pending = true;
}

static inline void
comp_queue_drain_enter_backoff_(struct comp_queue_drain *d, uint64_t now_ns)
{
	d->state = COMP_QD_BACKOFF;
	d->applied_backoff_ns = d->backoff_ns;
	d->backoff_until_ns = now_ns + d->backoff_ns;
	d->backoff_ns = d->backoff_ns * 2 > COMP_QD_BACKOFF_MAX_NS ? COMP_QD_BACKOFF_MAX_NS : d->backoff_ns * 2;
	d->pending = false;
	d->pending_reason = COMP_QD_REASON_NONE;
	d->over_count = 0;
	d->block_count = 0;
	d->success_run = 0;
}

//! Time-driven transitions: backoff expiry, the no-sample timeout.
static inline void
comp_queue_drain_tick_(struct comp_queue_drain *d, uint64_t now_ns)
{
	if (d->state == COMP_QD_BACKOFF && now_ns >= d->backoff_until_ns) {
		d->state = COMP_QD_IDLE; // the learned floor stays
		d->over_count = 0;
		d->block_count = 0;
	} else if (d->state == COMP_QD_WAIT_RESULT && now_ns - d->wait_start_ns > COMP_QD_RESULT_TIMEOUT_NS) {
		d->state = COMP_QD_IDLE; // no verdict, no floor change
		d->timeouts++;
		d->over_count = 0;
		d->block_count = 0;
	}
}

/*!
 * The panel period changed (not the initial set). Forgets the floor and any
 * backoff; a longer period arms a drain at once.
 */
static inline void
comp_queue_drain_on_period_change(struct comp_queue_drain *d, uint64_t old_ns, uint64_t new_ns, uint64_t now_ns)
{
	if (d == NULL) {
		return;
	}
	(void)now_ns;
	d->floor_periods = 0.0;
	d->state = COMP_QD_IDLE; // a pending verdict would compare across rates
	d->backoff_ns = COMP_QD_BACKOFF_BASE_NS;
	d->backoff_until_ns = 0;
	d->over_count = 0;
	d->block_count = 0;
	d->success_run = 0;
	d->no_sample_since_change = true;
	d->pending = false;
	d->pending_reason = COMP_QD_REASON_NONE;
	if (old_ns != 0 && new_ns != 0 && (double)new_ns > (double)old_ns * COMP_QD_RATE_DROP_RATIO) {
		comp_queue_drain_arm_(d, COMP_QD_REASON_RATE_DROP);
	}
}

/*!
 * One weave->scanout residual sample for the present with sequence @p seq.
 *
 * @return the verdict this sample produced on a pending drain, if any.
 */
static inline enum comp_queue_drain_verdict
comp_queue_drain_on_residual(
    struct comp_queue_drain *d, uint64_t seq, uint64_t residual_ns, uint64_t period_ns, uint64_t now_ns)
{
	if (d == NULL) {
		return COMP_QD_VERDICT_NONE;
	}
	comp_queue_drain_tick_(d, now_ns);
	if (period_ns == 0 || residual_ns == 0 || seq < d->settle_seq) {
		return COMP_QD_VERDICT_NONE; // stale: predates the last drain
	}
	d->last_residual_ns = residual_ns;
	d->last_period_ns = period_ns;
	d->no_sample_since_change = false;
	const double periods = (double)residual_ns / (double)period_ns;

	if (d->state == COMP_QD_WAIT_RESULT) {
		enum comp_queue_drain_verdict v;
		if (d->pre_drain_residual_ns == 0) {
			v = COMP_QD_VERDICT_LENIENT;
		} else if ((double)d->pre_drain_residual_ns - (double)residual_ns >=
		           COMP_QD_SUCCESS_DROP * (double)period_ns) {
			v = COMP_QD_VERDICT_DRAINED;
		} else {
			// What we were at is the pipeline's floor, not queue.
			d->floor_periods = periods;
			comp_queue_drain_enter_backoff_(d, now_ns);
			return COMP_QD_VERDICT_NOT_QUEUE;
		}
		d->state = COMP_QD_IDLE;
		d->backoff_ns = COMP_QD_BACKOFF_BASE_NS;
		d->over_count = 0;
		d->block_count = 0;
		d->success_run++;
		if (d->success_run >= COMP_QD_MAX_SUCCESS_RUN) {
			comp_queue_drain_enter_backoff_(d, now_ns);
			return COMP_QD_VERDICT_CAPPED;
		}
		return v;
	}

	if (d->state != COMP_QD_IDLE) {
		return COMP_QD_VERDICT_NONE;
	}
	if (periods > comp_queue_drain_threshold(d)) {
		if (++d->over_count >= COMP_QD_TRIGGER_SAMPLES) {
			comp_queue_drain_arm_(d, COMP_QD_REASON_RESIDUAL);
		}
	} else {
		d->over_count = 0;
		d->success_run = 0;
	}
	return COMP_QD_VERDICT_NONE;
}

//! How long one acquire blocked (vkAcquireNextImageKHR duration).
static inline void
comp_queue_drain_on_acquire_block(struct comp_queue_drain *d, uint64_t block_ns, uint64_t period_ns, uint64_t now_ns)
{
	if (d == NULL) {
		return;
	}
	comp_queue_drain_tick_(d, now_ns);
	if (d->state != COMP_QD_IDLE || period_ns == 0) {
		return;
	}
	if ((double)block_ns <= COMP_QD_ACQUIRE_BLOCK * (double)period_ns) {
		d->block_count = 0;
		return;
	}
	if (++d->block_count < COMP_QD_TRIGGER_SAMPLES) {
		return;
	}
	if (d->floor_periods > 0.0) {
		// A GPU-bound app's acquire blocks by nature: with a floor known,
		// only a residual above it says the block is queue.
		if (d->last_period_ns == 0 ||
		    (double)d->last_residual_ns / (double)d->last_period_ns <= comp_queue_drain_threshold(d)) {
			return;
		}
	}
	comp_queue_drain_arm_(d, COMP_QD_REASON_ACQUIRE_BLOCK);
}

/*!
 * Take an armed request, just before a weave. True exactly once per request
 * and only when idle: the caller then sleeps one refresh period.
 *
 * @param next_seq the sequence the upcoming present will carry.
 * @param last_residual_ns the residual currently published (0 = none yet).
 */
static inline bool
comp_queue_drain_take(struct comp_queue_drain *d,
                      uint64_t next_seq,
                      uint64_t last_residual_ns,
                      uint64_t now_ns,
                      enum comp_queue_drain_reason *out_reason)
{
	if (d == NULL) {
		return false;
	}
	comp_queue_drain_tick_(d, now_ns);
	if (d->state != COMP_QD_IDLE || !d->pending) {
		return false;
	}
	const enum comp_queue_drain_reason reason = d->pending_reason;
	d->pending = false;
	d->pending_reason = COMP_QD_REASON_NONE;
	uint64_t pre = last_residual_ns;
	if (reason == COMP_QD_REASON_RATE_DROP && d->no_sample_since_change) {
		// The published residual was measured at the old rate: comparing
		// against it could call a real drain a failure and learn a floor
		// that is all queue. Judge leniently instead.
		pre = 0;
	}
	if (pre == 0 && reason != COMP_QD_REASON_RATE_DROP) {
		return false; // nothing to judge against; only a rate drop is certain
	}
	d->pre_drain_residual_ns = pre;
	d->settle_seq = next_seq;
	d->wait_start_ns = now_ns;
	d->state = COMP_QD_WAIT_RESULT;
	d->over_count = 0;
	d->block_count = 0;
	d->drains_taken++;
	if (out_reason != NULL) {
		*out_reason = reason;
	}
	return true;
}

static inline const char *
comp_queue_drain_reason_str(enum comp_queue_drain_reason r)
{
	switch (r) {
	case COMP_QD_REASON_RATE_DROP: return "rate drop";
	case COMP_QD_REASON_RESIDUAL: return "residual";
	case COMP_QD_REASON_ACQUIRE_BLOCK: return "acquire block";
	default: return "none";
	}
}

#ifdef __cplusplus
}
#endif
