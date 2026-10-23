// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_tracking's scripted viewer (pure).
 * @ingroup drv_sim_tracking
 */

#include "sim_tracking_script.h"

#include <math.h>
#include <stddef.h>

static bool
ieq(const char *a, const char *lower)
{
	for (size_t i = 0;; i++) {
		const char c = (char)((a[i] >= 'A' && a[i] <= 'Z') ? a[i] + ('a' - 'A') : a[i]);
		if (c != lower[i]) {
			return false;
		}
		if (c == '\0') {
			return true;
		}
	}
}

bool
sim_tracking_parse_script(const char *s, enum sim_tracking_script_kind *out)
{
	if (s == NULL || out == NULL) {
		return false;
	}
	if (ieq(s, "sweep")) {
		*out = SIM_TRACKING_SCRIPT_SWEEP;
		return true;
	}
	if (ieq(s, "offset")) {
		*out = SIM_TRACKING_SCRIPT_OFFSET;
		return true;
	}
	return false;
}

bool
sim_tracking_script_eval(
    const struct sim_tracking_script *s, int64_t t_ns, float left[3], float right[3], float head[3])
{
	const int64_t t = t_ns - s->epoch_ns;
	float c[3] = {s->nominal[0] + s->offset[0], s->nominal[1] + s->offset[1], s->nominal[2] + s->offset[2]};
	if (s->kind == SIM_TRACKING_SCRIPT_SWEEP && s->period_ns > 0) {
		const double phase = (double)(t % s->period_ns) / (double)s->period_ns;
		c[0] += s->amplitude_m * (float)sin(2.0 * 3.14159265358979323846 * phase);
	}
	const float half = s->ipd_m * 0.5f;
	left[0] = c[0] - half;
	left[1] = c[1];
	left[2] = c[2];
	right[0] = c[0] + half;
	right[1] = c[1];
	right[2] = c[2];
	if (head != NULL) {
		head[0] = c[0];
		head[1] = c[1];
		head[2] = c[2];
	}
	if (s->loss_period_ns > 0) {
		const int64_t m = ((t % s->loss_period_ns) + s->loss_period_ns) % s->loss_period_ns;
		return m < s->loss_period_ns / 2;
	}
	return true;
}
