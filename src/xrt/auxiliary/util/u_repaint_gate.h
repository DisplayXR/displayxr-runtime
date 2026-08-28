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
 * imminent app frame — but reasons in VBLANK COUNTS over the FIFO
 * present queue instead of wall-clock margins (two measured rounds of
 * ms-domain mean±jitter prediction failed; see u_repaint_gate_cadence_n
 * and the #1257 issue thread):
 *
 *  - Per app frame it records the raw inter-frame interval in a small
 *    ring (single writer: the commit path). The cadence is "the app
 *    presents every N vblanks", estimated by a mode-majority /
 *    coherent-mean ladder over round(interval / period) — immune to
 *    vsync quantization, to displaced-commit pairs, and to the
 *    feature's own perturbation (see u_repaint_gate_cadence_n).
 *  - When the cadence is trusted at N == 2 (the measured ceiling — see
 *    the ledger note at the engagement site), the app frame gets a
 *    budget of one repaint filling the one missed vblank via the FIFO
 *    queue, opening at half a period of quiet and closing before the
 *    EARLIEST PLAUSIBLE next commit (a displaced commit arrives a
 *    vblank early). A repaint presented or lock-held past that lands
 *    where the app's next frame needs to be and costs the app a whole
 *    vblank (measured as weaves/s < presents/s + degraded cadence).
 *    N >= 3 deliberately falls back to the legacy gate — five schedule
 *    variants lost to it on hardware (#1257).
 *  - A closed-loop governor sheds budget by the ring's own slipped-
 *    interval count, so if repaints still displace app frames the
 *    feature backs itself off until the app holds its target rate —
 *    repaints that trade away app frames are worse than none.
 *  - If the app's predicted frame goes a full period overdue, the app
 *    is hitching, not pacing — the gate falls OPEN at panel-rate
 *    spacing (the original #868 use case; the steady window must never
 *    close it).
 *  - With no trusted cadence (startup, erratic app, a >1 s pause) it
 *    degrades to the legacy 2-period behavior, byte for byte.
 *
 * DXR_WEAVE_REPAINT_GATE=legacy pins the old behavior for A/B runs.
 * DXR_WEAVE_REPAINT_FORCE=1 bypasses this gate entirely (unchanged —
 * the backends check it before consulting the gate).
 * DXR_WEAVE_REPAINT_TRACE=1 emits the loop instrumentation below.
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
	uint64_t interval_ema_ns;   //!< EMA of the app inter-frame interval (TRACE readout only).
	uint64_t jitter_ema_ns;     //!< EMA of |interval - interval_ema| (TRACE readout only).
	uint64_t recent_iv_ns[16];  //!< Ring of raw intervals; the gate buckets these per tick.
	uint32_t samples;           //!< Interval samples since the last reset (capped).
	uint8_t iv_head;            //!< Ring write position.
	uint8_t fires_since_app;    //!< Repaints since the last app frame (budget = N-1).
	int mode;                   //!< 0 = unprobed, 1 = adaptive, 2 = legacy (env-pinned).
	bool adaptive_logged;       //!< One-shot "adaptive engaged" log.
};

//! Intervals above this are an app pause / startup gap, not cadence — restart measurement.
#define U_REPAINT_GATE_MAX_INTERVAL_NS (1000ull * 1000 * 1000)

//! Ring samples required before the measured cadence is trusted.
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
			g->iv_head = 0;
		} else {
			g->recent_iv_ns[g->iv_head] = iv;
			g->iv_head = (uint8_t)((g->iv_head + 1) % 16);
			if (g->samples < 255) {
				g->samples++;
			}
			// EMA/jitter are kept ONLY for the trace row; nothing gates
			// on them (see the vblank-count model in _open).
			if (g->interval_ema_ns == 0) {
				g->interval_ema_ns = iv;
			} else {
				const uint64_t ema = g->interval_ema_ns;
				const uint64_t dev = iv > ema ? iv - ema : ema - iv;
				g->interval_ema_ns =
				    (uint64_t)((int64_t)ema + ((int64_t)iv - (int64_t)ema) / 8);
				g->jitter_ema_ns =
				    (uint64_t)((int64_t)g->jitter_ema_ns +
				               ((int64_t)dev - (int64_t)g->jitter_ema_ns) / 8);
			}
		}
	}
	g->last_app_frame_ns = now_ns;
	g->fires_since_app = 0;
}

/*!
 * Record that a repaint fired at @p now_ns (adaptive spacing + budget key).
 */
static inline void
u_repaint_gate_note_repaint(struct u_repaint_gate *g, uint64_t now_ns)
{
	g->last_repaint_ns = now_ns;
	if (g->fires_since_app < 255) {
		g->fires_since_app++;
	}
}

/*!
 * The app's cadence in VBLANK COUNTS: "the app presents every N vblanks".
 * Returns 0 when there is no trusted cadence; otherwise N >= 1 with
 * @p out_votes / @p out_have reporting the mode's dominance (diagnostic).
 *
 * Why counts and not milliseconds: a present-capped app on a vsynced
 * present path is QUANTIZED — its intervals are multiples of the panel
 * period (measured at hz20: nominal 50 ms arriving as 33/50/67 mixes,
 * "jitter" EMA 12-23 ms on a metronomic app). Any ms-domain mean±jitter
 * model reads that as noise and strangles the window (#1257 round 2:
 * adaptive 3.3/s vs legacy 9.5/s).
 *
 * Two estimators, because the #1257 round-3 runs proved each fails alone:
 *
 *  - The MODE of round(interval / period). Clean when commits arrive on
 *    the true beat (hz30 measured 10-13/16), but a displaced commit
 *    produces a COMPENSATING PAIR (a 67 ms interval followed by a 33 ms
 *    one around a 50 ms beat), and enough pairs dilute the mode to a
 *    flapping plurality (hz20 measured 5-7/16, N flapping 2<->3, in
 *    ADAPTIVE AND LEGACY alike — so it is the commit stream itself, not
 *    the feature).
 *  - The MEAN over the ring. Displacement pairs cancel exactly (the late
 *    commit shortens the next interval), so the mean holds the true beat
 *    through the chaos that destroys the mode — but it drifts when the
 *    app's rate genuinely sags (hz30 measured mean 30-42 ms while the
 *    mode stayed a clean 2).
 *
 * So: a strong mode majority (>= 60%) wins; otherwise the mean-rounded N
 * is used when the mean sits coherently near a beat (within a third of a
 * period); otherwise there is no trusted cadence and the caller falls
 * back to the legacy gate.
 */
static inline uint32_t
u_repaint_gate_cadence_n(const struct u_repaint_gate *g,
                         uint64_t period_ns,
                         uint32_t *out_votes,
                         uint32_t *out_have)
{
	const uint32_t have = g->samples < 16 ? g->samples : 16;
	*out_votes = 0;
	*out_have = have;
	if (have < U_REPAINT_GATE_MIN_SAMPLES || period_ns == 0) {
		return 0;
	}
	uint32_t votes[10] = {0};
	uint64_t sum_ns = 0;
	for (uint32_t i = 0; i < have; i++) {
		const uint64_t iv = g->recent_iv_ns[i];
		sum_ns += iv;
		uint64_t n = (iv + period_ns / 2) / period_ns;
		if (n < 1) {
			n = 1;
		}
		if (n > 9) {
			n = 9;
		}
		votes[n]++;
	}
	uint32_t best_n = 1;
	for (uint32_t n = 2; n <= 9; n++) {
		if (votes[n] > votes[best_n]) {
			best_n = n;
		}
	}
	*out_votes = votes[best_n];

	// Strong majority: the commits are on the beat — trust the mode.
	if (votes[best_n] * 10 >= have * 6) {
		return best_n;
	}

	// Diluted mode: displacement pairs. The mean still holds the beat if
	// there is one — accept it only when it sits near a whole vblank count.
	const uint64_t mean_ns = sum_ns / have;
	uint64_t mean_n = (mean_ns + period_ns / 2) / period_ns;
	if (mean_n < 1) {
		mean_n = 1;
	}
	if (mean_n > 9) {
		mean_n = 9;
	}
	const uint64_t beat_ns = mean_n * period_ns;
	const uint64_t off_ns = mean_ns > beat_ns ? mean_ns - beat_ns : beat_ns - mean_ns;
	if (off_ns <= period_ns / 3) {
		return (uint32_t)mean_n;
	}

	return 0; // no trusted cadence
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
	const uint64_t since_rp_ns =
	    (g->last_repaint_ns != 0 && now_ns > g->last_repaint_ns) ? now_ns - g->last_repaint_ns
	                                                             : UINT64_MAX;

	/*
	 * Vblank-count model (see u_repaint_gate_cadence_n): the app presents
	 * every N panel periods. The window and budget follow from the FIFO
	 * present queue, not from wall-clock margins:
	 *
	 *  - The app's frame occupies one scanout slot; each repaint queued
	 *    behind it fills the NEXT slot. N-1 repaints fill the N-1 missed
	 *    vblanks exactly — a budget, not a rate.
	 *  - A repaint PRESENTED after (N-1) periods lands in the slot the
	 *    app's next frame needs, pushing the app out a whole vblank.
	 *    That queue theft is what round 2 measured as weaves/s <
	 *    presents/s AND as the inflated "jitter": the feature was
	 *    displacing the very cadence it was predicting. So the window
	 *    closes a quarter period (≈ one replay, measured 3.5-7.5 ms)
	 *    before that boundary, and the budget caps the queue depth.
	 */
	uint32_t votes = 0, have = 0;
	// Trust decision (mode-majority / coherent-mean ladder) lives inside
	// cadence_n — it returns 0 when there is no trusted cadence.
	const uint32_t n = u_repaint_gate_cadence_n(g, period_ns, &votes, &have);

	/*
	 * N == 2 ONLY — the measured ceiling (#1257, five schedule variants
	 * on hardware). At N == 2 the adaptive window is a clean win: hz30
	 * went from a structural dead zone (repaints/s exactly 0.0) to
	 * ~15-17/s with the app holding full rate. At N >= 3 every variant
	 * LOST to the legacy gate on both queue placements: the budget's
	 * multiple fires per gap collide with displaced-early commits, each
	 * ~5 ms lock hold vsync-snaps into a 16.7 ms app slip, the loop's
	 * own ticks starve on the convoy (56-95 ticks/s vs 149-163 in the
	 * same build's legacy run), and the shed/restore governor limit-
	 * cycles instead of converging (its ~1 s restore equals the ring
	 * window at the degraded rate). Two routes back into N >= 3, in
	 * order of promise: (a) PARTITION the vblank slots up front — app
	 * every Nth vblank, repaints the rest — so the app vsync-quantizes
	 * onto its own slots and the collision never exists by construction
	 * (this is why the FORCE probe succeeds where all five gap-filling
	 * schedules failed; independently measured twice: render-30/weave-60
	 * on Arc at -9.5 GPU pts, and Unity 15.6 app weaves + 44.3 repaints
	 * at -14.5 GPU pts with the display rate untouched); (b) shrink the
	 * replay's lock hold (partial replay / present outside the lock) so
	 * a collision stops costing a vblank. Not another gap-filling
	 * schedule. Full evidence chain: issue #1257.
	 */
	const bool engaged = g->mode != 2 && n == 2;

	if (!engaged) {
		// Legacy: only once the app has already missed a FULL refresh.
		return quiet_ns >= period_ns * 2;
	}

	/*
	 * Stall branch: the app's predicted frame is a full period overdue —
	 * it is hitching, not pacing. This is the case #868 exists for; the
	 * steady-state window below must not close it (v1/v2 silently did).
	 * Behaves like the legacy gate plus panel-rate spacing.
	 */
	if (quiet_ns >= (uint64_t)(n + 1) * period_ns) {
		return since_rp_ns >= (period_ns * 9) / 10;
	}

	/*
	 * Closed-loop budget governor (#1257 round 4). A replay's lock hold
	 * that overlaps an app commit costs a FULL vblank, not the hold: the
	 * commit waits ~5 ms, misses its vblank, and vsync snaps the slip to
	 * 16.7 ms. Measured: the first build whose adaptive schedule really
	 * ran at hz20 degraded the app from a 50 ms to a true ~60 ms cadence
	 * (weaves/s 20 -> 16.6) — repaints that displace app frames make the
	 * panel fresher but the CONTENT staler, strictly worse than doing
	 * nothing. The ring already records the damage as slipped intervals
	 * (>= N+0.5 beats), so the budget sheds one repaint per three slips
	 * in the last 16 app frames and restores itself as the ring cleans
	 * (~1 s). The acceptance pair this enforces: repaints up AND app
	 * weave rate at target.
	 */
	uint32_t slips = 0;
	const uint64_t slip_floor_ns = (uint64_t)n * period_ns + period_ns / 2;
	const uint32_t ring_have = g->samples < 16 ? g->samples : 16;
	for (uint32_t i = 0; i < ring_have; i++) {
		if (g->recent_iv_ns[i] >= slip_floor_ns) {
			slips++;
		}
	}
	const uint32_t shed = slips / 3;
	const uint32_t budget = (n - 1) > shed ? (n - 1) - shed : 0;

	// Steady state: the governed budget of repaints per app frame...
	if (g->fires_since_app >= budget) {
		return false;
	}
	// ...not so early the eye pose would be staler than useful...
	if (quiet_ns < period_ns / 2) {
		return false;
	}
	/*
	 * ...presented clear of the EARLIEST PLAUSIBLE next commit, not the
	 * nominal one. Displaced-commit pairs mean a commit can arrive a
	 * whole vblank early — at (N-1) periods — and a fire started just
	 * under a boundary computed for the nominal beat still holds the
	 * lock when that early commit lands (round 4's hz20 regression). So
	 * the last fire must END before (N-1) periods: close half a period
	 * before it (hold measured 3.5-7.5 ms). N=2's window is already a
	 * sliver ending at the first missed vblank; it keeps the quarter-
	 * period margin (hz30 measured healthy with it) and the governor
	 * guards the residual.
	 */
	const uint64_t close_margin_ns = (n == 2) ? period_ns / 4 : period_ns / 2;
	if (quiet_ns + close_margin_ns >= (uint64_t)(n - 1) * period_ns) {
		return false;
	}
	// ...and spaced ~a period apart so the queue fills one slot per vblank.
	if (since_rp_ns < (period_ns * 9) / 10) {
		return false;
	}

	if (!g->adaptive_logged) {
		g->adaptive_logged = true;
		U_LOG_W("#1257: repaint gate ADAPTIVE — app presents every %u vblanks "
		        "(%u/%u votes); budget %u repaint(s) per app frame, presented clear of "
		        "the app's own queue slot (DXR_WEAVE_REPAINT_GATE=legacy reverts)",
		        n, votes, have, n - 1);
	}
	return true;
}

/*
 *
 * Trace — DXR_WEAVE_REPAINT_TRACE=1 (#1257 verification instrumentation).
 *
 * The witness says how many repaints landed; it cannot say where the missing
 * ones went. This answers that with one WARN row per ~5 s per loop: the
 * loop's REAL tick cadence (sleep-granularity theory), how long a replay
 * holds the lock (present-block / GPU-contention theory), how long the pace
 * call blocks, and which gate ate the bailed ticks. Off by default; a probe,
 * never a default (per docs/reference/debug-logging.md the row is throttled
 * far below per-frame).
 *
 */

struct u_repaint_trace
{
	int enabled; //!< 0 = unprobed (zero-init), then 1 = on / 2 = off from DXR_WEAVE_REPAINT_TRACE.
	uint64_t last_tick_ns;
	uint64_t tick_iv_ema_ns; //!< EMA of loop iteration spacing.
	uint64_t fire_ema_ns;    //!< EMA of replay duration (lock -> present done).
	uint64_t pace_ema_ns;    //!< EMA of the repaint_pace call's duration.
	uint64_t last_report_ns;
	uint32_t ticks, fires, bail_armed, bail_gate, bail_race;
};

static inline bool
u_repaint_trace_enabled(struct u_repaint_trace *t)
{
	if (t->enabled == 0) {
		const char *e = getenv("DXR_WEAVE_REPAINT_TRACE");
		t->enabled = (e != NULL && e[0] == '1') ? 1 : 2;
	}
	return t->enabled == 1;
}

//! Call once per loop iteration, before the gates.
static inline void
u_repaint_trace_tick(struct u_repaint_trace *t, uint64_t now_ns)
{
	if (!u_repaint_trace_enabled(t)) {
		return;
	}
	if (t->last_tick_ns != 0 && now_ns > t->last_tick_ns) {
		const uint64_t iv = now_ns - t->last_tick_ns;
		t->tick_iv_ema_ns =
		    t->tick_iv_ema_ns == 0
		        ? iv
		        : (uint64_t)((int64_t)t->tick_iv_ema_ns +
		                     ((int64_t)iv - (int64_t)t->tick_iv_ema_ns) / 8);
	}
	t->last_tick_ns = now_ns;
	t->ticks++;
}

static inline void
u_repaint_trace_bail_armed(struct u_repaint_trace *t)
{
	t->bail_armed++;
}

static inline void
u_repaint_trace_bail_gate(struct u_repaint_trace *t)
{
	t->bail_gate++;
}

static inline void
u_repaint_trace_bail_race(struct u_repaint_trace *t)
{
	t->bail_race++;
}

static inline void
u_repaint_trace_pace(struct u_repaint_trace *t, uint64_t start_ns, uint64_t end_ns)
{
	if (t->enabled != 1 || end_ns <= start_ns) {
		return;
	}
	const uint64_t d = end_ns - start_ns;
	t->pace_ema_ns = t->pace_ema_ns == 0
	                     ? d
	                     : (uint64_t)((int64_t)t->pace_ema_ns +
	                                  ((int64_t)d - (int64_t)t->pace_ema_ns) / 8);
}

//! Call after a replay completes; start = before taking the lock.
static inline void
u_repaint_trace_fire(struct u_repaint_trace *t, uint64_t start_ns, uint64_t end_ns)
{
	if (t->enabled != 1) {
		return;
	}
	t->fires++;
	if (end_ns > start_ns) {
		const uint64_t d = end_ns - start_ns;
		t->fire_ema_ns = t->fire_ema_ns == 0
		                     ? d
		                     : (uint64_t)((int64_t)t->fire_ema_ns +
		                                  ((int64_t)d - (int64_t)t->fire_ema_ns) / 8);
	}
}

//! Call once per loop iteration; emits one row per ~5 s.
static inline void
u_repaint_trace_report(struct u_repaint_trace *t,
                       uint64_t now_ns,
                       const char *site,
                       const struct u_repaint_gate *g,
                       uint64_t period_ns)
{
	if (t->enabled != 1) {
		return;
	}
	if (t->last_report_ns == 0) {
		t->last_report_ns = now_ns;
		return;
	}
	const uint64_t elapsed = now_ns - t->last_report_ns;
	if (elapsed < 5ull * 1000 * 1000 * 1000) {
		return;
	}
	const double secs = (double)elapsed / 1e9;
	uint32_t votes = 0, have = 0;
	const uint32_t n = u_repaint_gate_cadence_n(g, period_ns, &votes, &have);
	uint32_t slips = 0, budget = 0;
	if (n >= 2 && period_ns > 0) {
		const uint64_t slip_floor_ns = (uint64_t)n * period_ns + period_ns / 2;
		for (uint32_t i = 0; i < have; i++) {
			if (g->recent_iv_ns[i] >= slip_floor_ns) {
				slips++;
			}
		}
		const uint32_t shed = slips / 3;
		budget = (n - 1) > shed ? (n - 1) - shed : 0;
	}
	U_LOG_W("#1257 trace site=%s: ticks/s=%.1f fires/s=%.1f tick_iv=%.2fms fire=%.2fms "
	        "pace=%.2fms bail{armed=%u gate=%u race=%u} gate{mode=%s N=%u votes=%u/%u "
	        "slips=%u budget=%u ema=%.1fms jit=%.1fms samples=%u}",
	        site, (double)t->ticks / secs, (double)t->fires / secs,
	        (double)t->tick_iv_ema_ns / 1e6, (double)t->fire_ema_ns / 1e6,
	        (double)t->pace_ema_ns / 1e6, t->bail_armed, t->bail_gate, t->bail_race,
	        // Three states so a reader isn't puzzled by "adaptive" rows with
	        // legacy numbers: env-pinned legacy, engaged (N==2), or the
	        // deliberate N!=2 fallback.
	        g->mode == 2 ? "legacy" : (n == 2 ? "adaptive" : "adaptive-fallback"), n, votes,
	        have, slips, budget, (double)g->interval_ema_ns / 1e6,
	        (double)g->jitter_ema_ns / 1e6, g->samples);
	t->ticks = 0;
	t->fires = 0;
	t->bail_armed = 0;
	t->bail_gate = 0;
	t->bail_race = 0;
	t->last_report_ns = now_ns;
}

#ifdef __cplusplus
}
#endif
