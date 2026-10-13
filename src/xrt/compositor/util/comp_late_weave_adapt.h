// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Self-correcting grid late-weave deadline for a COMPOSITED present (#2068).
 * @ingroup comp_util
 *
 * ## Why this exists
 *
 * Late weave aims a frame at a vblank. On Windows (DXGI flip) and Android that
 * vblank is the real deadline. On a Linux desktop we are never the DRM master:
 * GNOME / KDE latch client buffers some time BEFORE the vblank (up to about a
 * frame), so a weave aimed "1.5 ms before vblank" lands after the latch and is
 * shown one refresh later. Measured on an Intel box at 60 Hz with the grid tier
 * on: 40.5-43.4 presents/s against 52.9-56.7 with late weave off.
 *
 * The compositor's lead is not published anywhere, but its effect is: the
 * window system reports, per commit, when the frame actually reached the panel.
 * @ref comp_lw_lead learns from that: additive increase on a miss (the frame
 * landed after the vblank it was aimed at), a slow probe downward after a run
 * of hits. It has a floor where it gives up for the session and says why,
 * rather than keep costing frames: when the learned lead + weave cost + margin
 * no longer fit in a period there is no slack to weave late into.
 *
 * (The present_wait tier needs none of this: it waits on the frame actually
 * reaching glass, and measured at the panel rate either way on the same box.)
 *
 * Pure integer logic, no clocks or graphics types: unit-tested on the host
 * (tests/tests_comp_late_weave_adapt.cpp).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Lead added per miss (ns).
#define COMP_LW_LEAD_STEP_UP_NS (2ULL * 1000ULL * 1000ULL)
//! Lead removed after a run of hits (ns).
#define COMP_LW_LEAD_STEP_DOWN_NS (500ULL * 1000ULL)
//! Consecutive hits before the lead is probed lower (~2 s at 60 Hz).
#define COMP_LW_LEAD_HITS_TO_LOWER 120

enum comp_lw_adapt_event
{
	COMP_LW_ADAPT_NONE = 0,
	//! Grid: the lead rose. present_wait: escalated to a deeper latency.
	COMP_LW_ADAPT_RAISED,
	//! Grid: the lead was probed lower.
	COMP_LW_ADAPT_LOWERED,
	//! The tier gave up for the session.
	COMP_LW_ADAPT_DISABLED,
};

/*!
 * Grid tier: how far before the projected vblank, beyond the weave budget, a
 * weave must finish to make the compositor's latch.
 */
struct comp_lw_lead
{
	uint64_t lead_ns;
	uint32_t hits_in_row;
	bool initialised;
	bool off;
	//! Totals — diagnostics only.
	uint64_t hits;
	uint64_t misses;
};

/*!
 * Start conservative: a quarter period. Starting at 0 costs a burst of
 * misses on every launch; a quarter period is under most compositors' lead
 * and is probed down from there in 0.5 ms steps.
 */
static inline void
comp_lw_lead_init(struct comp_lw_lead *l, uint64_t period_ns)
{
	if (l == NULL || l->initialised) {
		return;
	}
	l->initialised = true;
	l->lead_ns = period_ns / 4;
}

/*!
 * Was a frame aimed at @p aimed_vblank_ns, presented at @p present_ns, a miss?
 * Anything past half a period after the aimed vblank landed on a later one.
 */
static inline bool
comp_lw_lead_is_miss(uint64_t aimed_vblank_ns, uint64_t present_ns, uint64_t period_ns)
{
	return present_ns > aimed_vblank_ns + period_ns / 2;
}

/*!
 * Feed one aimed frame's outcome.
 *
 * @param weave_cost_ns the weave budget the gate reserves (without margin).
 * @param margin_ns     the fixed wake-jitter margin.
 */
static inline enum comp_lw_adapt_event
comp_lw_lead_on_result(struct comp_lw_lead *l,
                       bool miss,
                       uint64_t period_ns,
                       uint64_t weave_cost_ns,
                       uint64_t margin_ns)
{
	if (l == NULL || l->off || period_ns == 0) {
		return COMP_LW_ADAPT_NONE;
	}
	comp_lw_lead_init(l, period_ns);
	if (miss) {
		l->misses++;
		l->hits_in_row = 0;
		l->lead_ns += COMP_LW_LEAD_STEP_UP_NS;
		if (l->lead_ns + weave_cost_ns + margin_ns >= period_ns) {
			l->off = true;
			return COMP_LW_ADAPT_DISABLED;
		}
		return COMP_LW_ADAPT_RAISED;
	}
	l->hits++;
	if (++l->hits_in_row >= COMP_LW_LEAD_HITS_TO_LOWER) {
		l->hits_in_row = 0;
		if (l->lead_ns > 0) {
			l->lead_ns = l->lead_ns > COMP_LW_LEAD_STEP_DOWN_NS ? l->lead_ns - COMP_LW_LEAD_STEP_DOWN_NS : 0;
			return COMP_LW_ADAPT_LOWERED;
		}
	}
	return COMP_LW_ADAPT_NONE;
}

#ifdef __cplusplus
}
#endif
