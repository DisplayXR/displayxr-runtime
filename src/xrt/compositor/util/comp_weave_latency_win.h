// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Weave-latency measurement harness for DXGI-presenting compositors
 *         (Windows-only, header-only, env-gated; zero cost when off).
 *
 * DXR_WEAVE_LATENCY_CSV=<prefix> enables per-frame CSV logs correlating the
 * QPC timestamp taken immediately before process_atlas (T_weave — the moment
 * the weaver pulls its internal eye prediction) with the DXGI-reported
 * scanout time of the same frame. The residual R = SyncQPCTime(frame) −
 * T_weave is the latency the eye predictor has to cover; the late-weave work
 * exists to shrink it. Row kinds, joined offline (dxr-perf-study parser):
 *   H,qpc_freq                                            (once per file)
 *   F,seq,qpc_weave,qpc_present_ret,present_count         (one per weave+present)
 *   S,present_count,present_refresh,sync_refresh,sync_qpc,qpc_now
 * Each call site owns one instance and writes <prefix>.<site>.csv. The VK
 * native compositor has its own present_wait-based twin (same row format).
 *
 * @ingroup comp_util
 */

#pragma once

#ifdef _WIN32

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <windows.h>
#include <dxgi.h>

#include "comp_late_weave_lookahead.h"
#include "util/u_app_partition.h" // #1339: u_app_partition_divisor()

struct weave_latency_log
{
	FILE *f = nullptr;
	/*!
	 * CSV state (the CSV only; R-tracking is always on).
	 *
	 * ZERO MUST MEAN "UNPROBED" (#1128). Instances of this type live inside
	 * C-style structs that are `std::memset(0)` on init — the per-client
	 * `d3d11_client_render_resources` and the multi-compositor — which wipes
	 * any non-zero default member initializer. A `-1 = unprobed` sentinel
	 * therefore silently became `probed, disabled` for every service-side
	 * ledger, and `DXR_WEAVE_LATENCY_CSV` produced no rows for the workspace
	 * or app-HWND presenters no matter how it was set. Keeping the unprobed
	 * state at 0 makes the type correct under memset by construction rather
	 * than by remembering to re-arm it at each site.
	 */
	enum csv_state
	{
		CSV_UNPROBED = 0,
		CSV_OFF = 1,
		CSV_ON = 2,
	};
	int enabled = CSV_UNPROBED;

	/*!
	 * #206 horizon jitter probe — DXR_DP_FORWARD_HORIZON_TRACE=1.
	 *
	 * predict_weave_to_scanout_ns() returns the distance to the FIRST VBLANK
	 * AFTER `now + headroom`, so it is a STEP function of the headroom: as
	 * last frame's submit->present cost drifts across a vblank boundary the
	 * answer jumps a whole refresh period, with no hysteresis, smoothing or
	 * deadband (the engage WARN says as much). The DP hands that number to the
	 * vendor eye predictor, so a period-sized jump asks it to extrapolate the
	 * eyes ~16.7 ms further on one weave than the last. The HYPOTHESIS was
	 * that this shows up as an interlace-phase step on lateral head motion;
	 * it was tested against the #1339 lateral shiver (DXR_DP_FORWARD_HORIZON=0
	 * verified engaged) and is NOT its cause. The flip itself is real and
	 * app-dependent, so it is reported as a defect in its own right.
	 *
	 * A trivial app never shows it (small, steady headroom sits well inside
	 * one slot); an app with large or variable headroom straddles the boundary.
	 * That asymmetry is measurable, and nothing in the tree measured it, so the
	 * mechanism could only be argued. This counts the jumps.
	 *
	 * Zero-safe like the CSV state above, and for the same reason (#1128):
	 * these live in memset(0) C structs, so 0 MUST mean "unprobed".
	 */
	enum hz_state
	{
		HZ_UNPROBED = 0,
		HZ_OFF = 1,
		HZ_ON = 2,
	};
	int hz_trace = HZ_UNPROBED;
	uint64_t hz_win_start_ns = 0; // 0 = window not started
	uint64_t hz_last_ns = 0;
	uint64_t hz_min_ns = 0;
	uint64_t hz_max_ns = 0;
	uint64_t hz_sum_ns = 0;
	uint32_t hz_n = 0;
	uint32_t hz_jumps = 0; // consecutive-weave deltas >= half a refresh period
	//! #1432 closed loop: predicted horizon vs the vblank the frame really
	//! hit. A |error| >= half a period is a WRONG SLOT CALL — the flip the
	//! predictor made (or failed to make) did not match reality. Flips that
	//! match reality are correct and must not be smoothed away.
	//! Resolved from after_present, one to a few presents after the
	//! prediction, so a window's `resolved` lags its `n` by the resolve
	//! depth — read the two as neighbours, not as a ratio.
	uint32_t hz_resolved = 0;
	uint32_t hz_wrong_slot = 0;
	uint64_t hz_abs_err_sum_ns = 0;
	int64_t hz_err_sum_ns = 0;    // signed, to see a systematic bias
	int hz_depth = 0;             // governor depth at the last present (0 = no governor)
	uint32_t hz_depth_changes = 0;
	uint64_t hz_sync_lag_sum = 0; // sum of (SyncRefreshCount - PresentRefreshCount)
	//! Histogram of round(err / period), k in [-2, +4] (clamped): the MODE is
	//! the pipeline's constant offset (a composed chain adds a whole DWM
	//! frame the forward horizon does not model); a WRONG SLOT is a sample
	//! that lands in any other bucket. Measured against zero, a constant
	//! +1 period read as 100% wrong — which is not what "wrong" means.
	uint32_t hz_k_hist[7] = {0, 0, 0, 0, 0, 0, 0};

	/*!
	 * #1435 closed loop — the pipeline's whole-period offset, LEARNED.
	 *
	 * The grid snap below predicts the first vblank a weave can reach. On an
	 * opaque flip chain at queue depth 1 that IS the flip (measured: +0
	 * periods, 0.00 ms error, 300/302 frames). But the frame DXGI reports as
	 * flipped sits whole periods later on a DComp (transparent) chain (+1 at
	 * depth 1 AND at depth 2 — the composition frame) and on an opaque chain
	 * once the governor backs off to depth 2 (+1 — the queued frame); the two
	 * terms were measured NOT to simply add. Rather than model chain type x
	 * governor depth x DWM state, learn the integer from the realised
	 * outcomes the ring already resolves: residual k = round((realised -
	 * handed) / period) over the last 32 resolved weaves; when a non-zero k
	 * is the mode of >= 24 of them, fold it into `po_applied`. The per-weave
	 * grid snap stays raw — this corrects a CONSTANT that changes on rare
	 * state transitions, which is what a mode with hysteresis is for (the
	 * levers doc's anti-pattern is smoothing a per-frame quantity).
	 *
	 * Zero-safe: applied 0 = today's behaviour until the first lock (~0.5 s
	 * at 60 weaves/s). DXR_DP_FORWARD_HORIZON_LOOP=0 pins applied at 0.
	 */
	enum po_state
	{
		PO_UNPROBED = 0,
		PO_OFF = 1,
		PO_ON = 2,
	};
	int po_enabled = PO_UNPROBED;
	int po_applied = 0;   // whole periods currently added to the forward horizon, [0, cap]
	int8_t po_win[32] = {0};
	uint32_t po_win_n = 0;
	uint32_t po_win_head = 0;
	uint32_t po_changes = 0;
	uint64_t po_last_log_ns = 0;
	bool po_clamp_logged = false;

	void
	po_observe(int k, uint64_t period_ns)
	{
		if (po_enabled != PO_ON || period_ns == 0) {
			return;
		}
		if (k < -2) {
			k = -2;
		}
		if (k > 4) {
			k = 4;
		}
		po_win[po_win_head] = (int8_t)k;
		po_win_head = (po_win_head + 1) % 32;
		if (po_win_n < 32) {
			po_win_n++;
			return; // decide only on a full window
		}
		uint32_t hist[7] = {0, 0, 0, 0, 0, 0, 0};
		for (uint32_t i = 0; i < 32; i++) {
			hist[po_win[i] + 2]++;
		}
		int mode_i = 0;
		for (int i = 1; i < 7; i++) {
			if (hist[i] > hist[mode_i]) {
				mode_i = i;
			}
		}
		const int m = mode_i - 2;
		if (m == 0 || hist[mode_i] < 24) {
			return; // locked, or not yet a clear majority
		}
		/*
		 * One period per change. The mode's SIGN is evidence; its
		 * magnitude is not a safe step. Measured on the avatar eyeball A/B
		 * (2026-09-10): a governor backoff mid-run deepened the queue for a
		 * few dozen frames, the residual read +2 in 24/32, and the loop
		 * jumped +1 -> +3 in one change — 56-75 ms handed, the DP's 60 ms
		 * clamp engaged, 39% wrong-slot for the window, on a pipeline that
		 * settled back to +1 moments later. Stepping by one and re-measuring
		 * (the window resets on every change, ~0.5 s) bounds the overshoot
		 * to a single period and costs nothing on a genuine +2 (two steps,
		 * ~1 s).
		 */
		const int step = (m > 0) ? 1 : -1;
		int next = po_applied + step;
		/*
		 * Floor 0: the grid snap is the FIRST vblank after now + headroom,
		 * so a realised flip before it can only mean headroom over-
		 * prediction — not a pipeline offset — and applying -1 would hand
		 * the DP a bimodal value (offset on long snaps, un-offset on short
		 * ones: the boundary flip this work exists to remove).
		 *
		 * Ceiling: the handed horizon is at most (k + 2) periods; keep it
		 * under ~55 ms so a vendor clamp (the Leia path pins at 60 ms,
		 * silently) never flattens it into a constant the loop cannot see
		 * — it scores what the runtime handed, not what the DP used.
		 * 55 ms / 16.7 ms → +3 at 60 Hz; +4 is the hard cap (governor depth).
		 */
		int cap = (int)(55000000ULL / period_ns);
		if (cap > 4) {
			cap = 4;
		}
		if (next < 0) {
			next = 0;
		}
		if (next > cap) {
			next = cap;
		}
		if (next == po_applied) {
			// Clamped. Drop the window rather than re-fire every frame, and
			// say so ONCE — a pipeline beyond the cap converges to a knowingly
			// short horizon with no other witness when the trace is off.
			if (!po_clamp_logged) {
				po_clamp_logged = true;
				U_LOG_W("#1435 forward horizon: pipeline offset clamped at %+d periods (residual mode %+d "
				        "persists; cap %+d for a %.2f ms period) — the horizon handed to the DP is knowingly "
				        "short by that residual",
				        po_applied, m, cap, (double)period_ns / 1e6);
			}
			po_win_n = 0;
			return;
		}
		const uint64_t now_ns = os_monotonic_get_ns();
		if (now_ns - po_last_log_ns > 5000000000ULL) {
			// Lifecycle-level: a pipeline state change, once per lock, throttled.
			U_LOG_W("#1435 forward horizon: pipeline offset %+d -> %+d periods (residual mode %+d in %u/32 "
			        "resolved weaves; change #%u — gaps in the numbering are throttled changes)",
			        po_applied, next, m, hist[mode_i], po_changes + 1);
			po_last_log_ns = now_ns;
		}
		po_applied = next;
		po_changes++;
		po_win_n = 0; // re-measure against the new value
	}

	uint64_t seq = 0;
	uint64_t qpc_weave = 0; // armed by mark_weave, consumed by after_present
	uint64_t qpc_freq = 0;

	// Always-on weave→scanout tracking for the DP timing feedback loop
	// (set_frame_timing): small ring correlating each present's
	// PresentCount with its weave-record QPC; resolved against
	// GetFrameStatistics once per frame in after_present.
	struct pending
	{
		UINT present_count;
		uint64_t qpc;
		//! #867: what xrWaitFrame promised this frame's photon time would
		//! be (os_monotonic ns — QPC-derived, so directly comparable to
		//! the scanout below). 0 = the caller did not supply one.
		uint64_t predicted_ns;
		bool repaint; //!< #868: this weave had no app frame behind it
		//! #1432: the forward horizon handed to the DP for this weave and the
		//! QPC it was measured from, so the realised scanout can be checked
		//! against it. 0 = no horizon was computed for this weave.
		uint64_t horizon_ns;
		uint64_t horizon_base_qpc;
	};
	uint64_t pending_predicted_ns = 0; // armed by mark_weave, consumed by after_present
	bool pending_repaint = false;      // #868: this weave re-wove an unchanged atlas
	uint64_t pending_horizon_ns = 0;   // #1432: armed by predict_weave_to_scanout_ns
	uint64_t pending_horizon_base_qpc = 0;
	//! #1051: last SyncQPCTime emitted as an S row, so stale-mixed stats can
	//! be suppressed from the CSV (see the S-row write in after_present).
	uint64_t last_s_sync_qpc = 0;
	pending ring[8] = {};
	int ring_head = 0;
	int ring_count = 0;
	uint64_t measured_r_ns = 0; // last completed frame's weave→scanout; 0 = unknown

	/*
	 * #206 forward horizon — the vblank GRID, captured always-on from DXGI
	 * frame statistics (not the CSV-gated last_s_sync_qpc above). The
	 * retrospective measured_r_ns is an estimator of a quantity that is
	 * not constant under variable cadence; these fields let the runtime
	 * compute THIS weave's present-to-photon time forward instead:
	 * vblanks sit on last_sync_qpc + k * refresh_period_qpc, and the
	 * period is measured from the statistics themselves (SyncQPCTime
	 * deltas over SyncRefreshCount deltas), so the grid is vsync-LOCKED —
	 * the #1257 partition v3 post-mortem is exactly what happens to a
	 * grid built from the monotonic clock times a nominal rate instead.
	 */
	uint64_t last_sync_qpc = 0;      // newest vblank timestamp seen; monotone
	uint64_t last_sync_refresh = 0;  // SyncRefreshCount at that vblank
	uint64_t refresh_period_qpc = 0; // TRUE measured vblank period; 0 = unknown
	/*!
	 * Last frame's mark→present-return cost, kept per weave POPULATION
	 * (#1432). An app weave sits behind the frame-latency waitable at the
	 * governor's depth; a repaint never waits on it and is paced to one
	 * panel period — the same two populations #868 already keeps apart for
	 * `measured_r_ns`, for the same reason. Feeding both into one value
	 * made the forward horizon's headroom alternate between them, and when
	 * the two sit either side of a vblank boundary the horizon flipped by a
	 * whole refresh period on alternate weaves (the #1431 trace: 0 flips/s
	 * on a trivial app that never repaints, up to 8.6/s on a heavy one).
	 */
	uint64_t headroom_qpc = 0;         // app weaves
	uint64_t headroom_repaint_qpc = 0; // repaint weaves (#868 population)

	uint64_t
	freq()
	{
		if (qpc_freq == 0) {
			LARGE_INTEGER f2;
			QueryPerformanceFrequency(&f2);
			qpc_freq = (uint64_t)f2.QuadPart;
		}
		return qpc_freq;
	}

	bool
	on(const char *site)
	{
		if (enabled == CSV_UNPROBED) {
			const char *prefix = getenv("DXR_WEAVE_LATENCY_CSV");
			enabled = CSV_OFF;
			if (prefix != nullptr && prefix[0] != '\0') {
				char path[MAX_PATH];
				snprintf(path, sizeof(path), "%s.%s.csv", prefix, site);
				f = fopen(path, "a");
				if (f != nullptr) {
					fprintf(f, "H,%lld\n", (long long)freq());
					enabled = CSV_ON;
				}
			}
		}
		return enabled == CSV_ON;
	}

	//! @p predicted_display_time_ns is the value xrWaitFrame handed the app
	//! for this frame (0 if unknown); it is differenced against the frame's
	//! true scanout in after_present to expose prediction error (#867).
	void
	mark_weave(const char *site, uint64_t predicted_display_time_ns = 0, bool repaint = false)
	{
		(void)on(site);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		qpc_weave = (uint64_t)now.QuadPart;
		pending_predicted_ns = predicted_display_time_ns;
		pending_repaint = repaint;
	}

	void
	after_present(const char *site, IDXGISwapChain *sc, struct late_weave_governor *gov = nullptr);

	/*!
	 * #206: forward-predict the weave→scanout time of a weave RECORDED NOW,
	 * from the vsync-locked vblank grid. Returns ns, or 0 when no trusted
	 * grid exists (no statistics yet, statistics stalled >500 ms, or the
	 * period unmeasured) — callers hand 0 to the DP as "unknown" and the
	 * plug-in falls back to its retrospective heuristic.
	 *
	 * The prediction is the first vblank after now + headroom, where the
	 * headroom is the measured cost of getting a weave from its record
	 * mark through submit and present into the queue (the last value from
	 * THIS weave's population — app or repaint, #1432 — clamped to
	 * [1 ms, one refresh]). A stale-but-on-grid SyncQPCTime is
	 * harmless: it only shifts the base by whole periods, and the period
	 * itself is measured, not nominal, so the grid does not drift.
	 */
	uint64_t
	predict_weave_to_scanout_ns()
	{
		// #1432: no horizon on record until one is computed below, so a
		// weave that returns 0 (kill switch, no grid) resolves to nothing.
		pending_horizon_ns = 0;
		pending_horizon_base_qpc = 0;
		// Kill switch: DXR_DP_FORWARD_HORIZON=0 returns "unknown" everywhere,
		// so the plug-in falls back to its retrospective heuristic — the
		// pre-#206 behavior, byte for byte.
		static int fh_enabled = -1;
		if (fh_enabled < 0) {
			const char *e = getenv("DXR_DP_FORWARD_HORIZON");
			fh_enabled = (e != nullptr && e[0] == '0') ? 0 : 1;
		}
		if (fh_enabled == 0) {
			return 0;
		}
		if (po_enabled == PO_UNPROBED) {
			const char *e = getenv("DXR_DP_FORWARD_HORIZON_LOOP");
			po_enabled = (e != nullptr && e[0] == '0') ? PO_OFF : PO_ON;
		}
		if (last_sync_qpc == 0 || refresh_period_qpc == 0) {
			return 0;
		}
		const uint64_t f2 = freq();
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		const uint64_t nowq = (uint64_t)now.QuadPart;
		if (nowq <= last_sync_qpc) {
			return 0; // clock weirdness — refuse rather than guess
		}
		if (nowq - last_sync_qpc > f2 / 2) {
			return 0; // statistics stalled >500 ms — grid not trusted
		}
		// #1432: headroom from THIS weave's population; fall back to the
		// other one only while ours has never been measured.
		uint64_t head = pending_repaint ? headroom_repaint_qpc : headroom_qpc;
		if (head == 0) {
			head = pending_repaint ? headroom_qpc : headroom_repaint_qpc;
		}
		const uint64_t head_min = f2 / 1000; // 1 ms
		if (head < head_min) {
			head = head_min;
		}
		if (head > refresh_period_qpc) {
			head = refresh_period_qpc;
		}
		const uint64_t ready = nowq + head;
		// First vblank strictly after `ready`.
		const uint64_t target =
		    last_sync_qpc + ((ready - last_sync_qpc) / refresh_period_qpc + 1) * refresh_period_qpc;
		uint64_t horizon_ns = (uint64_t)((double)(target - nowq) * 1000000000.0 / (double)f2);
		const uint64_t period_ns = (uint64_t)((double)refresh_period_qpc * 1000000000.0 / (double)f2);
		// #1435: the learned whole-period pipeline offset (0 until locked).
		if (po_enabled == PO_ON && po_applied != 0) {
			const int64_t adj = (int64_t)horizon_ns + (int64_t)po_applied * (int64_t)period_ns;
			horizon_ns = adj > 0 ? (uint64_t)adj : horizon_ns;
		}
		note_horizon(horizon_ns, period_ns);
		// #1432: remember what we told the DP, so after_present can check it
		// against the vblank this weave really lands on.
		pending_horizon_ns = horizon_ns;
		pending_horizon_base_qpc = nowq;
		return horizon_ns;
	}

	/*!
	 * #1432: one resolved weave — the realised weave→scanout of a frame
	 * against the forward horizon it was handed. Aggregated into the
	 * DXR_DP_FORWARD_HORIZON_TRACE row; no-op when the trace is off.
	 */
	void
	note_horizon_outcome(int64_t err_ns, uint64_t period_ns, uint32_t sync_lag_refreshes)
	{
		if (hz_trace != HZ_ON || period_ns == 0) {
			return;
		}
		const uint64_t a = (uint64_t)(err_ns < 0 ? -err_ns : err_ns);
		hz_resolved++;
		hz_abs_err_sum_ns += a;
		hz_err_sum_ns += err_ns;
		hz_sync_lag_sum += sync_lag_refreshes;
		// Nearest whole period, clamped into the histogram.
		const double kf = (double)err_ns / (double)period_ns;
		int k = (int)(kf >= 0.0 ? kf + 0.5 : kf - 0.5);
		if (k < -2) {
			k = -2;
		}
		if (k > 4) {
			k = 4;
		}
		hz_k_hist[k + 2]++;
	}

	/*!
	 * One throttled row per ~5 s, only under DXR_DP_FORWARD_HORIZON_TRACE=1.
	 * Never per-frame: this is on the weave path (see the logging convention
	 * in docs/reference/debug-logging.md).
	 */
	void
	note_horizon(uint64_t horizon_ns, uint64_t period_ns)
	{
		if (hz_trace == HZ_UNPROBED) {
			const char *e = getenv("DXR_DP_FORWARD_HORIZON_TRACE");
			hz_trace = (e != nullptr && e[0] != '0') ? HZ_ON : HZ_OFF;
		}
		if (hz_trace != HZ_ON || period_ns == 0) {
			return;
		}

		const uint64_t now_ns = os_monotonic_get_ns();
		if (hz_win_start_ns == 0) {
			hz_win_start_ns = now_ns;
		}

		// A jump of half a period or more cannot be drift — the grid quantises
		// to whole periods, so this counts boundary flips and nothing else.
		if (hz_n > 0) {
			const uint64_t d = (horizon_ns > hz_last_ns) ? (horizon_ns - hz_last_ns)
			                                             : (hz_last_ns - horizon_ns);
			if (d >= period_ns / 2) {
				hz_jumps++;
			}
		}
		hz_last_ns = horizon_ns;
		if (hz_n == 0 || horizon_ns < hz_min_ns) {
			hz_min_ns = horizon_ns;
		}
		if (horizon_ns > hz_max_ns) {
			hz_max_ns = horizon_ns;
		}
		hz_sum_ns += horizon_ns;
		hz_n++;

		if (now_ns - hz_win_start_ns < 5000000000ULL) {
			return;
		}
		const double secs = (double)(now_ns - hz_win_start_ns) / 1e9;
		// Mode of the offset histogram = the pipeline's constant; everything
		// else is a wrong slot call.
		int mode_i = 0;
		for (int i = 1; i < 7; i++) {
			if (hz_k_hist[i] > hz_k_hist[mode_i]) {
				mode_i = i;
			}
		}
		hz_wrong_slot = hz_resolved - hz_k_hist[mode_i];
		U_LOG_W(
		    "#206 horizon trace: n=%u  min %.2f  mean %.2f  max %.2f ms  "
		    "spread %.2f ms (%.2f periods)  boundary flips %u (%.1f/s)  "
		    "| resolved %u  residual %+d periods  applied %+d  wrong-slot %u (%.1f%%)  mean |err| %.2f ms  "
		    "bias %+.2f ms  sync-flip lag %.2f refr  | depth %d (%u changes)",
		    hz_n, (double)hz_min_ns / 1e6, (double)hz_sum_ns / (double)hz_n / 1e6, (double)hz_max_ns / 1e6,
		    (double)(hz_max_ns - hz_min_ns) / 1e6, (double)(hz_max_ns - hz_min_ns) / (double)period_ns,
		    hz_jumps, (double)hz_jumps / secs, hz_resolved, mode_i - 2, po_applied, hz_wrong_slot,
		    hz_resolved ? 100.0 * (double)hz_wrong_slot / (double)hz_resolved : 0.0,
		    hz_resolved ? (double)hz_abs_err_sum_ns / (double)hz_resolved / 1e6 : 0.0,
		    hz_resolved ? (double)hz_err_sum_ns / (double)hz_resolved / 1e6 : 0.0,
		    hz_resolved ? (double)hz_sync_lag_sum / (double)hz_resolved : 0.0, hz_depth, hz_depth_changes);
		hz_depth_changes = 0;

		hz_win_start_ns = now_ns;
		hz_min_ns = 0;
		hz_max_ns = 0;
		hz_sum_ns = 0;
		hz_n = 0;
		hz_jumps = 0;
		hz_resolved = 0;
		hz_wrong_slot = 0;
		hz_abs_err_sum_ns = 0;
		hz_err_sum_ns = 0;
		hz_sync_lag_sum = 0;
		for (int i = 0; i < 7; i++) {
			hz_k_hist[i] = 0;
		}
	}

	/*!
	 * Close the CSV, if one was opened, and re-arm the probe.
	 *
	 * A file-scope log lives for the process and never needed this. #918 PR 6
	 * gave the service one log PER PRESENTER CHAIN, and a chain belongs to a
	 * client — so with `DXR_WEAVE_LATENCY_CSV` set, a session that connects and
	 * disconnects clients would leak one `FILE *` per client without it.
	 */
	void
	close()
	{
		if (f != nullptr) {
			fclose(f);
			f = nullptr;
		}
		enabled = CSV_UNPROBED;
	}
};

/*!
 * Late-weave latency governor (#850).
 *
 * maxFrameLatency=1 removes all CPU/GPU frame overlap. That is the point on a
 * pipeline with headroom (R collapses to one refresh) — but on a pipeline that
 * cannot make rate it serializes the whole frame: measured −63% fps for ~4 ms
 * of R p50 on a saturated app, with no p95 win. Two escapes:
 *
 *  - DXR_LATE_WEAVE_MAX_LATENCY=N (1..LATE_WEAVE_MAX_DEPTH, default 1) forces
 *    a fixed queue depth. N>1 restores N−1 frames of overlap at the cost of
 *    (N−1) refresh intervals of extra weave-time eye-prediction horizon.
 *  - Saturation auto-backoff (default ON; DXR_LATE_WEAVE_AUTOBACKOFF=0
 *    disables; only active at the default depth 1): when the paced frame
 *    interval persistently exceeds what the current depth can absorb, the
 *    governor jumps straight to the depth the pipeline actually needs. Once
 *    it holds rate again it steps back down one level at a time — with a
 *    dwell that doubles on every failed probe, so a persistently saturated
 *    app converges instead of oscillating.
 *
 * Refresh-rate scaling (the reason the depth is computed, not fixed): the
 * frames of queue needed to keep a pipeline full is ceil(render_interval /
 * display_period), which grows as the panel gets faster. A 16 ms app needs
 * 1 frame at 60 Hz, ~3 at 165 Hz, ~4 at 240 Hz. A fixed 1→2 backoff is a
 * 60 Hz-shaped rule and leaves a high-refresh panel starved.
 *
 * The caller owns applying transitions (SetMaximumFrameLatency + one-shot
 * WARN — silent backoff would make gate numbers non-reproducible) and relaxes
 * its scanout pacer by (effective−1) presents.
 */
//! Queue-depth ceiling: covers a 16 ms frame on a 240 Hz panel (ceil(16/4.17)).
#define LATE_WEAVE_MAX_DEPTH 4

struct late_weave_governor
{
	int base = -1;         // DXR_LATE_WEAVE_MAX_LATENCY, probed once (1..MAX)
	int auto_backoff = -1; // DXR_LATE_WEAVE_AUTOBACKOFF, default 1
	int effective = 1;
	bool paced_logged = false; // #1339 one-shot

	// Saturation signal: EMA of weave-mark→weave-mark wall time, judged
	// against the display period derived from DXGI frame statistics.
	uint64_t last_mark_qpc = 0;
	double interval_ema_ns = 0.0;
	//! #867: wait_frame-return → weave span (app render + pacer wait). This
	//! is the app-side half of the lookahead, measured rather than inferred
	//! from the frame interval — the interval also contains post-weave work,
	//! and using it whole over-predicted by ~9 ms on a saturated app.
	uint64_t wait_frame_qpc = 0;
	double wait_to_weave_ema_ns = 0.0;
	double period_ns = 0.0;
	uint64_t last_sync_qpc = 0;
	UINT last_sync_refresh = 0;

	int over_frames = 0;
	int calm_frames = 0;
	// Slip-rate signal: the cost EMA only sees serialized frame cost, but a
	// pipeline can drop frames purely from GPU-completion jitter (shared-queue
	// iGPU: the weave lands behind the app's own render and misses DWM
	// pickup) while the EMA sits far under the starvation threshold —
	// measured: 11% of frames at 2 vsyncs with EMA 18.5 ms vs a 43 ms
	// threshold. A vsync-doubled frame is dt > ~1.55x period; escalate when
	// they exceed ~8% of a 120-mark window. An extra banked token is
	// latency-free at a met frame rate (presents are still picked up at the
	// next tick) — it only absorbs the jitter.
	int slip_marks = 0;
	int slip_count = 0;
	// Adaptive tick-align (the composed-chain freshness stage): aligning the
	// weave to the compositor tick buys a constant prediction phase, but it
	// also serializes each frame's whole GPU chain into exactly one period —
	// on a contended iGPU that alone dropped ~7-8% of frames at EVERY depth
	// (depth absorbs cost jitter, not the align's hard deadline). A dropped
	// frame holds a STALE weave on glass for a whole extra period — the
	// exact artifact late-weave exists to prevent — so when escalation has
	// run out (slip-heavy window at MAX depth) the align yields for a dwell
	// instead. Healthy machines keep the constant phase permanently.
	// Hold dwell doubles when a hold re-triggers shortly after expiring
	// (chronic contention converges to align-off ≈ free-run throughput;
	// transient contention recovers the constant phase after one dwell).
	uint64_t align_hold_until_qpc = 0;
	uint64_t align_hold_clear_qpc = 0;
	uint64_t align_hold_ns = 10ull * 1000000000ull; // 10 s, x2 on re-trigger, cap 5 min
	uint64_t backoff_qpc = 0;
	uint64_t last_probe_qpc = 0;
	uint64_t probe_dwell_ns = 30ull * 1000000000ull; // ×2 per failed probe, cap 5 min

	/*!
	 * #1432 follow-up — depth flapping made observable, and capped.
	 *
	 * The targets log the FIRST backoff and the FIRST return only, so a
	 * "cleared, probing return -> max latency 3" line (seen on the avatar)
	 * means depth had silently climbed to 4 first. Every change is now
	 * logged here, throttled to one WARN per 2 s with a suppressed count,
	 * and counted for the horizon trace row.
	 *
	 * The flap itself: slip-rate escalation adds a level per 120-mark window
	 * with >= 8% vsync-doubled frames, up to MAX, with no reference to what
	 * the frame cost needs; the return probe steps down after 300 calm frames
	 * once the probe dwell has passed; on a GPU-bound app at ~47 fps the two
	 * fight — up one per 2 s, down one per 5 s, dwell doubling to 5 min —
	 * and every step moves the pipeline offset the eye predictor is
	 * extrapolating over by a whole period. Measured on the 3DLuma avatar on
	 * the iGPU: `applied` swinging +1/+2/+3 within one 60 s leg.
	 *
	 * Policy (DXR_LATE_WEAVE_SLIP_CAP=0 restores the old behaviour for A/B):
	 *  - slip escalation is capped at needed_depth() + 1 — the one extra
	 *    banked token the field comment above credits it with; beyond that
	 *    the align yields, as it already does at MAX;
	 *  - the return probe needs THREE consecutive clean slip windows (6 s at
	 *    60 marks/s), not one — slip is bursty on a ~50 fps app, and a probe
	 *    fired on a single clean window re-escalated within 2-20 s in every
	 *    measured leg;
	 *  - a slip escalation within TWO dwells of a return probe is a FAILED
	 *    probe and doubles the probe dwell (30 -> 60 -> 120 -> 300 s). The
	 *    starvation path has the same rule at 5 s; the slip path's window
	 *    scales with the dwell because the measured cycle re-escalated
	 *    ~55 s after each probe (one dwell + 300 calm frames) — a fixed
	 *    30 s never caught it and the cycle ran forever (#1443). The dwell
	 *    never decays, so a genuinely new load arriving inside the window
	 *    is also counted as a failure; accepted.
	 *
	 * Two things the cap must NOT do (review of #1441): surrender the
	 * composed-chain tick-align — that yield is keyed on LATE_WEAVE_MAX_DEPTH,
	 * not on the cap, so a need-capped depth keeps its constant phase; and
	 * pin the depth — a probe is still allowed when the depth exceeds what
	 * the frame cost needs, even if the slip rate never goes quiet, so the
	 * dwell doubling (not a ratchet) is what converges a chronic slipper.
	 */
	int slip_cap_enabled = -1;      // DXR_LATE_WEAVE_SLIP_CAP, default 1
	int clean_windows = 0;          // consecutive slip windows below the threshold
	uint32_t depth_changes = 0;
	uint32_t depth_changes_suppressed = 0;
	uint64_t depth_log_qpc = 0;

	void
	log_change(uint64_t now, uint64_t freq_hz, int from, const char *reason)
	{
		depth_changes++;
		if (depth_log_qpc != 0 && (double)(now - depth_log_qpc) < 2.0 * (double)freq_hz) {
			depth_changes_suppressed++;
			return;
		}
		U_LOG_W("Late-weave governor: depth %d -> %d (%s; interval %.1f ms, period %.1f ms, need %d, "
		        "probe dwell %llu s; change #%u, %u suppressed since last line)",
		        from, effective, reason, interval_ema_ns / 1e6, period_ns / 1e6, needed_depth(),
		        (unsigned long long)(probe_dwell_ns / 1000000000ull), depth_changes, depth_changes_suppressed);
		depth_changes_suppressed = 0;
		depth_log_qpc = now;
	}

	int
	base_latency()
	{
		if (base < 0) {
			const char *e = getenv("DXR_LATE_WEAVE_MAX_LATENCY");
			int v = (e != nullptr && e[0] != '\0') ? atoi(e) : 1;
			base = v < 1 ? 1 : (v > LATE_WEAVE_MAX_DEPTH ? LATE_WEAVE_MAX_DEPTH : v);
			const char *a = getenv("DXR_LATE_WEAVE_AUTOBACKOFF");
			auto_backoff = (a != nullptr && a[0] == '0') ? 0 : 1;
			const char *sc = getenv("DXR_LATE_WEAVE_SLIP_CAP");
			slip_cap_enabled = (sc != nullptr && sc[0] == '0') ? 0 : 1;
			effective = base;
		}
		return base;
	}

	void
	on_stats(const DXGI_FRAME_STATISTICS &stats, uint64_t freq_hz)
	{
		const uint64_t sync_qpc = (uint64_t)stats.SyncQPCTime.QuadPart;
		if (last_sync_qpc != 0 && sync_qpc > last_sync_qpc &&
		    stats.SyncRefreshCount > last_sync_refresh) {
			const double per = (double)(sync_qpc - last_sync_qpc) * 1000000000.0 /
			                   (double)freq_hz /
			                   (double)(stats.SyncRefreshCount - last_sync_refresh);
			// 1000 Hz .. 10 Hz sanity window. The old 4 ms floor sat
			// right on a 240 Hz panel's period (4.17 ms), so any jitter
			// rejected every sample, left period_ns at 0 and silently
			// disabled the governor on exactly the displays that need it.
			if (per > 1e6 && per < 1e8) {
				period_ns = (period_ns == 0.0) ? per : period_ns * 0.9 + per * 0.1;
			}
		}
		last_sync_qpc = sync_qpc;
		last_sync_refresh = stats.SyncRefreshCount;
	}

	//! Headroom a level of queue is credited with. A depth of N absorbs N
	//! periods of work, and we only add a level once the frame overruns that
	//! by 30% — the same margin at every level, so deeper queues need
	//! proportionally more overrun to justify the refresh of extra latency
	//! they cost. Measured on a saturated Unity app at 60 Hz: forcing the
	//! extra level a plain ceil() asked for bought +3 fps and cost +14 ms of
	//! R p95, which is the wrong trade on a 3D display. The governor exists
	//! to escape pathological serialization, not to chase peak fps.
	static constexpr double kLevelHeadroom = 1.30;

	//! #867: app-visible wait_frame→scanout lookahead from this governor's
	//! measured frame interval plus the caller's measured weave→scanout
	//! residual. 0 when unmeasured (caller keeps its fallback).
	uint64_t
	predicted_lookahead_ns(uint64_t measured_r_ns) const
	{
		return late_weave_lookahead_ns(wait_to_weave_ema_ns, measured_r_ns, period_ns);
	}

	//! Call when xrWaitFrame returns, so the span to this frame's weave can
	//! be measured.
	void
	on_wait_frame()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		wait_frame_qpc = (uint64_t)now.QuadPart;
	}

	//! Frames of queue this pipeline needs at the current panel period —
	//! the smallest N whose absorbed budget (N × period × headroom) covers
	//! the frame. Clamped; 0 when unknown.
	int
	needed_depth() const
	{
		if (period_ns <= 0.0 || interval_ema_ns <= 0.0) {
			return 0;
		}
		const double budget = period_ns * kLevelHeadroom;
		int n = (int)((interval_ema_ns + budget - 1.0) / budget);
		if (n < 1) {
			n = 1;
		}
		return n > LATE_WEAVE_MAX_DEPTH ? LATE_WEAVE_MAX_DEPTH : n;
	}

	//! Call once per paced frame (end of the weave-mark pacer). Returns +1 when
	//! the depth rose, −1 when it fell, 0 otherwise; the caller applies
	//! SetMaximumFrameLatency and reads `effective`.
	/*!
	 * The waitable wait TIMED OUT (DWM stopped picking up presents —
	 * occluded/minimized window, not saturation). A ~100 ms timeout sits
	 * BELOW the 250 ms pause threshold in on_mark, so without this the EMA
	 * reads occlusion as a saturated pipeline, backs off to max depth, and
	 * probe-dwell doubling pins it there for minutes after restore. Treat
	 * it exactly like the pause: reset the signal, contribute nothing.
	 */
	void
	on_wait_timeout()
	{
		last_mark_qpc = 0;
		interval_ema_ns = 0.0;
		over_frames = 0;
		calm_frames = 0;
		slip_marks = 0;
		slip_count = 0;
		clean_windows = 0;
	}

	//! May the composed-chain tick-align run this frame? (See
	//! align_hold_until_qpc — false while yielded after slip-heavy windows
	//! at MAX depth.)
	bool
	align_ok()
	{
		if (align_hold_until_qpc == 0) {
			return true;
		}
		LARGE_INTEGER now_li;
		QueryPerformanceCounter(&now_li);
		if ((uint64_t)now_li.QuadPart >= align_hold_until_qpc) {
			align_hold_clear_qpc = align_hold_until_qpc;
			align_hold_until_qpc = 0;
			return true;
		}
		return false;
	}

	int
	on_mark(uint64_t freq_hz)
	{
		(void)base_latency();
		LARGE_INTEGER now_li;
		QueryPerformanceCounter(&now_li);
		const uint64_t now = (uint64_t)now_li.QuadPart;

		double dt_ns = 0.0;
		if (last_mark_qpc != 0 && now > last_mark_qpc) {
			dt_ns = (double)(now - last_mark_qpc) * 1000000000.0 / (double)freq_hz;
			// A pause (drag modal loop, occlusion, debugger) is not
			// saturation — reset the signal instead of poisoning it.
			if (dt_ns > 250e6) {
				interval_ema_ns = 0.0;
				over_frames = 0;
				calm_frames = 0;
				slip_marks = 0;
				slip_count = 0;
				clean_windows = 0;
				dt_ns = 0.0;
			}
		}
		// #867: wait_frame → weave, the app-side span of the lookahead.
		if (wait_frame_qpc != 0 && now > wait_frame_qpc) {
			const double phi_ns =
			    (double)(now - wait_frame_qpc) * 1000000000.0 / (double)freq_hz;
			if (phi_ns < 250e6) {
				wait_to_weave_ema_ns = (wait_to_weave_ema_ns == 0.0)
				                           ? phi_ns
				                           : wait_to_weave_ema_ns * 0.9 + phi_ns * 0.1;
			}
			wait_frame_qpc = 0;
		}
		last_mark_qpc = now;
		if (dt_ns > 0.0) {
			interval_ema_ns =
			    (interval_ema_ns == 0.0) ? dt_ns : interval_ema_ns * 0.9 + dt_ns * 0.1;
		}

		/*
		 * #1339: a PACED app is not a saturated one. Under DXR_APP_FRAME_DIVISOR=D
		 * the runtime itself holds the app to every Dth vblank, so the app-mark
		 * interval this governor measures reads ~D periods BY DESIGN -- and the
		 * saturation test below cannot tell that from a pipeline that cannot make
		 * rate. Measured: at D=3 it escalated to max latency 3 within 4 s on every
		 * run, putting three refreshes between every weave (repaints included) and
		 * the glass; a blind eyeball preferred depth 1 twice ("very good" vs "not
		 * as good"), with the fill unchanged. Under the partition the pipeline that
		 * reaches the panel is the weave loop, which is at display rate; extra
		 * queue depth there buys no throughput and costs only motion-to-photon.
		 * Hold the initialised depth (base, normally 1) for the app's life.
		 * #1442: `paced` is the partition's OUTCOME (engaged vs refused), not
		 * the env var. There is still no unwind path: every tier that has a
		 * governor gates the partition create-once (split_active / reroute /
		 * split set at compositor create), so the outcome cannot flip mid-life
		 * on those tiers; if a dynamically-gated tier ever grows a governor,
		 * an ENGAGED->REFUSED flip would leave `effective` wherever it stood.
		 * An explicit DXR_LATE_WEAVE_MAX_LATENCY still wins (base != 1 above).
		 *
		 * SCOPE: this bites IN-PROCESS only. u_app_partition_divisor() is a
		 * process-local getenv, and the partition is not wired into the IPC path
		 * (u_app_partition.h: "deliberately NOT wired into the IPC/service path
		 * yet"), so in the service's copy of this governor `paced` reads the
		 * SERVICE's env, never a client's -- a no-op there, not a hold. The
		 * service-side behaviour arrives when the divisor becomes a per-client
		 * property sampled at weave-present (step 2 of #1339). Foot-gun: setting
		 * DXR_APP_FRAME_DIVISOR in the service's own environment would pin the
		 * workspace governor to depth 1 for EVERY client.
		 */
		/* #1442: the OUTCOME, not the env var — on a tier where the partition
		 * refused there is no grid and the app is not paced; holding the depth
		 * there disabled the governor with a WARN saying the opposite. */
		const bool paced = u_app_partition_state() == U_APP_PARTITION_ENGAGED;
		/* Only claim the hold when the partition is the REASON for it. With an
		 * explicit DXR_LATE_WEAVE_MAX_LATENCY (base != 1) or AUTOBACKOFF=0 the
		 * governor is off regardless, and attributing that depth to the partition
		 * makes the WARN a lying witness for the one A/B this code invites
		 * (pin 3 with the env var vs. the held 1). Reported by leaiss on #1342. */
		if (paced && base == 1 && auto_backoff == 1 && !paced_logged) {
			paced_logged = true;
			U_LOG_W("Late-weave: app is PACED by the frame partition (divisor %u) -- "
			        "governor held at max latency %d; the app-mark interval is not a "
			        "saturation signal here (#1339)",
			        u_app_partition_divisor(), effective);
		}
		if (paced || base != 1 || auto_backoff != 1 || period_ns <= 0.0 || interval_ema_ns <= 0.0) {
			return 0;
		}

		// Slip-rate escalation (see the field comment): jitter-dropped
		// frames the cost EMA cannot see. Judged per 120-mark window so a
		// single hiccup never escalates.
		if (dt_ns > 0.0) {
			slip_marks++;
			if (dt_ns > period_ns * 1.55) {
				slip_count++;
			}
			if (slip_marks >= 120) {
				const bool slipping = slip_count >= 10; // ~8%
				slip_marks = 0;
				slip_count = 0;
				clean_windows = slipping ? 0 : clean_windows + 1;
				if (slipping) {
					// Cap: one banked token above what the frame cost needs.
					int cap = LATE_WEAVE_MAX_DEPTH;
					if (slip_cap_enabled == 1) {
						const int need = needed_depth();
						cap = (need > 0 ? need : 1) + 1;
						if (cap > LATE_WEAVE_MAX_DEPTH) {
							cap = LATE_WEAVE_MAX_DEPTH;
						}
					}
					if (effective < cap) {
						// Re-escalating after a return probe = the probe
						// failed -> double the next dwell (the starvation
						// path below has always done this, at 5 s). Window =
						// two dwells: measured, the 2<->3 cycle re-escalated
						// ~55 s after each probe (dwell 30 s + 300 calm
						// frames), which a 30 s window never caught, so the
						// dwell never grew and the cycle ran forever (#1443).
						if (slip_cap_enabled == 1 && last_probe_qpc != 0 &&
						    (double)(now - last_probe_qpc) <
						        2.0 * ((double)probe_dwell_ns / 1e9) * (double)freq_hz) {
							probe_dwell_ns = probe_dwell_ns >= 150ull * 1000000000ull
							                     ? 300ull * 1000000000ull
							                     : probe_dwell_ns * 2;
						}
						const int from = effective;
						effective++;
						over_frames = 0;
						calm_frames = 0;
						backoff_qpc = now;
						log_change(now, freq_hz, from, "slip-rate escalation");
						return +1;
					}
					if (effective < LATE_WEAVE_MAX_DEPTH) {
						// Need-capped, not exhausted: keep the tick-align's
						// constant phase (review of #1441 — at the cap this
						// would otherwise ratchet the align off for 5 min).
						return 0;
					}
					// Depth is exhausted — the align's hard deadline
					// is the residual cost; yield it (see field
					// comment). Re-trigger within 60 s of the last
					// expiry = chronic — double the dwell.
					if (align_hold_clear_qpc != 0 &&
					    (double)(now - align_hold_clear_qpc) <
					        60.0 * (double)freq_hz) {
						align_hold_ns =
						    align_hold_ns >= 150ull * 1000000000ull
						        ? 300ull * 1000000000ull
						        : align_hold_ns * 2;
					} else {
						align_hold_ns = 10ull * 1000000000ull;
					}
					align_hold_until_qpc =
					    now + (uint64_t)((double)align_hold_ns / 1e9 *
					                     (double)freq_hz);
				}
			}
		}

		// Starved once the frame overruns what the CURRENT depth absorbs.
		// At depth 1 this is exactly the original >1.30×period rule.
		const bool starved = interval_ema_ns > period_ns * (double)effective * kLevelHeadroom;
		// Over-provisioned once the frame fits inside one fewer level, with a
		// little extra margin so the two tests can't chatter against
		// each other at the boundary.
		const bool roomy = effective > 1 && interval_ema_ns <
		                                        period_ns * (double)(effective - 1) * kLevelHeadroom * 0.95;

		if (starved && effective < LATE_WEAVE_MAX_DEPTH) {
			calm_frames = 0;
			over_frames++;
			if (over_frames >= 30) {
				// Backoff shortly after a return probe = the probe
				// failed → double the next dwell.
				if (last_probe_qpc != 0 &&
				    (double)(now - last_probe_qpc) * 1e9 / (double)freq_hz < 5e9) {
					probe_dwell_ns = probe_dwell_ns >= 150ull * 1000000000ull
					                     ? 300ull * 1000000000ull
					                     : probe_dwell_ns * 2;
				}
				// Jump straight to what the pipeline needs — on a fast
				// panel a heavy frame can want 3-4 levels at once, and
				// crawling up one per 30 frames would stall for seconds.
				const int want = needed_depth();
				const int from = effective;
				effective = (want > effective) ? want : effective + 1;
				over_frames = 0;
				backoff_qpc = now;
				log_change(now, freq_hz, from, "starvation backoff");
				return +1;
			}
		} else if (roomy) {
			over_frames = 0;
			calm_frames++;
			const double since_backoff_ns =
			    (double)(now - backoff_qpc) * 1e9 / (double)freq_hz;
			// Do not probe down into a pipeline the last slip window
			// showed still slipping — that probe fails by construction.
			// ...unless the depth already exceeds what the frame cost needs:
			// then the probe is the only way down and the dwell doubling,
			// not a ratchet, is what converges a chronic slipper.
			const bool slip_quiet =
			    (slip_cap_enabled != 1) || clean_windows >= 3 || effective > needed_depth();
			if (calm_frames >= 300 && since_backoff_ns > (double)probe_dwell_ns && slip_quiet) {
				// Step down one level at a time: the probe is what
				// re-tests the pipeline, so it must be gentle.
				const int from = effective;
				effective--;
				calm_frames = 0;
				last_probe_qpc = now;
				log_change(now, freq_hz, from, "return probe");
				return -1;
			}
		} else {
			over_frames = 0;
			calm_frames = 0;
		}
		return 0;
	}
};

/*!
 * Wait until the swap chain reports @p target_present has reached glass, or a
 * refresh-scaled deadline passes.
 *
 * The frame-latency waitable alone is not scanout: on composed presents it
 * releases at DWM *pickup*, 2-3 frames early. This poll closes that gap.
 *
 * Two things here are refresh-rate-shaped. The bound is 3 panel periods (not
 * a fixed iteration count), so an occluded window can never wedge us and a
 * fast panel is not given a 60 Hz-sized budget. And the idle step yields
 * rather than sleeps once the period is short: `Sleep(1)` can overshoot ~15 ms
 * without a raised timer resolution, which is under one refresh at 60 Hz but
 * 2.5 refreshes at 165 Hz — enough to miss the very frame we are pacing to.
 */
/*!
 * Result of one scanout-wait attempt, for the behavioral trust gate: a chain
 * whose GetFrameStatistics *fails* is cheap (we return immediately), but a
 * chain whose stats SUCCEED yet never advance would burn the full bound every
 * frame. Callers count consecutive TIMED_OUT results and stop calling after a
 * few — the stats are then declared unusable for this chain (composition
 * swapchains commonly behave this way; a one-shot probe cannot tell, since
 * DXGI also returns DISJOINT transiently on the first call after presenting).
 */
enum late_weave_scanout_result
{
	LATE_WEAVE_SCANOUT_REACHED,   //!< stats advanced to the target present
	LATE_WEAVE_SCANOUT_NO_STATS,  //!< GetFrameStatistics failed (free)
	LATE_WEAVE_SCANOUT_TIMED_OUT, //!< stats succeed but never advanced (paid full bound)
};

inline enum late_weave_scanout_result
late_weave_wait_scanout(IDXGISwapChain *sc, UINT target_present, double period_ns, uint64_t freq_hz)
{
	if (sc == nullptr || target_present == 0 || freq_hz == 0) {
		return LATE_WEAVE_SCANOUT_NO_STATS;
	}
	double bound_ns = (period_ns > 0.0) ? period_ns * 3.0 : 50e6;
	if (bound_ns < 5e6) {
		bound_ns = 5e6;
	} else if (bound_ns > 100e6) {
		bound_ns = 100e6;
	}
	LARGE_INTEGER start;
	QueryPerformanceCounter(&start);
	const uint64_t deadline =
	    (uint64_t)start.QuadPart + (uint64_t)(bound_ns * (double)freq_hz / 1000000000.0);

	for (;;) {
		DXGI_FRAME_STATISTICS stats = {};
		if (FAILED(sc->GetFrameStatistics(&stats))) {
			return LATE_WEAVE_SCANOUT_NO_STATS;
		}
		if (stats.PresentCount >= target_present) {
			return LATE_WEAVE_SCANOUT_REACHED;
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if ((uint64_t)now.QuadPart >= deadline) {
			return LATE_WEAVE_SCANOUT_TIMED_OUT;
		}
		if (period_ns > 0.0 && period_ns < 8e6) {
			SwitchToThread();
		} else {
			Sleep(1);
		}
	}
}

/*!
 * Composed-chain freshness stage (#833/live-path late-weave): a composition
 * swapchain's waitable releases at DWM *pickup* (~1 interval before scanout)
 * and its frame statistics are unreliable, so the scanout-proximate signal
 * there is DWM's own compositor clock. Waiting for the next tick right before
 * the weave records aligns the weave (and its eye prediction) to a constant
 * phase of the composition interval. Resolved dynamically — the export exists
 * on Win10 1809+; absence just degrades to waitable-only pacing.
 *
 * Returns true when it waited on the clock, false when unavailable.
 */
inline bool
late_weave_wait_compositor_clock(double period_ns)
{
	typedef HRESULT(WINAPI * pfn_wait_clock)(UINT, const HANDLE *, DWORD);
	static pfn_wait_clock s_fn = nullptr;
	static bool s_resolved = false;
	if (!s_resolved) {
		s_resolved = true;
		HMODULE m = GetModuleHandleW(L"dcomp.dll");
		if (m == nullptr) {
			m = LoadLibraryW(L"dcomp.dll");
		}
		if (m != nullptr) {
			s_fn = (pfn_wait_clock)GetProcAddress(m, "DCompositionWaitForCompositorClock");
		}
	}
	if (s_fn == nullptr) {
		return false;
	}
	// Bounded: a stopped compositor clock (occlusion) must not wedge the
	// render thread. 2 periods covers a missed tick; occluded windows hit
	// the timeout and the caller's occlusion guard handles the rest.
	DWORD timeout_ms = 34;
	if (period_ns > 0.0) {
		const double t = period_ns * 2.0 / 1e6;
		timeout_ms = (t < 4.0) ? 4 : (t > 100.0) ? 100 : (DWORD)t;
	}
	s_fn(0, nullptr, timeout_ms);
	return true;
}

inline void
weave_latency_log::after_present(const char *site, IDXGISwapChain *sc, struct late_weave_governor *gov)
{
		if (sc == nullptr) {
			return;
		}
		const bool csv = on(site);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);

		UINT present_count = 0;
		sc->GetLastPresentCount(&present_count);

		if (qpc_weave != 0) {
			// #206: the mark→present-return cost of the frame just pushed —
			// the headroom the forward predictor adds before snapping to the
			// next vblank.
			if ((uint64_t)now.QuadPart > qpc_weave) {
				// #1432: into THIS weave's population only — see the
				// field comment; repaints and app weaves must not share.
				if (pending_repaint) {
					headroom_repaint_qpc = (uint64_t)now.QuadPart - qpc_weave;
				} else {
					headroom_qpc = (uint64_t)now.QuadPart - qpc_weave;
				}
			}
			// Track for the timing loop.
			ring[ring_head] = {present_count, qpc_weave, pending_predicted_ns, pending_repaint,
			                   pending_horizon_ns, pending_horizon_base_qpc};
			ring_head = (ring_head + 1) % 8;
			if (ring_count < 8) {
				ring_count++;
			}
			if (csv) {
				// #868: trailing field marks a repaint (re-weave of an
				// unchanged atlas) so the two present populations can be
				// counted apart. Older readers ignore the extra column.
				fprintf(f, "F,%llu,%llu,%llu,%u,%d\n", (unsigned long long)seq++,
				        (unsigned long long)qpc_weave, (unsigned long long)now.QuadPart,
				        present_count, pending_repaint ? 1 : 0);
			}
			qpc_weave = 0;
			pending_repaint = false;
			pending_horizon_ns = 0;
			pending_horizon_base_qpc = 0;
		}

		DXGI_FRAME_STATISTICS stats = {};
		if (SUCCEEDED(sc->GetFrameStatistics(&stats))) {
			if (gov != nullptr) {
				gov->on_stats(stats, freq());
				if (hz_depth != 0 && gov->effective != hz_depth) {
					hz_depth_changes++;
				}
				hz_depth = gov->effective;
			}
			// #206: capture the vsync-locked vblank grid, always-on. The
			// period comes from the statistics themselves — SyncQPCTime
			// delta over SyncRefreshCount delta — so 59.94 vs 60.00 is
			// measured, not assumed. Monotone guard mirrors the #1051 CSV
			// rule: a repeated/stale sync sample must not move the grid.
			if ((uint64_t)stats.SyncQPCTime.QuadPart > last_sync_qpc) {
				const uint64_t sync = (uint64_t)stats.SyncQPCTime.QuadPart;
				const uint64_t refresh = (uint64_t)stats.SyncRefreshCount;
				if (last_sync_qpc != 0 && refresh > last_sync_refresh) {
					const uint64_t cand =
					    (sync - last_sync_qpc) / (refresh - last_sync_refresh);
					// Sanity: 1..50 ms per vblank (20-1000 Hz panels).
					const uint64_t f2 = freq();
					if (cand > f2 / 1000 && cand < f2 / 20) {
						refresh_period_qpc = cand;
					}
				}
				last_sync_qpc = sync;
				last_sync_refresh = refresh;
			}
			// Resolve the newest ring entry whose present has flipped.
			for (int i = 0; i < ring_count; i++) {
				int idx = (ring_head - 1 - i + 16) % 8;
				if (ring[idx].present_count != 0 && ring[idx].present_count <= stats.PresentCount &&
				    (uint64_t)stats.SyncQPCTime.QuadPart > ring[idx].qpc) {
					/*
					 * #868: only an APP weave updates the residual the
					 * display processor is handed via set_frame_timing.
					 *
					 * A repaint's weave→scanout is a real measurement, but
					 * it belongs to a different population — repaints pace
					 * to one panel period while app frames sit at the
					 * governor's depth. Letting both feed one value makes
					 * it alternate; the vendor eye predictor then
					 * extrapolates to a different horizon on alternate
					 * frames, and an IDENTICAL atlas weaves to different
					 * interlace. That is the flicker.
					 *
					 * Measured on cube_handle_d3d11_win with forced
					 * repaints: adjacent repaints reproduced the app frame
					 * 23/1080 with repaints feeding this, and 909/960 with
					 * the residual held to app frames only.
					 *
					 * The same rule already governs the saturation EMA and
					 * the #867 prediction ledger — this was the one ledger
					 * repaints were still polluting.
					 */
					if (!ring[idx].repaint) {
						measured_r_ns =
						    (uint64_t)((double)((uint64_t)stats.SyncQPCTime.QuadPart -
						                        ring[idx].qpc) *
						               1000000000.0 / (double)freq());
					}
					// #867: xrWaitFrame's promise vs this frame's real
					// photon time. os_monotonic_get_ns() is QPC scaled
					// to ns on Windows, so the two share an origin and
					// the difference is the prediction error directly.
					if (csv && ring[idx].predicted_ns != 0) {
						const double scanout_ns =
						    (double)(uint64_t)stats.SyncQPCTime.QuadPart *
						    1000000000.0 / (double)freq();
						fprintf(f, "D,%llu,%llu,%lld,%lld\n",
						        (unsigned long long)seq,
						        (unsigned long long)ring[idx].present_count,
						        (long long)ring[idx].predicted_ns,
						        (long long)((int64_t)scanout_ns -
						                    (int64_t)ring[idx].predicted_ns));
					}
					break;
				}
			}
			/*
			 * #1432: predicted vs realised horizon — its OWN pass, not the
			 * loop above. That loop breaks on the newest entry whose weave
			 * precedes SyncQPCTime, which on the #1051 stale-sync configs is
			 * an OLDER present than stats.PresentCount; the exact entry we
			 * need would never be reached. And it re-selects the same entry
			 * on every repeated statistics sample, which is fine for the
			 * idempotent consumers above but would double-count here — so
			 * the entry is CONSUMED (horizon_ns = 0) once resolved.
			 *
			 * Exact PresentCount only: a skipped-ahead SyncQPCTime belongs
			 * to a later present and would read as a whole-period miss that
			 * never happened. Runs when the trace OR the #1435 loop is on
			 * (the loop is default-on, so in practice always): one scan of
			 * <= 8 ring entries per present.
			 *
			 * SyncQPCTime is the time of the LATEST vsync at sampling
			 * (SyncRefreshCount), NOT the vblank this present flipped on
			 * (PresentRefreshCount). Sampled after Present returns, the
			 * two are typically one refresh apart, so the raw difference
			 * over-reads the realised horizon by whole periods — the first
			 * live run showed 100% "wrong slot" at exactly +1 period.
			 * Walk SyncQPCTime back by the refresh-count gap to the flip.
			 */
			if ((hz_trace == HZ_ON || po_enabled == PO_ON) && refresh_period_qpc != 0 &&
			    stats.SyncRefreshCount >= stats.PresentRefreshCount) {
				const uint32_t lag = stats.SyncRefreshCount - stats.PresentRefreshCount;
				const uint64_t flip_qpc =
				    (uint64_t)stats.SyncQPCTime.QuadPart - (uint64_t)lag * refresh_period_qpc;
				for (int i = 0; i < ring_count; i++) {
					int idx = (ring_head - 1 - i + 16) % 8;
					if (ring[idx].horizon_ns != 0 && ring[idx].present_count == stats.PresentCount &&
					    flip_qpc > ring[idx].horizon_base_qpc) {
						const double k = 1000000000.0 / (double)freq();
						const int64_t realised_ns =
						    (int64_t)((double)(flip_qpc - ring[idx].horizon_base_qpc) * k);
						const int64_t err_ns = realised_ns - (int64_t)ring[idx].horizon_ns;
						const uint64_t period_ns = (uint64_t)((double)refresh_period_qpc * k);
						note_horizon_outcome(err_ns, period_ns, lag);
						// #1435: residual in whole periods, into the loop.
						const double kf = (double)err_ns / (double)period_ns;
						po_observe((int)(kf >= 0.0 ? kf + 0.5 : kf - 0.5), period_ns);
						ring[idx].horizon_ns = 0; // consumed
						break;
					}
				}
			}
			// #1051: on slow-app configs GetFrameStatistics can report an
			// ADVANCED PresentCount paired with a STALE SyncQPCTime (an
			// older flip's) — writing that raw joins offline as scanout
			// BEFORE weave, a physical impossibility (up to 97.8% of rows).
			// The internal consumer above is already guarded (sync_qpc >
			// ring[idx].qpc); gate only the CSV row the same way: a
			// strictly-increasing sync suppresses the repeated/stale
			// samples while every genuinely-resolved flip still emits once.
			if (csv && (uint64_t)stats.SyncQPCTime.QuadPart > last_s_sync_qpc) {
				last_s_sync_qpc = (uint64_t)stats.SyncQPCTime.QuadPart;
				fprintf(f, "S,%u,%u,%u,%llu,%llu\n", stats.PresentCount,
				        stats.PresentRefreshCount, stats.SyncRefreshCount,
				        (unsigned long long)stats.SyncQPCTime.QuadPart,
				        (unsigned long long)now.QuadPart);
			}
		}
		if (csv) {
			fflush(f);
		}
}

#endif // _WIN32
