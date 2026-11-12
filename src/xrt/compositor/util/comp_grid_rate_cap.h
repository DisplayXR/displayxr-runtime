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
 * So a weave aims at a VBLANK, not at "now": the first vblank it can still
 * make (`now + budget`), pushed one vblank later when that is the vblank the
 * previous weave already aimed at. At most one weave per vblank whatever the
 * budget says, so a producer faster than the panel cannot fill the queue. A
 * producer that is already slower (a 60 Hz repaint on a 90 Hz panel, a slow
 * app) never meets the push, and the cap does nothing for it.
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

//! A previous target older than this many periods is not a neighbour.
#define COMP_GRID_RATE_CAP_STALE_PERIODS 4
//! The aim sleep never exceeds this many periods: a wrong anchor costs at
//! most that, never a stall.
#define COMP_GRID_RATE_CAP_MAX_SLEEP_PERIODS 2

/*!
 * The vblank this weave aims at, and how long to sleep first.
 *
 * @param last_target_ns the vblank the previous weave aimed at (0 = none).
 * @param out_sleep_ns sleep before starting the weave (0 = start now).
 * @return the target vblank, or 0 when the grid is not trusted (no aim).
 */
static inline uint64_t
comp_grid_rate_cap_target(const struct comp_vblank_grid *g,
                          uint64_t now_ns,
                          uint64_t budget_ns,
                          uint64_t last_target_ns,
                          uint64_t *out_sleep_ns)
{
	if (out_sleep_ns != NULL) {
		*out_sleep_ns = 0;
	}
	if (g == NULL || g->period_ns == 0) {
		return 0;
	}
	const uint64_t p = g->period_ns;
	// The first vblank this weave can still make.
	uint64_t target = comp_vblank_grid_next_vblank_after(g, now_ns + budget_ns);
	if (target == 0) {
		return 0;
	}
	const bool last_fresh =
	    last_target_ns != 0 &&
	    (last_target_ns >= now_ns || now_ns - last_target_ns <= COMP_GRID_RATE_CAP_STALE_PERIODS * p);
	if (last_fresh && target < last_target_ns + p - p / 4) {
		// Would share the previous weave's vblank: take the next one.
		const uint64_t next = comp_vblank_grid_next_vblank_after(g, last_target_ns + p / 2);
		if (next != 0 && next > target) {
			target = next;
		}
	}
	if (out_sleep_ns != NULL && target > now_ns + budget_ns) {
		uint64_t sleep_ns = target - budget_ns - now_ns;
		const uint64_t cap = COMP_GRID_RATE_CAP_MAX_SLEEP_PERIODS * p;
		*out_sleep_ns = sleep_ns > cap ? cap : sleep_ns;
	}
	return target;
}

#ifdef __cplusplus
}
#endif
