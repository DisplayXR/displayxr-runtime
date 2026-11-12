// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  AIM vs THROUGHPUT pacing for the grid late-weave tier (#2629), with
 *         a measured verdict — and the robust weave budget it aims with.
 * @ingroup comp_util
 *
 * ## Why two modes
 *
 * A LATE weave (its next vblank is out of reach) can sleep toward the vblank
 * after it (AIM) or start at once (THROUGHPUT). Measured on a phone-class
 * device, round 2 vs round 3 of #2629:
 *
 *  - an app that fits the period (model viewer, 60 Hz): AIM 19 ms at 58.6
 *    weaves/s; THROUGHPUT made most weaves early, ~2 periods with an empty
 *    queue, the residual alternating 20/36 ms, and fed a drain loop;
 *  - an app whose render + weave exceeds the period (same app, 90 Hz): AIM
 *    65.7 weaves/s (the late sleep blocks its submit thread until the next
 *    frame misses too); THROUGHPUT 80.9.
 *
 * Neither wins everywhere, so AIM is the default and THROUGHPUT has to earn
 * its place. Over ~1 s windows the controller counts weaves made against
 * vblanks elapsed and how many weaves took the late sleep:
 *
 *  - AIM -> THROUGHPUT probe when the rate is under @ref COMP_PACE_LOW_RATE
 *    AND late sleeps are over @ref COMP_PACE_LATE_SHARE of the weaves (the
 *    sleep is the plausible cause; a slow app that is never late is left
 *    alone);
 *  - the probe's first full window must beat the AIM rate that triggered it
 *    by @ref COMP_PACE_PROVE_GAIN, or it returns to AIM and the next probe
 *    waits 5 s, doubling to 60 s (a content-limited app — a 45 fps video —
 *    does not sit in the worse-latency mode for nothing);
 *  - a proven THROUGHPUT stays until the refresh period changes, which
 *    returns to AIM with the backoff reset.
 *
 * ## The budget
 *
 * The aim needs a weave-cost estimate. The decaying high-water it used
 * (rise to any sample, decay 2 %/frame) let one long frame triple it for a
 * second, turning reachable vblanks into "late" ones (measured 3.45 ->
 * 12.80 ms within one 60 Hz phase). @ref comp_weave_budget is the 90th
 * percentile of the last @ref COMP_WEAVE_BUDGET_N weave costs instead: one
 * long frame, or three, do not move it; a sustained rise does.
 *
 * Pure logic, no clocks or graphics types: unit-tested on the host
 * (tests/tests_comp_grid_pace_mode.cpp).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Window length.
#define COMP_PACE_WINDOW_NS (1000ULL * 1000ULL * 1000ULL)
//! A gap this long between weaves (a pause) discards the window.
#define COMP_PACE_GAP_NS (250ULL * 1000ULL * 1000ULL)
//! AIM costs throughput when weaves/vblanks is under this...
#define COMP_PACE_LOW_RATE 0.9
//! ...and over this share of the weaves took the late sleep.
#define COMP_PACE_LATE_SHARE 0.10
//! THROUGHPUT must beat the AIM rate by this factor to stay.
#define COMP_PACE_PROVE_GAIN 1.05
#define COMP_PACE_BACKOFF_BASE_NS (5ULL * 1000ULL * 1000ULL * 1000ULL)
#define COMP_PACE_BACKOFF_MAX_NS (60ULL * 1000ULL * 1000ULL * 1000ULL)

enum comp_grid_pace_mode
{
	COMP_PACE_AIM = 0,
	COMP_PACE_THROUGHPUT,
};

enum comp_grid_pace_event
{
	COMP_PACE_EV_NONE = 0,
	COMP_PACE_EV_PROBE,     //!< AIM -> THROUGHPUT (probe)
	COMP_PACE_EV_PROVEN,    //!< the probe beat AIM: staying in THROUGHPUT
	COMP_PACE_EV_REJECTED,  //!< the probe did not beat AIM: back to AIM, backoff
	COMP_PACE_EV_RESET_AIM, //!< period change: back to AIM
};

struct comp_grid_pace
{
	enum comp_grid_pace_mode mode;
	bool probing; //!< in THROUGHPUT, first window not judged yet

	// Current window.
	uint64_t win_start_ns;
	uint64_t win_last_ns;
	uint32_t win_weaves;
	uint32_t win_late;

	//! Last full window's weaves/vblanks per mode (for the trace).
	double aim_rate;
	double thr_rate;
	//! The AIM rate the running probe has to beat.
	double probe_bar;
	double last_late_share;

	uint64_t backoff_ns;
	uint64_t backoff_until_ns;
	uint32_t probes;
};

static inline void
comp_grid_pace_reset(struct comp_grid_pace *p)
{
	if (p == NULL) {
		return;
	}
	memset(p, 0, sizeof(*p));
	p->mode = COMP_PACE_AIM;
	p->backoff_ns = COMP_PACE_BACKOFF_BASE_NS;
}

//! The refresh period changed: back to AIM, backoff reset.
static inline enum comp_grid_pace_event
comp_grid_pace_on_period_change(struct comp_grid_pace *p)
{
	if (p == NULL) {
		return COMP_PACE_EV_NONE;
	}
	const bool was_thr = p->mode == COMP_PACE_THROUGHPUT;
	p->mode = COMP_PACE_AIM;
	p->probing = false;
	p->win_weaves = 0;
	p->win_late = 0;
	p->win_start_ns = 0;
	p->backoff_ns = COMP_PACE_BACKOFF_BASE_NS;
	p->backoff_until_ns = 0;
	return was_thr ? COMP_PACE_EV_RESET_AIM : COMP_PACE_EV_NONE;
}

/*!
 * One weave. @p late: its next vblank was out of reach (AIM: it slept toward
 * the one after; THROUGHPUT: it wove at once).
 *
 * @return a mode change, if this weave closed a window that decided one.
 */
static inline enum comp_grid_pace_event
comp_grid_pace_on_weave(struct comp_grid_pace *p, bool late, uint64_t now_ns, uint64_t period_ns)
{
	if (p == NULL || period_ns == 0) {
		return COMP_PACE_EV_NONE;
	}
	if (p->win_start_ns == 0 || now_ns < p->win_last_ns || now_ns - p->win_last_ns > COMP_PACE_GAP_NS) {
		// First weave, or after a pause: start a fresh window here.
		p->win_start_ns = now_ns;
		p->win_last_ns = now_ns;
		p->win_weaves = 0;
		p->win_late = 0;
		return COMP_PACE_EV_NONE;
	}
	p->win_last_ns = now_ns;
	p->win_weaves++;
	p->win_late += late ? 1 : 0;
	const uint64_t elapsed = now_ns - p->win_start_ns;
	if (elapsed < COMP_PACE_WINDOW_NS) {
		return COMP_PACE_EV_NONE;
	}
	const double rate = (double)p->win_weaves / ((double)elapsed / (double)period_ns);
	const double late_share = (double)p->win_late / (double)p->win_weaves;
	p->last_late_share = late_share;
	p->win_start_ns = now_ns;
	p->win_weaves = 0;
	p->win_late = 0;

	enum comp_grid_pace_event ev = COMP_PACE_EV_NONE;
	if (p->mode == COMP_PACE_AIM) {
		p->aim_rate = rate;
		if (rate < COMP_PACE_LOW_RATE && late_share > COMP_PACE_LATE_SHARE && now_ns >= p->backoff_until_ns) {
			p->mode = COMP_PACE_THROUGHPUT;
			p->probing = true;
			p->probe_bar = rate;
			p->probes++;
			ev = COMP_PACE_EV_PROBE;
		}
	} else {
		p->thr_rate = rate;
		if (p->probing) {
			p->probing = false;
			if (rate >= p->probe_bar * COMP_PACE_PROVE_GAIN) {
				p->backoff_ns = COMP_PACE_BACKOFF_BASE_NS;
				ev = COMP_PACE_EV_PROVEN;
			} else {
				p->mode = COMP_PACE_AIM;
				p->backoff_until_ns = now_ns + p->backoff_ns;
				p->backoff_ns = p->backoff_ns * 2 > COMP_PACE_BACKOFF_MAX_NS ? COMP_PACE_BACKOFF_MAX_NS
				                                                             : p->backoff_ns * 2;
				ev = COMP_PACE_EV_REJECTED;
			}
		}
	}
	return ev;
}

static inline const char *
comp_grid_pace_mode_str(enum comp_grid_pace_mode m)
{
	return m == COMP_PACE_THROUGHPUT ? "THROUGHPUT" : "AIM";
}

/*
 * The weave budget: p90 of the last N weave costs.
 */

#define COMP_WEAVE_BUDGET_N 32

struct comp_weave_budget
{
	uint64_t ns[COMP_WEAVE_BUDGET_N];
	uint32_t n;
	uint32_t pos;
	//! Cached estimate, refreshed on every add.
	uint64_t p90_ns;
};

static inline void
comp_weave_budget_reset(struct comp_weave_budget *b)
{
	if (b != NULL) {
		memset(b, 0, sizeof(*b));
	}
}

static inline void
comp_weave_budget_add(struct comp_weave_budget *b, uint64_t cost_ns)
{
	if (b == NULL) {
		return;
	}
	b->ns[b->pos] = cost_ns;
	b->pos = (b->pos + 1) % COMP_WEAVE_BUDGET_N;
	if (b->n < COMP_WEAVE_BUDGET_N) {
		b->n++;
	}
	// Insertion sort of a copy: 32 elements, once per weave.
	uint64_t s[COMP_WEAVE_BUDGET_N];
	for (uint32_t i = 0; i < b->n; i++) {
		uint64_t v = b->ns[i];
		uint32_t j = i;
		while (j > 0 && s[j - 1] > v) {
			s[j] = s[j - 1];
			j--;
		}
		s[j] = v;
	}
	// Nearest-rank p90.
	uint32_t rank = (uint32_t)((90ULL * b->n + 99) / 100);
	if (rank < 1) {
		rank = 1;
	}
	b->p90_ns = s[rank - 1];
}

#ifdef __cplusplus
}
#endif
