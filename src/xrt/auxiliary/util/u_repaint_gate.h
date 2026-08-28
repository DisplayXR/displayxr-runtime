// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interval-aware quiet gate for the #868 repaint loops (#1257).
 *
 * Every native compositor's repaint loop must answer the same question
 * before replaying the last atlas: "is the app's next frame imminent?"
 * The original answer was a fixed constant — repaint only after the app
 * has been quiet for >= 2 panel periods — chosen so a repaint never
 * steals the compositor lock and the GPU from a submission that was
 * about to happen anyway (measured: a 46.7 fps app on a 60 Hz panel,
 * a 1.28-period interval, is about to submit and a repaint buys nothing).
 *
 * That constant is right for an app hovering near panel rate and wrong
 * for the case repaint exists for: a present-capped app. At 20 Hz on a
 * 60 Hz panel the 33 ms toll burns the first missed vblank of every app
 * frame and most of the idle window with it (measured ceiling ~1 repaint
 * per app frame instead of 2); at 30 Hz the interval is exactly 2 periods
 * and the gate NEVER opens — a repaint dead zone (#1257, hz30 witness:
 * repaints/s = 0.0).
 *
 * This helper keeps the constant's INTENT — never compete with an
 * imminent app frame — but derives "imminent" from the app's measured
 * cadence instead:
 *
 *  - Per app frame it maintains an EMA of the inter-frame interval and
 *    of its jitter (single writer: the commit path).
 *  - When the cadence is STABLE and genuinely slow (interval >= 1.5
 *    periods — the same threshold the margin below implies), the window
 *    opens after ONE period of quiet (a vblank was truly missed) and
 *    closes half a period before the predicted next app frame, which
 *    covers the replay's own duration plus the race the old constant
 *    guarded against. One repaint per panel period at most, so the
 *    window fills missed vblanks instead of bunching at tick rate.
 *  - With no stable measurement (startup, jittery app, mode switch,
 *    a >1 s pause) it degrades to the legacy 2-period behavior, byte
 *    for byte — a near-panel-rate app never sees a difference.
 *
 * DXR_WEAVE_REPAINT_GATE=legacy pins the old behavior for A/B runs.
 * DXR_WEAVE_REPAINT_FORCE=1 bypasses this gate entirely (unchanged —
 * the backends check it before consulting the gate).
 *
 * Threading: on_app_frame is called only from the frame path,
 * note_repaint only from the repaint thread, open reads both — the
 * same benign unlocked 64-bit reads the loops already did on
 * last_app_frame_ns.
 *
 * @ingroup aux_util
 */

#pragma once

#include "util/u_logging.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * State for one compositor's repaint quiet gate. Zero-init (calloc) is a
 * valid initial state.
 */
struct u_repaint_gate
{
	uint64_t last_app_frame_ns; //!< Stamp of the last REAL app frame.
	uint64_t last_repaint_ns;   //!< Stamp of the last repaint (adaptive spacing only).
	uint64_t interval_ema_ns;   //!< EMA of the app inter-frame interval.
	uint64_t jitter_ema_ns;     //!< EMA of |interval - interval_ema|.
	uint32_t samples;           //!< Interval samples since the last reset (capped).
	int mode;                   //!< 0 = unprobed, 1 = adaptive, 2 = legacy (env-pinned).
	bool adaptive_logged;       //!< One-shot "adaptive engaged" log.
};

//! Intervals above this are an app pause / startup gap, not cadence — restart measurement.
#define U_REPAINT_GATE_MAX_INTERVAL_NS (1000ull * 1000 * 1000)

//! Samples required before the measured cadence is trusted.
#define U_REPAINT_GATE_MIN_SAMPLES 8

/*!
 * Record a REAL app frame at @p now_ns. Call exactly where the loop's
 * last_app_frame_ns quiet-gate key is stamped (and nowhere else — a
 * repaint must never feed this, or repaints would pace off their own
 * timestamps).
 */
static inline void
u_repaint_gate_on_app_frame(struct u_repaint_gate *g, uint64_t now_ns)
{
	if (g->last_app_frame_ns != 0 && now_ns > g->last_app_frame_ns) {
		const uint64_t iv = now_ns - g->last_app_frame_ns;
		if (iv > U_REPAINT_GATE_MAX_INTERVAL_NS) {
			// Pause / startup / debugger stop: the cadence before it is stale.
			g->interval_ema_ns = 0;
			g->jitter_ema_ns = 0;
			g->samples = 0;
		} else if (g->interval_ema_ns == 0) {
			g->interval_ema_ns = iv;
			g->jitter_ema_ns = 0;
			g->samples = 1;
		} else {
			const uint64_t ema = g->interval_ema_ns;
			const uint64_t dev = iv > ema ? iv - ema : ema - iv;
			// Alpha 1/8: settles in ~1 s at 20 Hz, still tracks a cap change.
			g->interval_ema_ns = (uint64_t)((int64_t)ema + ((int64_t)iv - (int64_t)ema) / 8);
			g->jitter_ema_ns =
			    (uint64_t)((int64_t)g->jitter_ema_ns +
			               ((int64_t)dev - (int64_t)g->jitter_ema_ns) / 8);
			if (g->samples < 255) {
				g->samples++;
			}
		}
	}
	g->last_app_frame_ns = now_ns;
}

/*!
 * Record that a repaint fired at @p now_ns (adaptive spacing key).
 */
static inline void
u_repaint_gate_note_repaint(struct u_repaint_gate *g, uint64_t now_ns)
{
	g->last_repaint_ns = now_ns;
}

/*!
 * May a repaint fire at @p now_ns? Replaces the loop's
 * `quiet < period * 2` primary gate (the 1-period floor re-checked under
 * the lock stays with the caller). Never consulted under
 * DXR_WEAVE_REPAINT_FORCE=1 — the backends bypass it there, as before.
 */
static inline bool
u_repaint_gate_open(struct u_repaint_gate *g, uint64_t now_ns, uint64_t period_ns)
{
	if (g->mode == 0) {
		const char *e = getenv("DXR_WEAVE_REPAINT_GATE");
		g->mode = (e != NULL && strcmp(e, "legacy") == 0) ? 2 : 1;
		if (g->mode == 2) {
			U_LOG_W("#1257: DXR_WEAVE_REPAINT_GATE=legacy — fixed 2-period repaint gate "
			        "(present-capped apps will repaint below panel rate)");
		}
	}

	const uint64_t quiet_ns = now_ns - g->last_app_frame_ns;

	const uint64_t ema = g->interval_ema_ns;
	const bool stable = g->samples >= U_REPAINT_GATE_MIN_SAMPLES &&
	                    ema >= period_ns + period_ns / 2 && // genuinely slow: >= 1.5 periods
	                    g->jitter_ema_ns * 6 < ema;         // cadence is predictable

	if (g->mode == 2 || !stable) {
		// Legacy: only once the app has already missed a FULL refresh.
		return quiet_ns >= period_ns * 2;
	}

	// Adaptive: a vblank must actually have been missed...
	if (quiet_ns < period_ns) {
		return false;
	}
	// ...the predicted next app frame must be at least half a period out
	// (covers the replay's duration and the lock race the legacy constant
	// guarded against; also what makes 1.5 periods the engagement floor)...
	if (quiet_ns + period_ns / 2 >= ema) {
		return false;
	}
	// ...and at most one repaint per panel period, so a wide window fills
	// its missed vblanks instead of firing at tick rate.
	if (g->last_repaint_ns != 0 && now_ns > g->last_repaint_ns &&
	    now_ns - g->last_repaint_ns < (period_ns * 9) / 10) {
		return false;
	}

	if (!g->adaptive_logged) {
		g->adaptive_logged = true;
		U_LOG_W("#1257: repaint gate ADAPTIVE — app interval %.1f ms (jitter %.1f ms) is "
		        "stable and slow; repainting after 1 missed vblank instead of 2 "
		        "(DXR_WEAVE_REPAINT_GATE=legacy reverts)",
		        (double)ema / 1e6, (double)g->jitter_ema_ns / 1e6);
	}
	return true;
}

#ifdef __cplusplus
}
#endif
