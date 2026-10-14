// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Feedback pacing: the present_wait latency-1 behaviour for apps
 *         without present_wait, paced on the window system's presentation
 *         feedback (#2068, desktop Linux).
 * @ingroup comp_util
 *
 * ## Why
 *
 * Measured on GNOME/Wayland, same compositor, same panel: an enable2 app paced
 * by `VK_KHR_present_wait` at latency 1 sees a weave->scanout residual of 0.90
 * refresh periods; an unpaced enable1 app sees 2.5-3.1. Both present through
 * the same runtime FIFO swapchain onto the app's wl_surface, so the difference
 * is frames in flight: an unpaced FIFO producer runs ahead until acquire
 * blocks, and every commit waits behind the queue. enable1 apps usually do not
 * chain presentId/presentWait, so present_wait cannot pace them — but the
 * runtime already receives `wp_presentation.presented` for every commit. This
 * paces on that: before weaving frame N, wait (bounded) until the feedback
 * for commit N-latency has arrived.
 *
 * ## Policy — the Windows governor's slip rules (comp_weave_latency_win.h)
 *
 * - A wait that times out (> 1.5 periods) is a SLIP: the frame weaves anyway.
 * - Judged per @ref COMP_FB_PACE_WINDOW waits: >= @ref COMP_FB_PACE_SLIP_LIMIT
 *   slips (~8%) escalates one latency level, capped at base + 1 (the "one
 *   banked token" cap of DXR_LATE_WEAVE_SLIP_CAP).
 * - Above base, THREE consecutive clean windows and a dwell (30 s) allow a
 *   return probe one level down. A re-escalation within two dwells of a probe
 *   is a failed probe and doubles the dwell (30 -> 60 -> 120 -> 300 s).
 * - DXR_LATE_WEAVE_AUTOBACKOFF=0 holds the latency at base.
 * - An optional measured FLOOR (#2119, @ref comp_fb_pace_set_floor) sits
 *   under the slip machine: the effective latency is max(depth, floor), slip
 *   escalation is capped at max(base, floor) + 1 and starts from the effective
 *   latency, and a floor that falls counts as a return probe for the
 *   failed-probe rule. The present_wait tier sets it from the measured
 *   commit-to-glass lag (comp_late_weave_depth.h); the feedback-pacing path
 *   leaves it at 0, which is the pre-#2119 behaviour exactly.
 * - Never resolved: three slips in a row before any wait has ever succeeded
 *   (no feedback is arriving at all) turns pacing off for the session — the
 *   present_wait tier's 3-timeouts rule.
 *
 * Pure logic, no clocks or window-system types: unit-tested on the host
 * (tests/tests_comp_feedback_pace.cpp).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMP_FB_PACE_WINDOW 120
#define COMP_FB_PACE_SLIP_LIMIT 10
#define COMP_FB_PACE_CLEAN_WINDOWS 3
#define COMP_FB_PACE_MAX_DEPTH 4
#define COMP_FB_PACE_DWELL_NS (30ULL * 1000ULL * 1000ULL * 1000ULL)
#define COMP_FB_PACE_DWELL_MAX_NS (300ULL * 1000ULL * 1000ULL * 1000ULL)
#define COMP_FB_PACE_NEVER_RESOLVED 3

enum comp_fb_pace_event
{
	COMP_FB_PACE_NONE = 0,
	//! Slip-rate escalation: one level deeper.
	COMP_FB_PACE_DEEPER,
	//! Return probe: one level shallower.
	COMP_FB_PACE_SHALLOWER,
	//! No feedback ever arrived: pacing off for the session.
	COMP_FB_PACE_OFF,
};

struct comp_fb_pace
{
	int base;  //!< configured latency (DXR_LATE_WEAVE_LINUX_PACE_MAX_LATENCY)
	int depth; //!< slip-machine latency (effective = max(depth, floor))
	int floor; //!< measured floor, 0 = none (#2119)
	bool autobackoff;
	bool off;
	bool ever_ok;
	uint32_t slips_in_row;

	uint32_t win_waits;
	uint32_t win_slips;
	uint32_t clean_windows;
	uint64_t dwell_ns;
	uint64_t last_change_ns;
	uint64_t last_probe_ns;

	//! Totals since the last stats read — for the 5 s trace line.
	uint64_t waits;
	uint64_t slips;
	uint64_t wait_ns_sum;
};

static inline void
comp_fb_pace_init(struct comp_fb_pace *p, int base, bool autobackoff)
{
	if (p == NULL) {
		return;
	}
	struct comp_fb_pace z = {0};
	*p = z;
	p->base = base < 1 ? 1 : (base > COMP_FB_PACE_MAX_DEPTH ? COMP_FB_PACE_MAX_DEPTH : base);
	p->depth = p->base;
	p->autobackoff = autobackoff;
	p->dwell_ns = COMP_FB_PACE_DWELL_NS;
}

//! The latency actually paced at: max(depth, floor).
static inline int
comp_fb_pace_effective(const struct comp_fb_pace *p)
{
	if (p == NULL) {
		return 1;
	}
	return p->floor > p->depth ? p->floor : p->depth;
}

/*!
 * Set the measured floor (#2119), clamped to 0..MAX. A floor that lowers the
 * effective latency is a return probe: a slip escalation within two dwells of
 * it doubles the dwell, as for the slip machine's own probes.
 *
 * @return true when the effective latency changed.
 */
static inline bool
comp_fb_pace_set_floor(struct comp_fb_pace *p, int floor, uint64_t now_ns)
{
	if (p == NULL) {
		return false;
	}
	floor = floor < 0 ? 0 : (floor > COMP_FB_PACE_MAX_DEPTH ? COMP_FB_PACE_MAX_DEPTH : floor);
	const int before = comp_fb_pace_effective(p);
	p->floor = floor;
	// A slip level at or under the floor buys nothing; let it be re-earned
	// relative to the floor instead of resurfacing if the floor later falls.
	if (floor >= p->depth) {
		p->depth = p->base;
	}
	const int after = comp_fb_pace_effective(p);
	if (after < before) {
		p->last_change_ns = now_ns;
		p->last_probe_ns = now_ns;
	}
	return after != before;
}

/*!
 * The commit to wait for before weaving, given the newest commit issued, or 0
 * for "nothing to wait for yet".
 */
static inline uint64_t
comp_fb_pace_wait_target(const struct comp_fb_pace *p, uint64_t last_commit_seq)
{
	if (p == NULL || p->off || last_commit_seq == 0) {
		return 0;
	}
	const uint64_t relax = (uint64_t)(comp_fb_pace_effective(p) - 1);
	return last_commit_seq > relax ? last_commit_seq - relax : 0;
}

/*!
 * Bounded wait for one pacing decision: 1.5 periods (a missed vblank plus
 * margin), or 25 ms when the period is not known yet.
 */
static inline uint64_t
comp_fb_pace_timeout_ns(uint64_t period_ns)
{
	return period_ns != 0 ? period_ns + period_ns / 2 : 25ULL * 1000ULL * 1000ULL;
}

/*!
 * Feed the outcome of one wait.
 *
 * @param ok      the feedback arrived inside the timeout.
 * @param wait_ns how long the wait blocked.
 * @param now_ns  monotonic now, for the dwell rules.
 */
static inline enum comp_fb_pace_event
comp_fb_pace_on_wait(struct comp_fb_pace *p, bool ok, uint64_t wait_ns, uint64_t now_ns)
{
	if (p == NULL || p->off) {
		return COMP_FB_PACE_NONE;
	}
	p->waits++;
	p->wait_ns_sum += wait_ns;
	if (ok) {
		p->ever_ok = true;
		p->slips_in_row = 0;
	} else {
		p->slips++;
		p->win_slips++;
		if (!p->ever_ok && ++p->slips_in_row >= COMP_FB_PACE_NEVER_RESOLVED) {
			p->off = true;
			return COMP_FB_PACE_OFF;
		}
	}

	if (++p->win_waits < COMP_FB_PACE_WINDOW) {
		return COMP_FB_PACE_NONE;
	}
	const bool slipping = p->win_slips >= COMP_FB_PACE_SLIP_LIMIT;
	p->win_waits = 0;
	p->win_slips = 0;
	p->clean_windows = slipping ? 0 : p->clean_windows + 1;
	if (!p->autobackoff) {
		return COMP_FB_PACE_NONE;
	}

	if (slipping) {
		const int top = p->floor > p->base ? p->floor : p->base;
		const int cap = top + 1 > COMP_FB_PACE_MAX_DEPTH ? COMP_FB_PACE_MAX_DEPTH : top + 1;
		const int eff = comp_fb_pace_effective(p);
		if (eff >= cap) {
			return COMP_FB_PACE_NONE;
		}
		// Re-escalating soon after a probe: the probe failed.
		if (p->last_probe_ns != 0 && now_ns - p->last_probe_ns < 2 * p->dwell_ns) {
			p->dwell_ns = p->dwell_ns * 2 > COMP_FB_PACE_DWELL_MAX_NS ? COMP_FB_PACE_DWELL_MAX_NS
			                                                          : p->dwell_ns * 2;
		}
		// From the effective latency: a level under the floor buys nothing.
		p->depth = eff + 1;
		p->last_change_ns = now_ns;
		return COMP_FB_PACE_DEEPER;
	}

	if (p->depth > p->base && p->depth > p->floor && p->clean_windows >= COMP_FB_PACE_CLEAN_WINDOWS &&
	    now_ns - p->last_change_ns >= p->dwell_ns) {
		p->depth--;
		p->clean_windows = 0;
		p->last_change_ns = now_ns;
		p->last_probe_ns = now_ns;
		return COMP_FB_PACE_SHALLOWER;
	}
	return COMP_FB_PACE_NONE;
}

//! Take and reset the trace totals.
static inline void
comp_fb_pace_take_stats(struct comp_fb_pace *p, uint64_t *waits, uint64_t *slips, uint64_t *wait_ns_sum)
{
	*waits = p->waits;
	*slips = p->slips;
	*wait_ns_sum = p->wait_ns_sum;
	p->waits = 0;
	p->slips = 0;
	p->wait_ns_sum = 0;
}

#ifdef __cplusplus
}
#endif
