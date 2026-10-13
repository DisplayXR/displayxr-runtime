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
 * took one vblank more from commit to glass than the desktop's pipeline needs),
 * a slow probe downward after a run of hits. It has a floor where it gives up for the session and says why,
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
	//! The lead rose.
	COMP_LW_ADAPT_RAISED,
	//! The lead was probed lower.
	COMP_LW_ADAPT_LOWERED,
	//! The tier gave up for the session.
	COMP_LW_ADAPT_DISABLED,
};

//! Un-aimed samples observed before the gate starts aiming (~0.5 s at 60 Hz).
#define COMP_LW_LEAD_PRIME_SAMPLES 30

/*!
 * Grid tier: how far before the projected vblank, beyond the weave budget, a
 * weave must COMMIT to make the compositor's latch.
 *
 * ## What counts as a miss (measured, #2068)
 *
 * On a composited desktop a frame is never shown at the vblank right after its
 * commit: the compositor composes on one refresh and scans out on a later one.
 * Measured on GNOME/Wayland: 2.76 refresh periods from weave start to glass.
 * The first version of this controller called a frame aimed at vblank V a miss
 * whenever it landed after V — which, behind a compositor, is EVERY frame, so
 * the lead climbed 2 ms per frame to the floor and the tier turned itself off
 * in four frames (panel log: "lead 12.17 ms, aimed hits 0, misses 4 [OFF]").
 * No lead inside one period can remove a whole-vblank pipeline delay.
 *
 * So the unit is "vblanks from commit to glass", @ref comp_lw_lead_vblanks. The
 * desktop's own pipeline sets a floor — the fewest vblanks any commit has
 * needed (@ref comp_lw_lead::base_vblanks) — and a frame is a hit when it made
 * that floor, a miss when it took one more (it committed after the latch).
 *
 * ## Where the lead starts
 *
 * From the data, not a guess: before aiming anything, the gate watches
 * @ref COMP_LW_LEAD_PRIME_SAMPLES un-aimed frames. Each one that made the floor
 * proves its commit-to-next-vblank gap was early enough; the smallest such gap
 * (less the wake margin) is a lead known to hit. The controller starts there
 * and probes downward, so it begins on the safe side rather than learning by
 * missing.
 */
struct comp_lw_lead
{
	uint64_t lead_ns;
	uint32_t hits_in_row;
	bool initialised;
	bool off;
	//! Fewest vblanks from commit to glass seen this session; 0 = none yet.
	uint32_t base_vblanks;
	//! Smallest commit-to-next-vblank gap of an un-aimed frame that made
	//! @ref base_vblanks.
	uint64_t min_hit_gap_ns;
	bool have_hit_gap;
	//! Un-aimed samples observed.
	uint32_t observed;
	//! Totals — diagnostics only.
	uint64_t hits;
	uint64_t misses;
};

/*!
 * Vblanks from a commit at @p commit_ns to glass at @p present_ns (1 = shown
 * at the first vblank after the commit), and the gap from the commit to that
 * first vblank. Presentation timestamps sit on the vblank grid, so both follow
 * from the difference alone.
 *
 * @return 0 when the sample cannot be placed.
 */
static inline uint32_t
comp_lw_lead_vblanks(uint64_t commit_ns, uint64_t present_ns, uint64_t period_ns, uint64_t *out_gap_ns)
{
	if (out_gap_ns != NULL) {
		*out_gap_ns = 0;
	}
	if (period_ns == 0 || commit_ns == 0 || present_ns <= commit_ns) {
		return 0;
	}
	const uint64_t d = present_ns - commit_ns;
	if (out_gap_ns != NULL) {
		*out_gap_ns = d % period_ns;
	}
	return (uint32_t)(d / period_ns) + 1;
}

//! Enough un-aimed data to start aiming?
static inline bool
comp_lw_lead_primed(const struct comp_lw_lead *l)
{
	return l != NULL && l->base_vblanks != 0 && l->have_hit_gap &&
	       l->observed >= COMP_LW_LEAD_PRIME_SAMPLES;
}

/*!
 * Start aiming: the lead is the smallest gap that is known to have hit, less
 * the wake margin the gate adds on top. Call once primed.
 */
static inline void
comp_lw_lead_init(struct comp_lw_lead *l, uint64_t margin_ns)
{
	if (l == NULL || l->initialised || !comp_lw_lead_primed(l)) {
		return;
	}
	l->initialised = true;
	l->lead_ns = l->min_hit_gap_ns > margin_ns ? l->min_hit_gap_ns - margin_ns : 0;
}

/*!
 * Feed one presented frame (vsync'd, not discarded): its commit time, where it
 * landed, and whether the gate aimed it.
 *
 * @param weave_cost_ns the weave budget the gate reserves (without margin).
 * @param margin_ns     the fixed wake-jitter margin.
 */
static inline enum comp_lw_adapt_event
comp_lw_lead_observe(struct comp_lw_lead *l,
                     uint64_t commit_ns,
                     uint64_t present_ns,
                     bool aimed,
                     uint64_t period_ns,
                     uint64_t weave_cost_ns,
                     uint64_t margin_ns)
{
	if (l == NULL || l->off) {
		return COMP_LW_ADAPT_NONE;
	}
	uint64_t gap = 0;
	const uint32_t v = comp_lw_lead_vblanks(commit_ns, present_ns, period_ns, &gap);
	if (v == 0) {
		return COMP_LW_ADAPT_NONE;
	}
	if (l->base_vblanks == 0 || v < l->base_vblanks) {
		// A shorter pipeline than any seen: the old hit gaps were measured
		// against the wrong floor.
		l->base_vblanks = v;
		l->have_hit_gap = false;
	}

	if (!aimed || !l->initialised) {
		l->observed++;
		if (v == l->base_vblanks && (!l->have_hit_gap || gap < l->min_hit_gap_ns)) {
			l->min_hit_gap_ns = gap;
			l->have_hit_gap = true;
		}
		return COMP_LW_ADAPT_NONE;
	}

	if (v > l->base_vblanks) {
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
