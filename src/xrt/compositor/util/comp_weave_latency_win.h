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

struct weave_latency_log
{
	FILE *f = nullptr;
	int enabled = -1; // -1 = unprobed (CSV only; R-tracking is always on)
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
	};
	pending ring[8] = {};
	int ring_head = 0;
	int ring_count = 0;
	uint64_t measured_r_ns = 0; // last completed frame's weave→scanout; 0 = unknown

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
		if (enabled < 0) {
			const char *prefix = getenv("DXR_WEAVE_LATENCY_CSV");
			enabled = 0;
			if (prefix != nullptr && prefix[0] != '\0') {
				char path[MAX_PATH];
				snprintf(path, sizeof(path), "%s.%s.csv", prefix, site);
				f = fopen(path, "a");
				if (f != nullptr) {
					fprintf(f, "H,%lld\n", (long long)freq());
					enabled = 1;
				}
			}
		}
		return enabled == 1;
	}

	void
	mark_weave(const char *site)
	{
		(void)on(site);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		qpc_weave = (uint64_t)now.QuadPart;
	}

	void
	after_present(const char *site, IDXGISwapChain *sc, struct late_weave_governor *gov = nullptr);
};

/*!
 * Late-weave latency governor (#850).
 *
 * maxFrameLatency=1 removes all CPU/GPU frame overlap. That is the point on a
 * pipeline with headroom (R collapses to one refresh) — but on a pipeline that
 * cannot make rate it serializes the whole frame: measured −63% fps for ~4 ms
 * of R p50 on a saturated app, with no p95 win. Two escapes:
 *
 *  - DXR_LATE_WEAVE_MAX_LATENCY=N (1..3, default 1) forces a fixed queue
 *    depth. N>1 restores N−1 frames of overlap at the cost of (N−1) refresh
 *    intervals of extra weave-time eye-prediction horizon.
 *  - Saturation auto-backoff (default ON; DXR_LATE_WEAVE_AUTOBACKOFF=0
 *    disables; only active at the default depth 1): when the paced frame
 *    interval persistently exceeds the measured display period, effective
 *    depth goes to 2. Once the pipeline holds rate again the governor probes
 *    a return to 1 — with a dwell that doubles on every failed probe, so a
 *    persistently saturated app converges to depth 2 instead of oscillating.
 *
 * The caller owns applying transitions (SetMaximumFrameLatency + one-shot
 * WARN — silent backoff would make gate numbers non-reproducible) and relaxes
 * its scanout pacer by (effective−1) presents.
 */
struct late_weave_governor
{
	int base = -1;         // DXR_LATE_WEAVE_MAX_LATENCY, probed once (1..3)
	int auto_backoff = -1; // DXR_LATE_WEAVE_AUTOBACKOFF, default 1
	int effective = 1;

	// Saturation signal: EMA of weave-mark→weave-mark wall time, judged
	// against the display period derived from DXGI frame statistics.
	uint64_t last_mark_qpc = 0;
	double interval_ema_ns = 0.0;
	double period_ns = 0.0;
	uint64_t last_sync_qpc = 0;
	UINT last_sync_refresh = 0;

	int over_frames = 0;
	int calm_frames = 0;
	uint64_t backoff_qpc = 0;
	uint64_t last_probe_qpc = 0;
	uint64_t probe_dwell_ns = 30ull * 1000000000ull; // ×2 per failed probe, cap 5 min

	int
	base_latency()
	{
		if (base < 0) {
			const char *e = getenv("DXR_LATE_WEAVE_MAX_LATENCY");
			int v = (e != nullptr && e[0] != '\0') ? atoi(e) : 1;
			base = v < 1 ? 1 : (v > 3 ? 3 : v);
			const char *a = getenv("DXR_LATE_WEAVE_AUTOBACKOFF");
			auto_backoff = (a != nullptr && a[0] == '0') ? 0 : 1;
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
			if (per > 4e6 && per < 5e7) { // 200 Hz .. 20 Hz sanity window
				period_ns = (period_ns == 0.0) ? per : period_ns * 0.9 + per * 0.1;
			}
		}
		last_sync_qpc = sync_qpc;
		last_sync_refresh = stats.SyncRefreshCount;
	}

	//! Call once per paced frame (end of the weave-mark pacer). Returns +1 on
	//! a backoff engage (1→2), −1 on a return probe (2→1), 0 otherwise; the
	//! caller applies SetMaximumFrameLatency and reads `effective`.
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
				dt_ns = 0.0;
			}
		}
		last_mark_qpc = now;
		if (dt_ns > 0.0) {
			interval_ema_ns =
			    (interval_ema_ns == 0.0) ? dt_ns : interval_ema_ns * 0.9 + dt_ns * 0.1;
		}

		if (base != 1 || auto_backoff != 1 || period_ns <= 0.0 || interval_ema_ns <= 0.0) {
			return 0;
		}

		if (effective == 1) {
			over_frames = (interval_ema_ns > period_ns * 1.30) ? over_frames + 1 : 0;
			if (over_frames >= 30) {
				// Backoff shortly after a return probe = the probe
				// failed → double the next dwell.
				if (last_probe_qpc != 0 &&
				    (double)(now - last_probe_qpc) * 1e9 / (double)freq_hz < 5e9) {
					probe_dwell_ns = probe_dwell_ns >= 150ull * 1000000000ull
					                     ? 300ull * 1000000000ull
					                     : probe_dwell_ns * 2;
				}
				effective = 2;
				over_frames = 0;
				calm_frames = 0;
				backoff_qpc = now;
				return +1;
			}
		} else {
			calm_frames = (interval_ema_ns < period_ns * 1.05) ? calm_frames + 1 : 0;
			const double since_backoff_ns =
			    (double)(now - backoff_qpc) * 1e9 / (double)freq_hz;
			if (calm_frames >= 300 && since_backoff_ns > (double)probe_dwell_ns) {
				effective = 1;
				calm_frames = 0;
				last_probe_qpc = now;
				return -1;
			}
		}
		return 0;
	}
};

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
			// Track for the timing loop.
			ring[ring_head] = {present_count, qpc_weave};
			ring_head = (ring_head + 1) % 8;
			if (ring_count < 8) {
				ring_count++;
			}
			if (csv) {
				fprintf(f, "F,%llu,%llu,%llu,%u\n", (unsigned long long)seq++,
				        (unsigned long long)qpc_weave, (unsigned long long)now.QuadPart,
				        present_count);
			}
			qpc_weave = 0;
		}

		DXGI_FRAME_STATISTICS stats = {};
		if (SUCCEEDED(sc->GetFrameStatistics(&stats))) {
			if (gov != nullptr) {
				gov->on_stats(stats, freq());
			}
			// Resolve the newest ring entry whose present has flipped.
			for (int i = 0; i < ring_count; i++) {
				int idx = (ring_head - 1 - i + 16) % 8;
				if (ring[idx].present_count != 0 && ring[idx].present_count <= stats.PresentCount &&
				    (uint64_t)stats.SyncQPCTime.QuadPart > ring[idx].qpc) {
					measured_r_ns = (uint64_t)(
					    (double)((uint64_t)stats.SyncQPCTime.QuadPart - ring[idx].qpc) *
					    1000000000.0 / (double)freq());
					break;
				}
			}
			if (csv) {
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
