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
	after_present(const char *site, IDXGISwapChain *sc)
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
};

#endif // _WIN32
