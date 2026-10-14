// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Late-weave depth floor for the present_wait pacer (#2119): raised
 *         by measured lag or starvation, lowered by measured slack.
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
 * ## The floor — every signal is one the pacer already measures for free
 *
 * **Raised by lag.** `presented − commit` from the window system
 * (wp_presentation; on X11 the return of a present wait that blocked), used
 * only from presents committed with ZERO of our presents outstanding: how
 * much of a sample taken behind our own queue is queueing is not observable
 * (a FIFO that withholds the frame adds a period per present ahead; a
 * pipelined compositor adds nothing). EMA of the clean samples, each clamped
 * to EMA ± 0.5 period; the floor rises to round(EMA) once the EMA reaches
 * floor + 0.5 periods. Never lowered by lag.
 *
 * **Raised by starvation.** Within a window of
 * @ref COMP_LW_STARVE_WINDOW frames, if most frames both waited
 * >= 0.8 period in the pacer AND took >= 1.5 periods, the wait IS the frame:
 * one level up. This is the depth-1 serialisation signature and needs no lag
 * sample; it also catches a lowering that was wrong within half a second.
 *
 * **Lowered by slack.** At depth d the pacer waits on present N−(d−1). If
 * over a dwell >= 95 % of those waits return in < 0.1 period, that present
 * was already on glass when acquire started — the pipeline has a whole period
 * to spare — so step down one level. No probe, no disruption: if the lower
 * depth is wrong, its waits become long at once and the starvation window
 * brings it back. A raise within two dwells of a lowering doubles the dwell
 * (5 -> 10 -> ... -> 300 s), so a target that keeps tempting a lowering
 * converges instead of oscillating.
 *
 * (History, three CI rounds: subtracting nothing from the lag ran away to
 * depth 4; subtracting depth − 1 or the outstanding count misread GNOME's
 * pipelined compositor and dropped to depth 1; a 1 s throughput probe worked
 * but cost a second at half rate per probe.)
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

//! Starvation: frames judged per window, and how many must be starved.
#define COMP_LW_STARVE_WINDOW 30
#define COMP_LW_STARVE_FRAMES 24
//! A starved frame waited at least this many periods...
#define COMP_LW_STARVE_WAIT 0.8
//! ...and took at least this many.
#define COMP_LW_STARVE_INTERVAL 1.5

//! Slack: a wait shorter than this many periods found the present on glass.
#define COMP_LW_SLACK_WAIT 0.1
//! Fraction of waits that must be slack over a dwell to lower.
#define COMP_LW_SLACK_SHARE 0.95
//! Minimum frames in a slack window before it can lower.
#define COMP_LW_SLACK_MIN_FRAMES 60

#define COMP_LW_DWELL_NS (5ULL * 1000ULL * 1000ULL * 1000ULL)
#define COMP_LW_DWELL_MAX_NS (300ULL * 1000ULL * 1000ULL * 1000ULL)

enum comp_lw_lag_event
{
	COMP_LW_LAG_NONE = 0,
	COMP_LW_LAG_RAISED_LAG,     //!< clean commit-to-glass samples
	COMP_LW_LAG_RAISED_STARVED, //!< the pacer's wait was the frame
	COMP_LW_LAG_LOWERED_SLACK,  //!< waits found the present already on glass
};

struct comp_lw_lag
{
	int depth; //!< current floor (1..MAX)

	//! EMA of commit-to-glass samples taken with 0 of ours outstanding.
	double ema_ns;
	uint32_t samples;

	// Starvation window.
	uint32_t starve_n;
	uint32_t starve_hits;

	// Slack window.
	uint64_t slack_start_ns;
	uint32_t slack_n;
	uint32_t slack_hits;
	double last_slack_share; //!< share of the window that lowered/was judged

	uint64_t dwell_ns;
	uint64_t last_change_ns;
	uint64_t last_lower_ns;
	uint32_t lowers_reverted; //!< raises within two dwells of a lowering
};

static inline void
comp_lw_lag_init(struct comp_lw_lag *l)
{
	if (l == NULL) {
		return;
	}
	memset(l, 0, sizeof(*l));
	l->depth = 1;
	l->dwell_ns = COMP_LW_DWELL_NS;
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
comp_lw_lag_reset_windows_(struct comp_lw_lag *l, uint64_t now_ns)
{
	l->starve_n = 0;
	l->starve_hits = 0;
	l->slack_start_ns = now_ns;
	l->slack_n = 0;
	l->slack_hits = 0;
}

static inline void
comp_lw_lag_raise_(struct comp_lw_lag *l, int to, uint64_t now_ns)
{
	// Raised again soon after a lowering: that lowering was wrong.
	if (l->last_lower_ns != 0 && now_ns - l->last_lower_ns < 2 * l->dwell_ns) {
		l->dwell_ns = l->dwell_ns * 2 > COMP_LW_DWELL_MAX_NS ? COMP_LW_DWELL_MAX_NS : l->dwell_ns * 2;
		l->lowers_reverted++;
		l->last_lower_ns = 0;
	}
	l->depth = comp_lw_lag_clamp_depth(to);
	l->last_change_ns = now_ns;
	comp_lw_lag_reset_windows_(l, now_ns);
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
 * Feed one paced frame: the starvation (raise) and slack (lower) windows.
 *
 * @param interval_ns acquire-to-acquire time (0 = none: first frame).
 * @param wait_ns     how long the present wait on N−(depth−1) blocked.
 */
static inline enum comp_lw_lag_event
comp_lw_lag_on_frame(struct comp_lw_lag *l, uint64_t interval_ns, uint64_t wait_ns, uint64_t period_ns, uint64_t now_ns)
{
	if (l == NULL || period_ns == 0) {
		return COMP_LW_LAG_NONE;
	}
	if (interval_ns > COMP_LW_LAG_PAUSE_NS) {
		comp_lw_lag_reset_windows_(l, now_ns); // a pause is neither
		return COMP_LW_LAG_NONE;
	}
	if (l->slack_start_ns == 0) {
		l->slack_start_ns = now_ns;
	}
	const double per = (double)period_ns;

	// Starvation: the wait is the frame.
	if (interval_ns != 0) {
		l->starve_n++;
		if ((double)wait_ns >= per * COMP_LW_STARVE_WAIT &&
		    (double)interval_ns >= per * COMP_LW_STARVE_INTERVAL) {
			l->starve_hits++;
		}
		if (l->starve_n >= COMP_LW_STARVE_WINDOW) {
			const bool starved = l->starve_hits >= COMP_LW_STARVE_FRAMES;
			l->starve_n = 0;
			l->starve_hits = 0;
			if (starved && l->depth < COMP_LW_LAG_MAX_DEPTH) {
				comp_lw_lag_raise_(l, l->depth + 1, now_ns);
				return COMP_LW_LAG_RAISED_STARVED;
			}
		}
	}

	// Slack: the present we waited on was already on glass.
	l->slack_n++;
	if ((double)wait_ns < per * COMP_LW_SLACK_WAIT) {
		l->slack_hits++;
	}
	if (now_ns - l->slack_start_ns >= l->dwell_ns) {
		const bool enough = l->slack_n >= COMP_LW_SLACK_MIN_FRAMES;
		const double share = l->slack_n != 0 ? (double)l->slack_hits / (double)l->slack_n : 0.0;
		l->last_slack_share = share;
		l->slack_start_ns = now_ns;
		l->slack_n = 0;
		l->slack_hits = 0;
		if (l->depth > 1 && enough && share >= COMP_LW_SLACK_SHARE &&
		    now_ns - l->last_change_ns >= l->dwell_ns) {
			l->depth--;
			l->last_change_ns = now_ns;
			l->last_lower_ns = now_ns;
			// The lag EMA froze at the higher depth (no clean samples there)
			// and describes a compositor that may have changed: re-learn it.
			l->ema_ns = 0.0;
			l->samples = 0;
			l->starve_n = 0;
			l->starve_hits = 0;
			return COMP_LW_LAG_LOWERED_SLACK;
		}
	}
	return COMP_LW_LAG_NONE;
}

/*!
 * The feedback tag to drain to before admitting the next frame, or 0.
 *
 * A raise must not leave more of our presents outstanding than the new depth
 * allows: the frames admitted while the depth was lower stay queued, and a
 * FIFO slot once filled never drains on its own (round 4: the residual sat at
 * 2.97 periods for ~50 s after a revert to depth 2). Before weaving, wait
 * until at most depth − 1 of ours are outstanding.
 *
 * @param last_tag  newest tag committed.
 * @param answered  newest tag the window system has answered (presented or
 *                  discarded).
 */
static inline uint64_t
comp_lw_lag_drain_tag(uint64_t last_tag, uint64_t answered, int depth)
{
	const uint64_t allowed = (uint64_t)(depth < 1 ? 0 : depth - 1);
	if (last_tag <= answered || last_tag - answered <= allowed) {
		return 0;
	}
	return last_tag - allowed;
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
