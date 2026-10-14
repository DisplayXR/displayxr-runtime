// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Late-weave depth floor for the present_wait pacer (#2119): raised
 *         by measured commit-to-glass lag, lowered only by a verified probe.
 * @ingroup comp_util
 *
 * ## Why
 *
 * The VK_KHR_present_wait late-weave tier blocks in acquire until present id
 * `last - (depth - 1)` has hit glass. At depth 1 the next weave cannot start
 * before the previous frame is on glass, so the loop only runs at the panel
 * rate when a present reaches glass within one refresh of its commit. That
 * holds on a direct-scanout / fullscreen target (and on DXGI flip); it does
 * NOT hold for a windowed surface on a composited desktop (GNOME/Wayland),
 * which shows a commit about TWO refreshes later. Measured on ds1-linux (Intel
 * PTL, GNOME 50, in-process Unity player, 60 Hz): 30.0 present/s at depth 1,
 * 58.0 at depth 2, 55.5 with late weave off; the GPU clock is not involved.
 *
 * A per-OS default would trade one target for another — depth 2 costs a
 * refresh of latency on a 1-period target — so the depth is derived from what
 * the target measures, with depth 1 as the universal base:
 *
 *   effective depth = max(floor, slip level), ceiling 4
 *
 * where the slip level is the shared machine in comp_feedback_pace.h with no
 * level banked above the floor on this tier, so in practice depth = floor.
 *
 * ## The floor — asymmetric on purpose, after three CI iterations
 *
 * **Raised by lag, never lowered by lag.** `presented − commit` from the
 * window system (wp_presentation; on X11 the return of a present wait that
 * blocked) is used only from presents committed with ZERO of our presents
 * outstanding. How much of a sample taken behind our own queue is queueing is
 * not observable: a FIFO / frame callback that withholds the frame adds a
 * period per present ahead (round 2: a 5.5-period raw sample at depth 3),
 * while a pipelined compositor shows the frame `commit + 2 periods` regardless
 * (round 3: subtracting one period per outstanding present read a 2-period
 * pipeline as 0.95 periods, the floor fell to 1, and the app sat at 30
 * present/s for two minutes). Round 1 subtracted nothing and ran away to 4.
 * So: EMA of the clean samples (each clamped to EMA ± 0.5 period, so one
 * frame moves it by <= 0.05 period); the floor rises to round(EMA) once the
 * EMA reaches floor + 0.5 periods. At depth >= 2 the samples stop (there is
 * always one of ours outstanding), which is fine: the floor only needs them
 * to go UP.
 *
 * **Raised by depth-1 starvation.** A slipping window seen at depth 1 while
 * the acquire wait dominates the frame (wait EMA >= 0.8 period) is the
 * signature of depth-1 serialisation; it raises the floor to 2 even without a
 * clean lag sample (@ref comp_lw_lag_on_slip_window_at_cap).
 *
 * **Lowered only by a verified return probe.** After a dwell (30 s) at a
 * floor above 1, with the app at rate, the floor steps down one level. The
 * probe is judged by throughput: the mean paced frame interval over the ~1 s
 * after it against a slow EMA (~1-2 s) from before it. A rate drop of more
 * than 15 %, or @ref COMP_LW_PROBE_SLOW_FRAMES frames at >= 1.5 periods,
 * reverts it at once (which cuts the 30-present/s stretch to a fraction of a
 * second), doubles the dwell (30 -> 60 -> 120 -> 300 s) and logs `probe
 * failed`; otherwise the lower floor holds. Depth 2 is cheap by comparison
 * (the #206 weave->scanout horizon measured the same at depth 1 and 2), so
 * staying up a while longer costs nothing visible.
 *
 * Pure logic, no clocks or graphics types: unit-tested on the host
 * (tests/tests_comp_late_weave_depth.cpp).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Ceiling, same as COMP_FB_PACE_MAX_DEPTH / Windows LATE_WEAVE_MAX_DEPTH.
#define COMP_LW_LAG_MAX_DEPTH 4
//! Raise once the lag reaches floor + this many periods.
#define COMP_LW_LAG_RAISE_MARGIN 0.5
#define COMP_LW_LAG_MIN_SAMPLES 16
//! A sample may sit at most this many periods from the EMA.
#define COMP_LW_LAG_SAMPLE_CLAMP 0.5
//! Lag samples above this are a stall, not a property of the pipeline.
#define COMP_LW_LAG_MAX_NS (200ULL * 1000ULL * 1000ULL)
//! A frame gap longer than this is a pause, not a slow frame.
#define COMP_LW_LAG_PAUSE_NS (250ULL * 1000ULL * 1000ULL)
//! Depth-1 starvation: the acquire wait EMA is at least this many periods.
#define COMP_LW_LAG_STARVED_WAIT 0.8

#define COMP_LW_PROBE_DWELL_NS (30ULL * 1000ULL * 1000ULL * 1000ULL)
#define COMP_LW_PROBE_DWELL_MAX_NS (300ULL * 1000ULL * 1000ULL * 1000ULL)
//! How long a probe is watched before it is accepted.
#define COMP_LW_PROBE_VERIFY_NS (1000ULL * 1000ULL * 1000ULL)
//! A probe fails when the mean interval after it exceeds before / this.
#define COMP_LW_PROBE_RATE_KEEP 0.85
//! ...or after this many frames at >= 1.5 periods inside the verify window.
#define COMP_LW_PROBE_SLOW_FRAMES 10
//! Probe only from an app at rate (slow interval EMA within this many periods).
#define COMP_LW_PROBE_AT_RATE 1.2

enum comp_lw_lag_event
{
	COMP_LW_LAG_NONE = 0,
	COMP_LW_LAG_PROBE_DOWN,   //!< floor stepped down one level (on trial)
	COMP_LW_LAG_PROBE_FAILED, //!< throughput fell: reverted, dwell doubled
	COMP_LW_LAG_PROBE_HELD,   //!< throughput held: the lower floor stays
};

struct comp_lw_lag
{
	int depth; //!< current floor (1..MAX)

	//! EMA of commit-to-glass samples taken with 0 of ours outstanding.
	double ema_ns;
	uint32_t samples;

	// Throughput signal for the probe.
	uint64_t last_frame_ns;
	double interval_slow_ema_ns; //!< alpha 1/64: the "before" reference
	double wait_ema_ns;          //!< acquire wait, alpha 0.1

	uint64_t dwell_ns;
	uint64_t last_change_ns;

	// Probe under verification.
	bool probing;
	uint64_t probe_ns;
	double probe_before_ns; //!< slow interval EMA at probe time
	double probe_sum_ns;
	uint32_t probe_frames;
	uint32_t probe_slow;
	double last_probe_after_ns; //!< for the log line

	uint32_t probes_failed;
};

static inline void
comp_lw_lag_init(struct comp_lw_lag *l)
{
	if (l == NULL) {
		return;
	}
	memset(l, 0, sizeof(*l));
	l->depth = 1;
	l->dwell_ns = COMP_LW_PROBE_DWELL_NS;
}

static inline int
comp_lw_lag_clamp_depth(int n)
{
	return n < 1 ? 1 : (n > COMP_LW_LAG_MAX_DEPTH ? COMP_LW_LAG_MAX_DEPTH : n);
}

//! round(ema / period), clamped to 1..MAX; 0 when unmeasured.
static inline int
comp_lw_lag_target(const struct comp_lw_lag *l, uint64_t period_ns)
{
	if (l == NULL || period_ns == 0 || l->samples == 0) {
		return 0;
	}
	return comp_lw_lag_clamp_depth((int)(l->ema_ns / (double)period_ns + COMP_LW_LAG_RAISE_MARGIN));
}

static inline void
comp_lw_lag_raise_(struct comp_lw_lag *l, int to, uint64_t now_ns)
{
	if (l->probing) {
		// Raised while a probe was on trial: the probe failed.
		l->probing = false;
		l->dwell_ns =
		    l->dwell_ns * 2 > COMP_LW_PROBE_DWELL_MAX_NS ? COMP_LW_PROBE_DWELL_MAX_NS : l->dwell_ns * 2;
		l->probes_failed++;
	}
	l->depth = comp_lw_lag_clamp_depth(to);
	l->last_change_ns = now_ns;
}

/*!
 * Feed one commit-to-glass sample. Only samples committed with zero of our
 * presents outstanding count; the floor only ever RISES here.
 *
 * @return true when the floor rose.
 */
static inline bool
comp_lw_lag_on_sample(struct comp_lw_lag *l, uint64_t lag_ns, uint32_t outstanding, uint64_t period_ns, uint64_t now_ns)
{
	if (l == NULL || outstanding != 0 || lag_ns == 0 || lag_ns > COMP_LW_LAG_MAX_NS) {
		return false;
	}
	double v = (double)lag_ns;
	if (l->samples != 0 && period_ns != 0) {
		const double c = (double)period_ns * COMP_LW_LAG_SAMPLE_CLAMP;
		v = v > l->ema_ns + c ? l->ema_ns + c : (v < l->ema_ns - c ? l->ema_ns - c : v);
	}
	l->ema_ns = (l->samples == 0) ? v : l->ema_ns * 0.9 + v * 0.1;
	l->samples++;
	if (period_ns == 0 || l->samples < COMP_LW_LAG_MIN_SAMPLES) {
		return false;
	}
	const int want = comp_lw_lag_target(l, period_ns);
	if (want > l->depth) {
		comp_lw_lag_raise_(l, want, now_ns);
		return true;
	}
	return false;
}

/*!
 * A slipping window was seen at the depth cap. At depth 1, with the acquire
 * wait dominating the frame, that is depth-1 serialisation: raise to 2.
 *
 * @return true when the floor rose.
 */
static inline bool
comp_lw_lag_on_slip_window_at_cap(struct comp_lw_lag *l, uint64_t period_ns, uint64_t now_ns)
{
	if (l == NULL || l->depth != 1 || period_ns == 0) {
		return false;
	}
	if (l->wait_ema_ns < (double)period_ns * COMP_LW_LAG_STARVED_WAIT) {
		return false;
	}
	comp_lw_lag_raise_(l, 2, now_ns);
	return true;
}

/*!
 * Feed one paced frame: drives the return probe and its verification.
 *
 * @param interval_ns acquire-to-acquire time (0 = none: first frame).
 * @param wait_ns     how long the present wait blocked this frame.
 */
static inline enum comp_lw_lag_event
comp_lw_lag_on_frame(struct comp_lw_lag *l, uint64_t interval_ns, uint64_t wait_ns, uint64_t period_ns, uint64_t now_ns)
{
	if (l == NULL) {
		return COMP_LW_LAG_NONE;
	}
	l->wait_ema_ns = l->wait_ema_ns == 0.0 ? (double)wait_ns : l->wait_ema_ns * 0.9 + (double)wait_ns * 0.1;
	if (interval_ns == 0 || interval_ns > COMP_LW_LAG_PAUSE_NS || period_ns == 0) {
		if (interval_ns > COMP_LW_LAG_PAUSE_NS) {
			l->interval_slow_ema_ns = 0.0; // a pause is not a rate
		}
		return COMP_LW_LAG_NONE;
	}
	const double dt = (double)interval_ns;
	const double per = (double)period_ns;

	if (l->probing) {
		l->probe_sum_ns += dt;
		l->probe_frames++;
		if (dt >= per * 1.5) {
			l->probe_slow++;
		}
		const bool done = now_ns - l->probe_ns >= COMP_LW_PROBE_VERIFY_NS;
		const bool slow = l->probe_slow >= COMP_LW_PROBE_SLOW_FRAMES;
		if (!done && !slow) {
			return COMP_LW_LAG_NONE;
		}
		const double after = l->probe_sum_ns / (double)l->probe_frames;
		l->last_probe_after_ns = after;
		l->probing = false;
		if (slow || after > l->probe_before_ns / COMP_LW_PROBE_RATE_KEEP || after >= per * 1.5) {
			l->depth++;
			l->last_change_ns = now_ns;
			l->dwell_ns =
			    l->dwell_ns * 2 > COMP_LW_PROBE_DWELL_MAX_NS ? COMP_LW_PROBE_DWELL_MAX_NS : l->dwell_ns * 2;
			l->probes_failed++;
			return COMP_LW_LAG_PROBE_FAILED;
		}
		// Held: the probe's frames are a valid "before" for the next one.
		l->interval_slow_ema_ns = after;
		return COMP_LW_LAG_PROBE_HELD;
	}

	l->interval_slow_ema_ns =
	    l->interval_slow_ema_ns == 0.0 ? dt : l->interval_slow_ema_ns * (63.0 / 64.0) + dt / 64.0;

	if (l->depth > 1 && now_ns - l->last_change_ns >= l->dwell_ns &&
	    l->interval_slow_ema_ns <= per * COMP_LW_PROBE_AT_RATE) {
		l->probing = true;
		l->probe_ns = now_ns;
		l->probe_before_ns = l->interval_slow_ema_ns;
		l->probe_sum_ns = 0.0;
		l->probe_frames = 0;
		l->probe_slow = 0;
		l->depth--;
		l->last_change_ns = now_ns;
		return COMP_LW_LAG_PROBE_DOWN;
	}
	return COMP_LW_LAG_NONE;
}

//! The slip signal for the present_wait tier: a paced frame interval over
//! 1.55 periods is a vsync-doubled frame (the Windows governor's slip test).
static inline bool
comp_lw_lag_is_slip(uint64_t interval_ns, uint64_t period_ns)
{
	return period_ns != 0 && (double)interval_ns > (double)period_ns * 1.55;
}

#ifdef __cplusplus
}
#endif
