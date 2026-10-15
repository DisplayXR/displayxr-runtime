// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's synthetic viewer: the nominal eye layout plus a
 *         deterministic fake-tracking sway, evaluated at a TARGET time
 *         (ADR-054 §4.1 / N6 Part 1, #2144).
 * @ingroup drv_sim_display
 *
 * Pure (no settings, no clock, no logging) so tests/tests_sim_eyes.cpp can
 * pin it host-side; sim_display_device.c wraps it with the knobs
 * (`TRACKING_SWAY_MM`, `TRACKING_SWAY_PERIOD_MS`) and the webcam override.
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_display_metrics.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The synthetic viewer. @ref sway_m == 0 (the default) is the static nominal
 * viewer sim_display always reported; a non-zero sway moves the viewer on a
 * closed Lissajous path (x: one period, y: two, half the amplitude) so a
 * target-time query at t and t + 16 ms returns a different, predictable pair.
 */
struct sim_fake_eyes_params
{
	float nominal_x_m;
	float nominal_y_m;
	float nominal_z_m;
	float ipd_m;
	float sway_m;           //!< Horizontal sway amplitude (metres); 0 = static.
	int64_t sway_period_ns; //!< Period of the sway; ≤ 0 = static.
};

/*!
 * The viewer's eyes at @p target_ns in sim_display's N-view layout: 1 = the
 * midpoint, 2 = the pair, ≥ 4 = the pair 32 mm below and above. Sets
 * `eyes`, `count`, `timestamp_ns` (= @p target_ns: the synthetic sample is
 * evaluated exactly there) and `valid`; `is_tracking` is the caller's.
 */
void
sim_fake_eyes_at(const struct sim_fake_eyes_params *p,
                 uint32_t view_count,
                 int64_t target_ns,
                 struct xrt_eye_positions *out);

/*!
 * The fake tracker's square-wave `is_tracking` at @p target_ns: always true
 * with @p period_ms ≤ 0, else tracking for one period, lost for the next.
 * @param[out] out_edges  Elapsed half-periods (0 without a period); may be NULL.
 */
bool
sim_fake_tracking_at(int64_t period_ms, int64_t target_ns, uint64_t *out_edges);

#ifdef __cplusplus
}
#endif
