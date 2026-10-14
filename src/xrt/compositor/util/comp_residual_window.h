// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windowed statistics of the #206 weave->scanout residual (#2068).
 * @ingroup comp_util
 *
 * The cadence trace used to print the LAST measured residual every 5 s. On a
 * panel A/B that misled: early samples (n = 1..138) read 2.5-3.1 periods for
 * a native app that, once warm, sits near 0.9 — a single sample cannot tell a
 * transient from the distribution. This collects every sample of one trace
 * window and summarises it: n, mean, p50, p95, max, and the fraction above
 * 1.5 periods (a frame that waited behind another, or missed the cycle).
 *
 * Pure logic, unit-tested on the host (tests/tests_comp_residual_window.cpp).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Samples kept per window: 5 s at 240 Hz is 1200. Beyond this, later
//! samples count toward n / mean / max / the tail fraction but not the
//! percentiles (reported via @ref comp_residual_stats::truncated).
#define COMP_RESIDUAL_WINDOW_CAP 2048

struct comp_residual_window
{
	uint32_t ns_us[COMP_RESIDUAL_WINDOW_CAP]; //!< samples, microseconds
	uint32_t kept;
	uint64_t n;
	uint64_t sum_ns;
	uint64_t max_ns;
};

struct comp_residual_stats
{
	uint64_t n;
	double mean_ns;
	uint64_t p50_ns;
	uint64_t p95_ns;
	uint64_t max_ns;
	//! Fraction of samples above 1.5 refresh periods.
	double frac_over_1p5;
	bool truncated;
};

static inline void
comp_residual_window_add(struct comp_residual_window *w, uint64_t residual_ns)
{
	if (w == NULL) {
		return;
	}
	w->n++;
	w->sum_ns += residual_ns;
	if (residual_ns > w->max_ns) {
		w->max_ns = residual_ns;
	}
	if (w->kept < COMP_RESIDUAL_WINDOW_CAP) {
		const uint64_t us = residual_ns / 1000ULL;
		w->ns_us[w->kept++] = us > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)us;
	}
}

static inline int
comp_residual_window_cmp_u32(const void *a, const void *b)
{
	const uint32_t x = *(const uint32_t *)a;
	const uint32_t y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

//! Nearest-rank percentile of a sorted array, @p pct in 0..100.
static inline uint32_t
comp_residual_window_rank(const uint32_t *sorted, uint32_t count, uint32_t pct)
{
	if (count == 0) {
		return 0;
	}
	uint64_t rank = ((uint64_t)pct * count + 99) / 100; // ceil(pct/100 * count)
	if (rank < 1) {
		rank = 1;
	}
	return sorted[rank - 1];
}

/*!
 * Summarise the window against @p period_ns and reset it.
 *
 * @return stats with n == 0 when the window was empty.
 */
static inline struct comp_residual_stats
comp_residual_window_take(struct comp_residual_window *w, uint64_t period_ns)
{
	struct comp_residual_stats st = {0, 0.0, 0, 0, 0, 0.0, false};
	if (w == NULL || w->n == 0) {
		return st;
	}
	qsort(w->ns_us, w->kept, sizeof(w->ns_us[0]), comp_residual_window_cmp_u32);
	st.n = w->n;
	st.mean_ns = (double)w->sum_ns / (double)w->n;
	st.p50_ns = (uint64_t)comp_residual_window_rank(w->ns_us, w->kept, 50) * 1000ULL;
	st.p95_ns = (uint64_t)comp_residual_window_rank(w->ns_us, w->kept, 95) * 1000ULL;
	st.max_ns = w->max_ns;
	st.truncated = w->n > w->kept;
	if (period_ns != 0) {
		const uint64_t thr_us = (period_ns + period_ns / 2) / 1000ULL;
		uint32_t over = 0;
		for (uint32_t i = 0; i < w->kept; i++) {
			if (w->ns_us[i] > thr_us) {
				over++;
			}
		}
		st.frac_over_1p5 = w->kept != 0 ? (double)over / (double)w->kept : 0.0;
	}
	w->kept = 0;
	w->n = 0;
	w->sum_ns = 0;
	w->max_ns = 0;
	return st;
}

#ifdef __cplusplus
}
#endif
