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
	int enabled = -1; // -1 = unprobed
	uint64_t seq = 0;
	uint64_t qpc_weave = 0; // armed by mark_weave, consumed by after_present

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
					LARGE_INTEGER freq;
					QueryPerformanceFrequency(&freq);
					fprintf(f, "H,%lld\n", (long long)freq.QuadPart);
					enabled = 1;
				}
			}
		}
		return enabled == 1;
	}

	void
	mark_weave(const char *site)
	{
		if (!on(site)) {
			return;
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		qpc_weave = (uint64_t)now.QuadPart;
	}

	void
	after_present(const char *site, IDXGISwapChain *sc)
	{
		if (!on(site) || sc == nullptr) {
			return;
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (qpc_weave != 0) {
			UINT present_count = 0;
			sc->GetLastPresentCount(&present_count);
			fprintf(f, "F,%llu,%llu,%llu,%u\n", (unsigned long long)seq++,
			        (unsigned long long)qpc_weave, (unsigned long long)now.QuadPart, present_count);
			qpc_weave = 0;
		}
		DXGI_FRAME_STATISTICS stats = {};
		if (SUCCEEDED(sc->GetFrameStatistics(&stats))) {
			fprintf(f, "S,%u,%u,%u,%llu,%llu\n", stats.PresentCount, stats.PresentRefreshCount,
			        stats.SyncRefreshCount, (unsigned long long)stats.SyncQPCTime.QuadPart,
			        (unsigned long long)now.QuadPart);
		}
		fflush(f);
	}
};

#endif // _WIN32
