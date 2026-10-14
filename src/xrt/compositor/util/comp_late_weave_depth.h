// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Measured commit-to-glass lag -> late-weave depth floor (#2119).
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
 *   effective depth = max(lag floor, slip/saturation level), ceiling 4
 *
 * - **Lag floor (this file).** EMA of the compositor's INTRINSIC
 *   commit-to-glass latency per present: `presented − commit` minus the
 *   periods the frame spent queued behind our own earlier presents
 *   (@ref comp_lw_lag_intrinsic_ns). Without that correction the floor runs
 *   away: at depth d the app commits d−1 frames ahead, each sample grows by
 *   ~(d−1) periods of FIFO wait, the floor rises, the depth rises, and so on —
 *   measured on the PR's first CI build: 1 -> 2 -> 3 -> 4 with the sample at
 *   3.51 periods and the #206 residual doubled from 1.99 to 3.99 periods. The
 *   raw sample comes from the window system's presentation feedback (wp_presentation; or, where the
 *   feedback cannot be correlated per present, the return of a present wait
 *   that actually blocked). The floor is the lag rounded to whole periods:
 *   RAISED as soon as the EMA (after @ref COMP_LW_LAG_MIN_SAMPLES samples)
 *   reaches floor + 0.5 periods; LOWERED only after round(lag) has sat below
 *   the floor continuously for a dwell (30 s), and a re-raise within two
 *   dwells of a lowering is a failed probe that doubles the dwell (30 -> 60
 *   -> 120 -> 300 s). A composited GNOME window reads 2, direct scanout 1, a
 *   PRIME / copy path whatever it measures — the generic form of "GNOME needs
 *   2".
 *
 *   The lag is NOT a constant per desktop, which is why this is a tracked
 *   signal with hysteresis and never a one-shot measurement: mutter switches
 *   between double and triple buffering on its own measured render time, so
 *   the same window flips between 1 and 2 periods of lag within one run —
 *   and depth-1 pacing feeds back into it (less app load -> double buffering
 *   -> 1 period -> we run faster -> more load -> triple -> 2 periods -> we
 *   halve). Measured: 5 s windows of the same Unity window at depth 1
 *   alternating 59, 34, 97, 60, 61, 60, 58, 24, 98, 61 presents/s. Depth 2 is
 *   cheap by comparison (the #206 weave->scanout horizon measured the same at
 *   depth 1 and 2), so the asymmetry is deliberate: up fast, down slow.
 * - **Saturation level** — the existing slip machine in
 *   comp_feedback_pace.h (the Windows governor's slip rules: per-120-frame
 *   slip windows escalate one level above the effective depth, three clean
 *   windows + a doubling dwell probe back down), fed with "paced frame
 *   interval > 1.55 periods" as the slip, and given this floor through
 *   comp_fb_pace_set_floor().
 *
 * Why the floor matters for the slip machine: without it, a composited window
 * at depth 1 slips every frame, escalates to 2, goes clean, and the return
 * probe drops it back to 1 — a guaranteed ~1 s at half rate every dwell. With
 * the floor at 2, depth 1 is never probed there.
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
#define COMP_LW_LAG_DWELL_NS (30ULL * 1000ULL * 1000ULL * 1000ULL)
#define COMP_LW_LAG_DWELL_MAX_NS (300ULL * 1000ULL * 1000ULL * 1000ULL)
//! Lag samples above this are a stall, not a property of the pipeline.
#define COMP_LW_LAG_MAX_NS (200ULL * 1000ULL * 1000ULL)

struct comp_lw_lag
{
	double ema_ns;
	uint32_t samples;
	int depth;               //!< current floor (1..MAX)
	uint64_t lower_since_ns; //!< round(lag) first sat below the floor; 0 = not
	uint64_t dwell_ns;       //!< calm needed before a lowering (doubles on failure)
	uint64_t last_lower_ns;  //!< last lowering (a probe)
	uint32_t changes;
};

static inline void
comp_lw_lag_init(struct comp_lw_lag *l)
{
	if (l == NULL) {
		return;
	}
	memset(l, 0, sizeof(*l));
	l->depth = 1;
	l->dwell_ns = COMP_LW_LAG_DWELL_NS;
}

//! round(ema / period), clamped to 1..MAX; 0 when unmeasured.
static inline int
comp_lw_lag_target(const struct comp_lw_lag *l, uint64_t period_ns)
{
	if (l == NULL || period_ns == 0 || l->samples == 0) {
		return 0;
	}
	const int n = (int)(l->ema_ns / (double)period_ns + COMP_LW_LAG_RAISE_MARGIN);
	return n < 1 ? 1 : (n > COMP_LW_LAG_MAX_DEPTH ? COMP_LW_LAG_MAX_DEPTH : n);
}

/*!
 * Feed one commit-to-glass sample.
 *
 * @param now_ns monotonic now, for the lowering dwell.
 * @return true when the floor (@ref comp_lw_lag::depth) changed.
 */
static inline bool
comp_lw_lag_on_sample(struct comp_lw_lag *l, uint64_t lag_ns, uint64_t period_ns, uint64_t now_ns)
{
	if (l == NULL || lag_ns == 0 || lag_ns > COMP_LW_LAG_MAX_NS) {
		return false;
	}
	l->ema_ns = (l->samples == 0) ? (double)lag_ns : l->ema_ns * 0.9 + (double)lag_ns * 0.1;
	l->samples++;
	if (period_ns == 0 || l->samples < COMP_LW_LAG_MIN_SAMPLES) {
		return false;
	}
	const int want = comp_lw_lag_target(l, period_ns);
	if (want > l->depth) {
		// Up fast. Re-raising soon after a lowering: the probe failed.
		if (l->last_lower_ns != 0 && now_ns - l->last_lower_ns < 2 * l->dwell_ns) {
			l->dwell_ns =
			    l->dwell_ns * 2 > COMP_LW_LAG_DWELL_MAX_NS ? COMP_LW_LAG_DWELL_MAX_NS : l->dwell_ns * 2;
		}
		l->depth = want;
		l->lower_since_ns = 0;
		l->changes++;
		return true;
	}
	if (want < l->depth) {
		// Down slow: only after the lower value held for a whole dwell.
		if (l->lower_since_ns == 0) {
			l->lower_since_ns = now_ns;
			return false;
		}
		if (now_ns - l->lower_since_ns < l->dwell_ns) {
			return false;
		}
		l->depth--; // one level at a time
		l->lower_since_ns = 0;
		l->last_lower_ns = now_ns;
		l->changes++;
		return true;
	}
	l->lower_since_ns = 0;
	return false;
}

/*!
 * The compositor's intrinsic commit-to-glass latency from one raw sample:
 * `sample − queued_ahead × period`, where @p queued_ahead is how many of our
 * own presents were ahead of this one at commit (the effective depth − 1 the
 * pacer allowed). At least 1 ns, so a fully-queue-explained sample still
 * counts as "well under a period". The estimate is the same at every depth,
 * which is what keeps the floor from feeding on its own depth.
 */
static inline uint64_t
comp_lw_lag_intrinsic_ns(uint64_t sample_ns, uint32_t queued_ahead, uint64_t period_ns)
{
	const uint64_t q = (uint64_t)queued_ahead * period_ns;
	return sample_ns > q ? sample_ns - q : 1;
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
