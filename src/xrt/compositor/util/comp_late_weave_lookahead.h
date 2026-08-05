// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  App-visible wait_frame→scanout lookahead (#867).
 *
 * Split out of comp_weave_latency_win.h so the Vulkan target — which is
 * cross-platform and carries its own present_wait harness — can share the
 * formula without pulling DXGI in.
 *
 * @ingroup comp_util
 */

#pragma once

#include <stdint.h>

/*!
 * The interval from an `xrWaitFrame` return to the moment the frame the app is
 * about to render reaches photons.
 *
 * Every in-process compositor used to answer this with a hardcoded
 * `period × 2`. That constant is right in exactly one situation — an app
 * keeping up at queue depth 1, where it spends one refresh rendering and the
 * frame scans out one refresh after the weave. It goes wrong in both terms the
 * moment the pipeline leaves that point: the late-weave governor backing off
 * pushes scanout further out, and a backed-off app is by definition not
 * keeping up, so its own frame cost exceeds a refresh too. The app then
 * renders geometry and view poses for T while the frame lands at T + Δ.
 *
 * This is invisible to throughput metrics by construction: the display
 * processor re-predicts the *eyes* at weave time, so the weave itself stays
 * correct and fps/latency gates all pass. What goes stale is the projection
 * the app already baked into the frame.
 *
 * Both terms are measured rather than assumed — @p interval_ema_ns is the
 * observed frame-to-frame cost, @p measured_r_ns the weave→scanout residual
 * from present statistics or `vkWaitForPresentKHR`. In the healthy case the
 * sum evaluates to ≈2 periods on its own, reproducing the old constant without
 * special-casing it, which is what makes this safe to switch on by default.
 *
 * @return 0 when either term is unmeasured — the caller keeps its fallback.
 */
static inline uint64_t
late_weave_lookahead_ns(double interval_ema_ns, uint64_t measured_r_ns, double period_ns)
{
	if (measured_r_ns == 0 || interval_ema_ns <= 0.0 || period_ns <= 0.0) {
		return 0;
	}
	double lookahead = interval_ema_ns + (double)measured_r_ns;
	// A hitch must not throw the prediction far into the future, and a
	// bogusly small sample must not predict the past.
	const double lo = period_ns;
	const double hi = period_ns * 8.0;
	if (lookahead < lo) {
		lookahead = lo;
	} else if (lookahead > hi) {
		lookahead = hi;
	}
	return (uint64_t)lookahead;
}
