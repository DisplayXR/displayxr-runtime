// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  One weave per vblank for the grid late-weave tier (#2629): pick the
 *         vblank a weave aims at so two weaves never share one.
 * @ingroup comp_util
 *
 * ## Why
 *
 * The grid tier used to sleep only when the next vblank was further away than
 * the weave budget. The budget is a decaying high-water of the weave's cost,
 * and that cost included a blocking acquire. Once a FIFO queue was full the
 * acquire blocked, the budget rose toward a period, the sleep condition went
 * false and the tier stopped pacing: the producer free-ran against the
 * blocking acquire, the queue stayed full, and any slot a drain freed was
 * refilled at once (measured on a phone-class device at 60 Hz: residual
 * ~80 ms, ~4.8 periods).
 *
 * So the tier keeps the vblank the previous weave will be SHOWN at (FIFO:
 * the later of the vblank its present returned before and one after the
 * frame ahead of it — @ref comp_grid_rate_cap_on_present) and a weave whose
 * present, at its budget, would land on or before that vblank is pushed to
 * the one after.
 * At most one frame per vblank, so a producer faster than the panel cannot
 * fill the queue whatever the budget says. A producer slower than the panel
 * (a 60 Hz repaint on a 90 Hz panel, a slow app) never meets the push.
 *
 * THROUGHPUT FIRST (device round 2). Round 1 also slept toward the vblank
 * AFTER an unreachable one; that delayed the app's submit thread (under the
 * compositor lock) until its next frame missed too — one weave per two
 * vblanks, 65.7 weaves/s on a 90 Hz panel. Now, when the next vblank is not
 * reachable and not taken, the weave starts at once, as before the cap. The
 * only sleep beyond the usual late-weave aim is the push (see the cap in
 * @ref comp_grid_rate_cap_target).
 *
 * WHY THE BOOKKEEPING IS THE SHOWN VBLANK (device round 2). Round 1 kept the
 * vblank it AIMED at. A weave that overruns returns its present after that,
 * and one with a budget above its cost returns before the vblank aimed at;
 * either way it can land on the vblank the next weave aims at — two frames
 * in one vblank, and one stays queued. With app frames and repaints filling
 * every vblank (a 30 fps video on a 60 Hz panel) any such slip collides,
 * which is how the media player regained a queued frame ~7 s in (with
 * repaint off the slip landed in an empty vblank). Tracking where frames are
 * shown, from where their presents actually returned, closes that.
 *
 * Pure logic over comp_vblank_grid.h; unit-tested on the host
 * (tests/tests_comp_grid_rate_cap.cpp).
 */

#pragma once

#include "util/comp_vblank_grid.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A shown vblank older than this many periods is not a neighbour.
#define COMP_GRID_RATE_CAP_STALE_PERIODS 4

enum comp_grid_rate_cap_kind
{
	COMP_GRID_CAP_NONE = 0, //!< grid not trusted: no aim
	COMP_GRID_CAP_AIMED,    //!< the next vblank, reachable: the usual late-weave sleep
	COMP_GRID_CAP_NOW,      //!< the next vblank is not reachable: weave at once
	COMP_GRID_CAP_PUSHED,   //!< the next vblank already shows the previous frame: one later
};

static inline bool
comp_grid_rate_cap_fresh_(uint64_t shown_ns, uint64_t now_ns, uint64_t p)
{
	return shown_ns != 0 && (shown_ns >= now_ns || now_ns - shown_ns <= COMP_GRID_RATE_CAP_STALE_PERIODS * p);
}

/*!
 * The vblank this weave aims at, and how long to sleep first.
 *
 * @param last_shown_ns the vblank the previous frame will be shown at
 *        (from @ref comp_grid_rate_cap_on_present; 0 = none).
 * @param out_sleep_ns sleep before starting the weave (0 = start now).
 * @param out_kind which case applied (may be NULL).
 * @return the target vblank, or 0 when the grid is not trusted (no aim).
 */
static inline uint64_t
comp_grid_rate_cap_target(const struct comp_vblank_grid *g,
                          uint64_t now_ns,
                          uint64_t budget_ns,
                          uint64_t last_shown_ns,
                          uint64_t *out_sleep_ns,
                          enum comp_grid_rate_cap_kind *out_kind)
{
	if (out_sleep_ns != NULL) {
		*out_sleep_ns = 0;
	}
	if (out_kind != NULL) {
		*out_kind = COMP_GRID_CAP_NONE;
	}
	if (g == NULL || g->period_ns == 0) {
		return 0;
	}
	const uint64_t p = g->period_ns;
	const uint64_t next = comp_vblank_grid_next_vblank_after(g, now_ns);
	if (next == 0) {
		return 0;
	}
	enum comp_grid_rate_cap_kind kind;
	uint64_t target;
	uint64_t sleep_ns = 0;
	// Where the present lands if the weave costs its budget (a high-water,
	// so this is the late case; a cheaper weave lands at or before it).
	const uint64_t est = comp_vblank_grid_next_vblank_after(g, now_ns + budget_ns);
	if (est == 0) {
		return 0;
	}
	if (comp_grid_rate_cap_fresh_(last_shown_ns, now_ns, p) && est < last_shown_ns + p / 2) {
		// It would land on the vblank that already shows the previous frame:
		// aim at the one after it. (A weave cheaper than its budget can
		// still return early into the taken vblank; it then waits behind the
		// frame ahead and on_present books it one later — never two.)
		target = comp_vblank_grid_next_vblank_after(g, last_shown_ns + p / 2);
		if (target == 0) {
			return 0;
		}
		kind = COMP_GRID_CAP_PUSHED;
		if (target > now_ns + budget_ns) {
			sleep_ns = target - budget_ns - now_ns;
		}
	} else if (next > now_ns + budget_ns) {
		kind = COMP_GRID_CAP_AIMED;
		target = next;
		sleep_ns = next - budget_ns - now_ns;
	} else {
		// Not reachable and not taken: weave at once (throughput first).
		kind = COMP_GRID_CAP_NOW;
		target = est;
	}
	if (out_sleep_ns != NULL) {
		// A wrong anchor must cost a frame, never a stall: one period at
		// most, except a push, which may need up to two — the producer is
		// AHEAD of the panel there (its present would land on a vblank that
		// is already taken), so sleeping costs it no throughput, and a
		// shorter cap leaves it weaving early forever after one overrun
		// instead of re-locking to the late aim.
		const uint64_t cap = kind == COMP_GRID_CAP_PUSHED ? 2 * p : p;
		*out_sleep_ns = sleep_ns > cap ? cap : sleep_ns;
	}
	if (out_kind != NULL) {
		*out_kind = kind;
	}
	return target;
}

/*!
 * A present just returned at @p present_ret_ns: the vblank that frame will be
 * shown at — the first vblank after the return, or one after the previous
 * frame's (@p last_shown_ns) when that is later (FIFO). 0 when the grid is
 * not trusted.
 */
static inline uint64_t
comp_grid_rate_cap_on_present(const struct comp_vblank_grid *g, uint64_t last_shown_ns, uint64_t present_ret_ns)
{
	if (g == NULL || g->period_ns == 0) {
		return 0;
	}
	const uint64_t p = g->period_ns;
	const uint64_t made = comp_vblank_grid_next_vblank_after(g, present_ret_ns);
	if (made == 0) {
		return 0;
	}
	if (comp_grid_rate_cap_fresh_(last_shown_ns, present_ret_ns, p) && made < last_shown_ns + p / 2) {
		return comp_vblank_grid_next_vblank_after(g, last_shown_ns + p / 2);
	}
	return made;
}

#ifdef __cplusplus
}
#endif
