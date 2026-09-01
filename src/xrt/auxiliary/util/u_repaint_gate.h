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
 * DXR_WEAVE_REPAINT_GATE=adaptive opts in to the N == 2 window — it is
 * NOT the default: its intermittent engagement failed the hz30 visual
 * check (panel cadence oscillating 28-50 updates/s reads as judder
 * where a steady 33 does not; the eye grades cadence stability, not
 * average rate). Default behavior is the legacy schedule.
 *
 * DXR_WEAVE_REPAINT_GATE=fill is the ANDROID DEFAULT (#1257 NP02J
 * measurement, see the engagement site below): the legacy gate opening
 * at two periods, plus a fire that itself blocks 9-18 ms in the FIFO
 * acquire, deterministically yields exactly ONE repaint per app frame at
 * phase ~0.70 — the panel then alternates ~41/18 ms (interval CoV ~40%)
 * on a panel provably pinned at 59.86 Hz. Fill schedules against the
 * MEASURED interval EMA instead of a vblank count, and is scoped to the
 * apps for which that EMA is trustworthy. Windows/macOS/Linux keep the
 * legacy default; fill is opt-in there via the env var.
 * DXR_WEAVE_REPAINT_FORCE=1 bypasses this gate entirely (unchanged —
 * the backends check it before consulting the gate).
 * DXR_WEAVE_REPAINT_TRACE=1 emits the loop instrumentation below.
 *
 * On Android getenv() reaches nothing, so all three also read a
 * sysprop fallback carrying the same values — debug.dxr.weave_repaint_gate,
 * debug.dxr.weave_repaint_trace, debug.dxr.weave_repaint_force. The
 * environment variable always wins when it is set and non-empty.
 *
 * Threading: on_app_frame is called only from the frame path,
 * note_repaint only from the repaint thread, open reads both — the
 * same benign unlocked 64-bit reads the loops already did on
 * last_app_frame_ns.
 *
 * @ingroup aux_util
 */

#pragma once

#include "util/u_app_partition.h"
#include "util/u_logging.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_ANDROID
#include <sys/system_properties.h>
#endif

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
	uint64_t interval_ema_ns;   //!< EMA of the app inter-frame interval (fill mode + TRACE).
	uint64_t jitter_ema_ns;     //!< EMA of |interval - interval_ema| (fill-mode trust key + TRACE).
	uint64_t recent_iv_ns[16];  //!< Ring of raw intervals; the gate buckets these per tick.
	uint32_t samples;           //!< Interval samples since the last reset (capped).
	uint8_t iv_head;            //!< Ring write position.
	uint8_t fires_since_app;    //!< Repaints since the last app frame (budget = N-1).
	int mode;                   //!< 0 = unprobed, 1 = adaptive, 2 = legacy (env-pinned), 3 = default, 4 = fill.
	bool adaptive_logged;       //!< One-shot "adaptive/fill engaged" log.
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
			// EMA/jitter feed the FILL schedule (mode 4) and the trace
			// row. The vblank-count model in _open (legacy/adaptive/
			// partition) still gates on counts, not on these — the ms
			// domain is only trustworthy for a GPU-bound metronomic app,
			// which is exactly the scope fill checks jitter_ema for.
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
 * Is the FILL schedule (mode 4) trustworthy right now?
 *
 * Fill schedules in the MILLISECOND domain off interval_ema_ns, and that
 * domain is only honest for a GPU-bound metronomic app — one whose frame
 * time is set by its own render cost, so its intervals are a smooth
 * distribution around a mean. A PRESENT-CAPPED app is vsync-QUANTIZED
 * instead: its intervals are multiples of the panel period (hz20
 * measured as a 33/50/67 mix), which reads as a jitter EMA of 12-23 ms
 * on an app that is in fact perfectly regular. Feeding that into a
 * ms-domain schedule is the failure the vblank-count model exists to
 * avoid (#1257 round 2), so the jitter <= period/4 test is the scope
 * fence: above it, fill declines and the caller keeps the vblank-count /
 * legacy path unchanged.
 */
static inline bool
u_repaint_gate_fill_ok(const struct u_repaint_gate *g, uint64_t period_ns)
{
	return g->mode == 4 &&                                    //
	       g->samples >= U_REPAINT_GATE_MIN_SAMPLES &&        //
	       period_ns != 0 &&                                  //
	       g->interval_ema_ns >= period_ns &&                 //
	       g->interval_ema_ns <= U_REPAINT_GATE_MAX_INTERVAL_NS && //
	       g->jitter_ema_ns <= period_ns / 4;
}

/*!
 * Fill mode's governed budget: how many repaints one app interval has
 * room for. Single source for the gate and the trace row — printing a
 * separately-derived budget is what twice sent the #1257 verification
 * chasing a shed that only existed in the log.
 *
 * Each fire's present lands on the next free vblank, so fires+1 presents
 * (the app's own frame plus its fills) must all fit strictly inside the
 * app interval with half a period of guard, or the last repaint steals
 * the slot the app's next frame needs. Hence
 * budget = floor((interval_ema - period/2) / period) - shed:
 * a 35 ms interval (the known-good 59.9 fps config) yields 1, a 58 ms
 * one yields 2.
 *
 * The governor is the #1257 round-4 one, re-floored for the ms domain:
 * an interval half a period past the EMA is a slip, and one repaint is
 * shed per three slips in the last 16 app frames.
 */
static inline uint32_t
u_repaint_gate_fill_budget(const struct u_repaint_gate *g,
                           uint64_t period_ns,
                           uint32_t *out_slips,
                           uint32_t *out_shed)
{
	uint32_t slips = 0;
	const uint64_t slip_floor_ns = g->interval_ema_ns + period_ns / 2;
	const uint32_t have = g->samples < 16 ? g->samples : 16;
	for (uint32_t i = 0; i < have; i++) {
		if (g->recent_iv_ns[i] >= slip_floor_ns) {
			slips++;
		}
	}
	const uint32_t shed = slips / 3;
	uint32_t budget = 0;
	// Unsigned domain: interval_ema >= period > period/2 is already
	// established by _fill_ok, but this is a header compiled by MSVC and
	// mingw too — never let the subtraction wrap on a caller that skipped
	// the trust check (the trace row can).
	if (period_ns != 0 && g->interval_ema_ns > period_ns / 2) {
		const uint64_t fits = (g->interval_ema_ns - period_ns / 2) / period_ns;
		budget = fits > (uint64_t)shed ? (uint32_t)(fits - (uint64_t)shed) : 0;
	}
	*out_slips = slips;
	*out_shed = shed;
	return budget;
}

/*!
 * May a repaint fire at @p now_ns? Replaces the loop's
 * `quiet < period * 2` primary gate (the re-check under the lock stays
 * with the caller). Never consulted under DXR_WEAVE_REPAINT_FORCE=1 —
 * the backends bypass it there, as before.
 *
 * @p ps is the compositor's partition throttle state (may be NULL on a
 * path with no throttle); when the partition is active it carries the
 * RELEASE GRID, and the fill is scheduled absolutely against it — see
 * the grid branch below.
 */
static inline bool
u_repaint_gate_open(struct u_repaint_gate *g,
                    uint64_t now_ns,
                    uint64_t period_ns,
                    const struct u_app_partition *ps)
{
	if (g->mode == 0) {
		/*
		 * Default is the LEGACY schedule. The N=2 adaptive window won its
		 * perf numbers (hz30: 0 -> ~15 repaints/s) but FAILED the human
		 * eyeball: the trust ladder engages intermittently, the panel rate
		 * breathes 28-50 updates/s, and an oscillating cadence reads as
		 * judder where a steady 33 does not — the eye grades cadence
		 * STABILITY, not average rate (#1257 hz30 visual verdict). Both
		 * schedules humans liked (steady legacy, FORCE partition) were
		 * steady. Adaptive stays opt-in until either engagement hysteresis
		 * proves a steady win or the slot-partition mode replaces it.
		 */
		const char *e = getenv("DXR_WEAVE_REPAINT_GATE");
#ifdef XRT_OS_ANDROID
		// getenv reaches nothing on Android; the schedule selector would
		// be permanently unreachable there without this. Env still wins,
		// and the prop carries the same "adaptive"/"legacy"/"fill" strings.
		char sp_gate[PROP_VALUE_MAX] = {0};
		if ((e == NULL || e[0] == '\0') &&
		    __system_property_get("debug.dxr.weave_repaint_gate", sp_gate) > 0) {
			e = sp_gate;
		}
#endif
		if (e != NULL && strcmp(e, "legacy") == 0) {
			// Checked FIRST: explicit legacy outranks everything,
			// including the partition fill and the Android default.
			g->mode = 2;
		} else if (e != NULL && strcmp(e, "adaptive") == 0) {
			g->mode = 1;
			U_LOG_W("#1257: DXR_WEAVE_REPAINT_GATE=adaptive — EXPERIMENTAL N=2 window; "
			        "its intermittent engagement failed the hz30 visual check "
			        "(oscillating panel cadence reads as judder), default is legacy");
		} else if (e != NULL && strcmp(e, "fill") == 0) {
			g->mode = 4;
		} else {
			// DEFAULT IS NEVER FILL. Fill was briefly the Android default and
			// it REGRESSED — see the header comment on mode 4. Opt-in only.
			g->mode = 3; // default: legacy schedule unless the partition engages
		}
		if (g->mode == 4) {
			U_LOG_W("#1257: repaint gate FILL — OPT-IN AND MEASURED HARMFUL on the "
			        "in-process Android tier. Repaints and app frames are SERIALISED on "
			        "one thread there (#1196 single weave ownership: the repaint thread "
			        "is also the app's weave server), so a budget above 1 starves the "
			        "hand-off: measured app frame delivery stopping ~10 s in, "
			        "xrBeginFrame returning XR_FRAME_DISCARDED at ~12k/s, and a panel "
			        "that looks SMOOTHER only because it is re-weaving a frozen frame. "
			        "Use only where repaints have their own weave thread");
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
	/*
	 * Engagement, in order of authority:
	 *
	 * 1. PARTITION (DXR_APP_FRAME_DIVISOR >= 2, u_app_partition.h): the
	 *    RUNTIME paces the app to every Dth vblank via xrWaitFrame, so
	 *    N = D is a known schedule, not an estimate — no cadence
	 *    flapping (the hz30 visual verdict: the eye grades cadence
	 *    STABILITY, not average rate), and commits are phase-locked so
	 *    the fire/commit collision that sank five gap-filling schedules
	 *    (#1257: each ~5 ms lock hold vsync-snapping into a 16.7 ms app
	 *    slip, tick starvation on the convoy, a limit-cycling governor)
	 *    never exists by construction. This is the productized form of
	 *    what the FORCE probe demonstrated (Arc render-30/weave-60 at
	 *    -9.5 GPU pts "really crisp"; Unity at -14.5 GPU pts).
	 * 2. DXR_WEAVE_REPAINT_GATE=adaptive: the EXPERIMENTAL estimated
	 *    N == 2 window (kept for engagement-hysteresis experiments).
	 * 3. Default: the legacy schedule.
	 *
	 * Explicit DXR_WEAVE_REPAINT_GATE=legacy outranks everything.
	 */
	const uint32_t part = u_app_partition_divisor();
	// part_on requires the throttle to have actually ENGAGED (next_release
	// set): on a tier where the throttle refused, the app runs unthrottled
	// at panel rate and the partition fill must stay out of the way —
	// single point of tier control, in the throttle.
	const bool part_on =
	    part >= 2 && g->mode != 2 && ps != NULL && ps->next_release_ns != 0;

	/*
	 * FILL (mode 4) — the measured Android default. The partition still
	 * outranks it: a KNOWN schedule beats an estimated one, so when
	 * part_on the engaged path below runs unchanged.
	 *
	 * Why a second schedule at all. On the NP02J the loss is not variance
	 * and not instability — it is placement, and it is DETERMINISTIC:
	 *
	 *   - The legacy gate opens at quiet >= 2 * period, i.e. ~33.4 ms into
	 *     a ~58 ms app interval.
	 *   - The one fire it then permits blocks 9-18 ms inside the FIFO
	 *     acquire (trace fire_ema), completing right as the next app frame
	 *     lands — so no open tick ever survives to fire again.
	 *
	 * The result is exactly 1:1 repaints:app-frames at phase 0.70, the
	 * on-screen stream alternating ~41/18 ms (panel interval CoV ~40%)
	 * while the panel is provably pinned at 59.86 Hz and the app is
	 * metronomic (interval EMA 50-59 ms, jitter EMA 1-2 ms). The trace
	 * carries all the losses on bail_gate (~600-710 per 5 s), with
	 * bail_armed = 0 after startup, bail_race = 0 and pace = 0: nothing is
	 * racing or blocking, the gate is simply shut.
	 *
	 * So fill budgets off the MEASURED interval rather than a vblank
	 * count, and fires as many repaints as the interval has room for: at a
	 * 58 ms interval two fills put presents at ~16.7 / 33.4 / 50.1 / 66.8
	 * ms and the panel runs ~60 from an app running ~17.
	 *
	 * Scope fence (u_repaint_gate_fill_ok): a vsync-quantized app has a
	 * jitter EMA far above period/4 and keeps the vblank-count/legacy path
	 * untouched. Anything that fails the trust test falls through to the
	 * EXACT prior behavior below.
	 */
	/*
	 * MODE 4 — "fill". OPT-IN ONLY; it was the Android default for one commit
	 * and that was WRONG. Read this before enabling it anywhere.
	 *
	 * The budget arithmetic below is correct and does what it says: at a
	 * 62 ms app interval it authorises 3 repaints, at 45 ms it authorises 2,
	 * and the fires land. What the arithmetic ASSUMES is that a repaint and an
	 * app frame are independent work. On the in-process Android tier they are
	 * not: #1196 single weave ownership makes the repaint thread the app's
	 * weave server too (it serves weave_hand.pending), so every authorised
	 * repaint is time the app's posted frame cannot be served.
	 *
	 * Measured consequence, NP02J, panel pinned 59.86 Hz:
	 *   - app frame delivery stops ~10 s after launch (census app n=0 from the
	 *     second report onward, gate EMA frozen at samples=39)
	 *   - xrBeginFrame returns XR_FRAME_DISCARDED continuously, ~12k/s
	 *   - the panel reads BETTER — 39.7 fps / CoV 33% against legacy's
	 *     34.4 / 47% — because it is re-weaving a STALE frame. The metric
	 *     improved while the product broke.
	 *
	 * FULL CHAIN, traced afterwards — the runtime's part is far smaller than
 * the outcome suggests, and the distinction matters to anyone reviving it:
 *
 *   1. Fill authorises extra fires. Each takes c->mutex, so it can delay
 *      one xrEndFrame past the two-wait window.
 *   2. oxr_session.c then returns XR_FRAME_DISCARDED for that ONE frame.
 *      That is a SUCCESS code and a normal, recoverable event.
 *   3. The app under test treats it as failure — `if (res != XR_SUCCESS)
 *      { return false; }` — and returns WITHOUT calling xrEndFrame
 *      (displayxr-demo-modelviewer android/src/main/cpp/main.cpp:1087).
 *      sess->frame_started never clears, so EVERY subsequent xrBeginFrame
 *      discards. One recoverable discard latches into a permanent trap,
 *      and the ~12k/s "spam" is that early return logging itself.
 *      Re-verified on a known-good matched pair: fill selected, census
 *      app n=0 across every report, 2.5M discards, and the app process
 *      ultimately DIES rather than merely stalling.
 *
 * So fill's actual defect is only step 1: it can cause a discard. Steps
 * 2-3 are conformant runtime behaviour meeting a non-conformant app. A
 * spec-compliant app would drop one frame and continue — which is a
 * HYPOTHESIS about fill's viability, not a result. No such app has been
 * measured here.
 *
 * That last line is the reason this comment is long. Panel-interval
	 * statistics cannot distinguish a well-paced live stream from a
	 * well-paced frozen one; only the per-kind census (app n) can, and any
	 * future scheduling change here must be graded on it.
	 *
	 * This is the sixth member of the #1257 fire/commit collision family. The
	 * five gap-filling variants before it failed for the same underlying
	 * reason on Windows tiers; the fix that worked there — the slot partition
	 * — works by giving the app a guaranteed release slot rather than by
	 * authorising more fires.
	 */
	if (!part_on && u_repaint_gate_fill_ok(g, period_ns)) {
		// STALL: the app is hitching, not pacing — the original #868 case.
		// Same treatment as the engaged stall branch: panel-rate fill.
		if (quiet_ns >= g->interval_ema_ns + period_ns) {
			return since_rp_ns >= (period_ns * 9) / 10;
		}

		uint32_t fill_slips = 0, fill_shed = 0;
		const uint32_t fill_budget =
		    u_repaint_gate_fill_budget(g, period_ns, &fill_slips, &fill_shed);

		/*
		 * BUDGET. Each fire's present lands on the next free vblank, so
		 * this fire plus every fire already spent plus the app's own
		 * frame must all fit strictly inside the app interval with half
		 * a period of guard — otherwise the last repaint steals the slot
		 * the app's next frame needs (the queue theft the #1257 rounds
		 * measured as weaves/s < presents/s). interval_ema >= period is
		 * established by _fill_ok, and period/2 < period, so the
		 * subtraction cannot wrap; the explicit test keeps that true for
		 * any future caller.
		 */
		const uint64_t want_ns =
		    ((uint64_t)g->fires_since_app + 1u + (uint64_t)fill_shed) * period_ns;
		if (g->interval_ema_ns <= period_ns / 2 ||
		    want_ns > g->interval_ema_ns - period_ns / 2) {
			return false;
		}
		// OPEN: not so early the replayed eye pose is staler than useful.
		if (quiet_ns < period_ns / 2) {
			return false;
		}
		// SPACING: coarse on purpose — the FIFO acquire does the fine
		// pacing (it is what blocks 9-18 ms per fire), so a tight spacing
		// here would only re-close the window it is trying to open.
		if (since_rp_ns < (period_ns * 3) / 4) {
			return false;
		}
		if (!g->adaptive_logged) {
			g->adaptive_logged = true;
			U_LOG_W("#1257 fill: app interval EMA %.2f ms (jitter %.2f ms) at a %.2f ms "
			        "panel period — budget %u repaint(s) per app frame filling the "
			        "remaining vblank slots (DXR_WEAVE_REPAINT_GATE=legacy reverts)",
			        (double)g->interval_ema_ns / 1e6, (double)g->jitter_ema_ns / 1e6,
			        (double)period_ns / 1e6, fill_budget);
		}
		return true;
	}

	/*
	 * COMMIT-RELATIVE fill under the partition — deliberately, and the
	 * lesson is load-bearing (#1257 partition v3 post-mortem): an
	 * absolute release-grid fill was tried and it regressed EVERY tier,
	 * including the four-for-four bridge config, with a slow breathing
	 * pattern (presents climbing 48 -> 57.5 across windows). The grid
	 * ran on the monotonic clock times the NOMINAL refresh rate, which
	 * is open-loop against the real vsync — a 59.94 Hz panel walks such
	 * a grid a full period every ~16 s, beating the fill schedule
	 * against the scanout. Commit-relative scheduling is vsync-LOCKED
	 * for free: the app's present blocks on vsync, so its commit times
	 * inherit the true vblank phase and every window below inherits it
	 * too. Do not rebuild an absolute grid without real vblank
	 * timestamps (DXGI frame statistics / present feedback) as its
	 * clock.
	 */
	uint32_t votes = 0, have = 0;
	uint32_t n;
	bool engaged;
	if (part_on) {
		// The divisor is a KNOWN schedule — no estimation, no flapping.
		n = part > 9 ? 9 : part;
		engaged = true;
	} else {
		// Trust decision (mode-majority / coherent-mean ladder) lives
		// inside cadence_n — it returns 0 when there is no trusted cadence.
		n = u_repaint_gate_cadence_n(g, period_ns, &votes, &have);
		engaged = g->mode == 1 && n == 2;
	}

	if (!engaged) {
		/*
		 * Legacy: fill once the app has missed a refresh.
		 *
		 * The 2-period wait is why warm-up judder survives every other fix.
		 * Measured on the Unity avatar with the ~185 ms plug-in publish already
		 * eliminated: recurring ~50 ms present gaps, each one the loop awake and
		 * ARMED but bailing on this gate ([GAP] ticks+9 bail_armed+0 bail_gate+7).
		 * Two periods is 33.4 ms of deliberate silence, and the ~5.5 ms tick
		 * granularity pushes the actual gap to ~50 ms -- comfortably over the 33 ms
		 * at which a dropped 60 Hz update becomes visible.
		 *
		 * DXR_WEAVE_REPAINT_QUIET_PERIODS sets the multiplier (default 1.2, swept:
		 * 2.0/1.5/1.2/1.05 -> 18/7/3/4 hitches >33 ms in the load window, with
		 * presents 60/s and weaves 56/s at every setting, i.e. NO queue theft). The
		 * floor matters: filling too early steals the slot the app's next frame
		 * needs -- the queue-theft this file documents at length -- so this must
		 * stay above 1.0, and 2.0 restores the historical behaviour exactly.
		 */
		static double quiet_mult = -1.0;
		if (quiet_mult < 0.0) {
			const char *qe = getenv("DXR_WEAVE_REPAINT_QUIET_PERIODS");
			quiet_mult = 1.2;
			if (qe != NULL && qe[0] != '\0') {
				const double v = atof(qe);
				if (v >= 1.0 && v <= 4.0) {
					quiet_mult = v;
				}
			}
			U_LOG_W("#1257 legacy fill gate: quiet threshold %.2f periods "
				        "(DXR_WEAVE_REPAINT_QUIET_PERIODS; 2.0 = historical)\n", quiet_mult);
		}
		return (double)quiet_ns >= (double)period_ns * quiet_mult;
	}

	/*
	 * Stall branch: the app's predicted frame is overdue — it is
	 * hitching, not pacing. This is the case #868 exists for; the
	 * steady-state window below must not close it (v1/v2 silently did).
	 * Behaves like the legacy gate plus panel-rate spacing. Under the
	 * partition the app's slot is a KNOWN schedule, so "overdue" starts
	 * a quarter period past its slot rather than a full period past an
	 * estimated one — an app that misses its slot must not leave the
	 * panel dark while the gate waits out an extra period.
	 */
	const uint64_t stall_ns = part_on ? (uint64_t)n * period_ns + period_ns / 4
	                                  : (uint64_t)(n + 1) * period_ns;
	if (quiet_ns >= stall_ns) {
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
	/*
	 * No shed under the partition: the governor exists to catch repaints
	 * DISPLACING app frames, and the partition excludes that channel by
	 * construction (the runtime paces the app; commits are phase-locked).
	 * A slipped interval there means the app missed its own slot — and
	 * then repaints are exactly what keeps the panel fed; shedding them
	 * collapses the display for nothing (measured: panel at ~19/s while
	 * the governor shed against self-inflicted "slips").
	 */
	const uint32_t shed = part_on ? 0 : slips / 3;
	const uint32_t budget = (n - 1) > shed ? (n - 1) - shed : 0;

	// Steady state: the governed budget of repaints per app frame...
	if (g->fires_since_app >= budget) {
		return false;
	}
	// ...not so early the eye pose would be staler than useful. Under the
	// partition the commits are phase-locked and every slot must be
	// filled STEADILY, so the window opens earlier and fires pack
	// tighter — slot coverage outranks marginal eye freshness (FORCE,
	// which fires with no regard for freshness, eyeballed "really
	// crisp")...
	const uint64_t open_ns = part_on ? period_ns / 4 : period_ns / 2;
	if (quiet_ns < open_ns) {
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
	// ...and spaced ~a period apart so the queue fills one slot per vblank
	// (3/4 under the partition, so a tick landing late still fits the
	// window's last slot).
	const uint64_t spacing_ns = part_on ? (period_ns * 3) / 4 : (period_ns * 9) / 10;
	if (since_rp_ns < spacing_ns) {
		return false;
	}

	if (!g->adaptive_logged) {
		g->adaptive_logged = true;
		if (part_on) {
			U_LOG_W("#1257 partition fill: known schedule — app every %u vblanks, "
			        "budget %u repaint(s) per app frame filling the other slots at "
			        "panel rate (DXR_APP_FRAME_DIVISOR=%u)",
			        n, n - 1, part);
		} else {
			U_LOG_W("#1257: repaint gate ADAPTIVE — app presents every %u vblanks "
			        "(%u/%u votes); budget %u repaint(s) per app frame, presented clear "
			        "of the app's own queue slot (DXR_WEAVE_REPAINT_GATE=legacy reverts)",
			        n, votes, have, n - 1);
		}
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
#ifdef XRT_OS_ANDROID
		// getenv reaches nothing on Android; the trace would be
		// permanently unreachable there without this. Env still wins.
		char sp_tr[PROP_VALUE_MAX] = {0};
		if ((e == NULL || e[0] == '\0') &&
		    __system_property_get("debug.dxr.weave_repaint_trace", sp_tr) > 0) {
			e = sp_tr;
		}
#endif
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
                       uint64_t period_ns,
                       const struct u_app_partition *ps)
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
	uint32_t n = u_repaint_gate_cadence_n(g, period_ns, &votes, &have);
	const uint32_t part = u_app_partition_divisor();
	const bool part_on =
	    part >= 2 && g->mode != 2 && ps != NULL && ps->next_release_ns != 0;
	uint32_t slips = 0, budget = 0;
	// Mirror the ENGAGED path, not just the estimator — earlier revisions
	// printed the estimator's numbers under the partition and twice sent
	// the verification chasing a "budget shed" that only existed in this
	// row. Under the partition: N is the divisor, there is no budget
	// arithmetic (the grid has none); slips stay printed as a health
	// readout of the app hitting its slots.
	const uint32_t slip_n = part_on ? part : n;
	if (slip_n >= 2 && period_ns > 0) {
		const uint64_t slip_floor_ns = (uint64_t)slip_n * period_ns + period_ns / 2;
		for (uint32_t i = 0; i < have; i++) {
			if (g->recent_iv_ns[i] >= slip_floor_ns) {
				slips++;
			}
		}
	}
	if (part_on) {
		n = part;
		budget = part - 1;
	} else if (u_repaint_gate_fill_ok(g, period_ns)) {
		// Mirror the ENGAGED fill path, same reason as the partition arm
		// above: fill's budget comes from the interval EMA, not from the
		// vblank-count estimator, and its slips use the EMA-relative
		// floor. N stays the estimator's reading — a diagnostic here, not
		// an input to this schedule.
		uint32_t fill_shed = 0;
		budget = u_repaint_gate_fill_budget(g, period_ns, &slips, &fill_shed);
	} else if (n >= 2) {
		const uint32_t shed = slips / 3;
		budget = (n - 1) > shed ? (n - 1) - shed : 0;
	}
	U_LOG_W("#1257 trace site=%s: ticks/s=%.1f fires/s=%.1f tick_iv=%.2fms fire=%.2fms "
	        "pace=%.2fms bail{armed=%u gate=%u race=%u} gate{mode=%s N=%u votes=%u/%u "
	        "slips=%u budget=%u ema=%.1fms jit=%.1fms samples=%u}",
	        site, (double)t->ticks / secs, (double)t->fires / secs,
	        (double)t->tick_iv_ema_ns / 1e6, (double)t->fire_ema_ns / 1e6,
	        (double)t->pace_ema_ns / 1e6, t->bail_armed, t->bail_gate, t->bail_race,
	        // Distinct states so a reader isn't puzzled by rows whose label
	        // and numbers disagree: partition (known schedule), env-pinned
	        // legacy, engaged adaptive (N==2), the deliberate N!=2 adaptive
	        // fallback, engaged fill, the untrusted-EMA fill fallback, or
	        // the plain default.
	        part_on ? "partition"
	                : (g->mode == 2
	                       ? "legacy"
	                       : (g->mode == 4
	                              ? (u_repaint_gate_fill_ok(g, period_ns) ? "fill" : "fill-fallback")
	                              : (g->mode == 1 ? (n == 2 ? "adaptive" : "adaptive-fallback")
	                                              : "default"))),
	        n, votes, have, slips, budget, (double)g->interval_ema_ns / 1e6,
	        (double)g->jitter_ema_ns / 1e6, g->samples);
	t->ticks = 0;
	t->fires = 0;
	t->bail_armed = 0;
	t->bail_gate = 0;
	t->bail_race = 0;
	t->last_report_ns = now_ns;
}

/*
 *
 * #1264 — the event-absorption shed (opt-in).
 *
 */

/*!
 * Fire-cost-keyed fill shedding, from the ~105 s box event's measured two-phase
 * signature (epic #1264, the Unity-d3d11 leg-A trace):
 *
 *  - PHASE 1 (onset): the GPU slows and weaves suddenly cost 6x (fire= 14 ms vs
 *    a 1.5-2.9 ms baseline) while the fill loop still ticks fine.
 *  - PHASE 2 (feedback): stretched weaves collide with the app's slots, races
 *    spike (49/5 s), the loop loses its cadence, and presents dip — the part
 *    that actually reads as judder.
 *
 * The shed breaks the phase-1 -> phase-2 coupling: after a fill whose fire
 * exceeded the threshold, SKIP fills for two panel periods, then let one
 * through as a probe; while the GPU stays slow every probe re-opens the window,
 * so the fill duty-cycles at ~1 per 3 periods instead of racing the app. App
 * frames never consult this — their slots stay sacrosanct, which is the point.
 *
 * Deliberately keyed on MEASURED fire duration (a leading indicator, monotone,
 * bounded) rather than slips or races (trailing) — the #1257 governor rounds
 * showed trailing-indicator feedback loops limit-cycle. Off by default:
 * `DXR_FILL_SHED_FIRE_MS=<n>` arms it with an n-millisecond threshold (6 is the
 * measured gap between healthy fires and event-onset fires on the bring-up
 * box); unset or 0 compiles the checks down to one branch.
 */
struct u_fill_shed
{
	int enabled; //!< 0 = unprobed (zero-init), then 1 = on / 2 = off from DXR_FILL_SHED_FIRE_MS.
	uint64_t thresh_ns;
	uint64_t shed_until_ns;
	uint64_t sheds;    //!< Fills suppressed (diagnostics).
	uint32_t episodes; //!< Threshold crossings (an episode may span many sheds).
};

static inline bool
u_fill_shed_enabled(struct u_fill_shed *s)
{
	if (s->enabled == 0) {
		const char *e = getenv("DXR_FILL_SHED_FIRE_MS");
		long v = (e != NULL) ? strtol(e, NULL, 10) : 0;
		if (v > 0) {
			s->thresh_ns = (uint64_t)v * 1000000u;
			s->enabled = 1;
			// One-shot per loop, deliberately: the first shed A/B ran
			// with no way to tell "armed but never tripped" from "the
			// env never reached this loop" — a null that cannot name
			// its own cause is not a measurement (#1264).
			U_LOG_W("#1264 shed ARMED in this fill loop: fires > %ld ms shed fills for 2 periods "
			        "(DXR_FILL_SHED_FIRE_MS)",
			        v);
		} else {
			s->enabled = 2;
		}
	}
	return s->enabled == 1;
}

/*!
 * Before firing a FILL: true = shed it (the caller bails this tick). Counts the
 * shed. App frames never call this.
 */
static inline bool
u_fill_shed_active(struct u_fill_shed *s, uint64_t now_ns)
{
	if (!u_fill_shed_enabled(s) || s->shed_until_ns == 0 || now_ns >= s->shed_until_ns) {
		return false;
	}
	s->sheds++;
	return true;
}

/*!
 * After every FILL completes: an expensive fire opens (or re-opens) the shed
 * window. Cheap fires do nothing, so recovery is one probe away by
 * construction — no hysteresis state to mis-tune.
 */
static inline void
u_fill_shed_note_fire(struct u_fill_shed *s, uint64_t start_ns, uint64_t end_ns, uint64_t period_ns)
{
	if (!u_fill_shed_enabled(s)) {
		return;
	}
	if (end_ns > start_ns && (end_ns - start_ns) > s->thresh_ns) {
		s->shed_until_ns = end_ns + 2 * period_ns;
		s->episodes++;
		// The first few episodes in full, then sampled — an event window
		// produces a burst of these and the absorption verdict needs the
		// engagement visible, not the log flooded.
		if (s->episodes <= 3 || (s->episodes % 32u) == 0u) {
			U_LOG_W("#1264 shed: episode %u — fire %.2f ms > threshold; shedding fills "
			        "for 2 periods (%llu fills shed so far)",
			        s->episodes, (double)(end_ns - start_ns) / 1.0e6,
			        (unsigned long long)s->sheds);
		}
	}
}

#ifdef __cplusplus
}
#endif
