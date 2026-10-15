// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's synthetic viewer evaluated at a target time
 *         (ADR-054 §4.1 / N6 Part 1, #2144). See the header.
 * @ingroup drv_sim_display
 */

#include "sim_display_eyes.h"

#include <math.h>
#include <string.h>

// Not M_PI: MSVC defines it only under _USE_MATH_DEFINES, strict C11 never.
#define SIM_EYES_TWO_PI 6.28318530717958647692

void
sim_fake_eyes_at(const struct sim_fake_eyes_params *p,
                 uint32_t view_count,
                 int64_t target_ns,
                 struct xrt_eye_positions *out)
{
	float cx = p->nominal_x_m;
	float cy = p->nominal_y_m;
	const float cz = p->nominal_z_m;
	if (p->sway_m != 0.0f && p->sway_period_ns > 0) {
		// Phase from the target time directly: deterministic, no clock.
		const double phase =
		    SIM_EYES_TWO_PI * (double)(target_ns % p->sway_period_ns) / (double)p->sway_period_ns;
		cx += p->sway_m * (float)sin(phase);
		cy += 0.5f * p->sway_m * (float)sin(2.0 * phase);
	}
	const float half_ipd = p->ipd_m / 2.0f;

	memset(out, 0, sizeof(*out));
	if (view_count == 1) {
		out->eyes[0] = (struct xrt_eye_position){cx, cy, cz};
		out->count = 1;
	} else if (view_count >= 4) {
		out->eyes[0] = (struct xrt_eye_position){cx - half_ipd, cy - 0.032f, cz};
		out->eyes[1] = (struct xrt_eye_position){cx + half_ipd, cy - 0.032f, cz};
		out->eyes[2] = (struct xrt_eye_position){cx - half_ipd, cy + 0.032f, cz};
		out->eyes[3] = (struct xrt_eye_position){cx + half_ipd, cy + 0.032f, cz};
		out->count = 4;
	} else {
		out->eyes[0] = (struct xrt_eye_position){cx - half_ipd, cy, cz};
		out->eyes[1] = (struct xrt_eye_position){cx + half_ipd, cy, cz};
		out->count = 2;
	}
	out->timestamp_ns = target_ns;
	out->valid = true;
}

bool
sim_fake_tracking_at(int64_t period_ms, int64_t target_ns, uint64_t *out_edges)
{
	if (out_edges != NULL) {
		*out_edges = 0;
	}
	if (period_ms <= 0) {
		return true; // No period: always tracking.
	}
	// Square wave: tracking for one period, lost for the next. Every
	// half-period boundary is one edge.
	const int64_t t_ms = target_ns / 1000000;
	const int64_t half_periods = t_ms / period_ms;
	if (out_edges != NULL) {
		*out_edges = (uint64_t)half_periods;
	}
	return (half_periods % 2) == 0;
}
