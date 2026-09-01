// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Frame witness — per-interval weave/present/repaint counters
 *         (env-gated, header-only, zero cost when off).
 *
 * DXR_FRAME_WITNESS=1 (or =N seconds, 1..60; default interval 5 s) makes each
 * native compositor emit one throttled log line per interval:
 *
 *   [WITNESS] site=d3d11 window=5.0s presents/s=60.0 weaves/s=22.2
 *             repaints/s=37.8 mode=3d
 *
 * Rationale (#1044 / perf-decomposition ladder): the weave-latency harness
 * measures weave→scanout, which requires presentation-timing machinery that is
 * absent on Intel VK (no present_wait) and structurally unmeasurable on
 * composed/DComp chains (frame statistics DISJOINT) — so "the weave held panel
 * rate" was an inference on exactly the configurations that matter. These are
 * plain counters at the weave/present choke points, independent of the
 * presentation path and of any timing extension, so they hold on every
 * backend, adapter, and chain type.
 *
 * Population split: `weaves` counts app-frame weaves (a fresh atlas behind the
 * weave), `repaints` counts #868 re-weaves of an unchanged atlas. Total DP
 * rate = weaves + repaints. `mode` reports what the weaves actually ran as
 * (2d flat blit vs 3d weave), so a harness arm can verify its configured mode
 * instead of assuming it.
 *
 * Threading: counted from the app frame thread and the repaint thread; relaxed
 * atomics, and the emitter claims the window with a CAS so two presenters
 * cannot double-report. Counts drop with at most ±1 skew per window, which is
 * irrelevant at a 5 s aggregate.
 *
 * @ingroup comp_util
 */

#pragma once

#include <atomic>
#include <cstdlib>
#include <cstdint>

#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_setting.h"

struct comp_frame_witness
{
	const char *site;
	std::atomic<int> enabled{-1}; // -1 = unprobed, 0 = off, 1 = on
	int64_t interval_ns = 0;
	std::atomic<int64_t> window_start_ns{0};
	std::atomic<uint32_t> weaves{0};   // app-frame weaves
	std::atomic<uint32_t> repaints{0}; // #868 repaints (no app frame behind them)
	std::atomic<uint32_t> presents{0};
	std::atomic<uint32_t> weaves_3d{0}; // subset of weaves+repaints that wove 3D

	bool
	on()
	{
		int e = enabled.load(std::memory_order_relaxed);
		if (e < 0) {
			// #1252: settings chain (env > per-user > machine). This is one of
			// the two levers the Control Panel's Diagnostics toggle drives.
			char buf[64];
			const char *v = u_setting_get_raw("DXR_FRAME_WITNESS", buf, sizeof(buf), nullptr);
			long secs = (v != nullptr && v[0] != '\0') ? atol(v) : 0;
			if (secs < 0 || secs > 60) {
				secs = 0;
			}
			if (secs == 1) {
				secs = 5; // "=1" means enabled-at-default, not 1 s
			}
			interval_ns = (int64_t)secs * 1000000000LL;
			e = (secs > 0) ? 1 : 0;
			enabled.store(e, std::memory_order_relaxed);
		}
		return e == 1;
	}

	void
	count_weave(bool repaint, bool mode_3d)
	{
		if (!on()) {
			return;
		}
		(repaint ? repaints : weaves).fetch_add(1, std::memory_order_relaxed);
		if (mode_3d) {
			weaves_3d.fetch_add(1, std::memory_order_relaxed);
		}
	}

	//! Count a present; the interval elapsing emits the witness line.
	//! Presents are the heartbeat, so a fully wedged pipeline goes silent —
	//! which is itself the signal (absence of [WITNESS] lines ≠ healthy).
	void
	count_present()
	{
		if (!on()) {
			return;
		}
		presents.fetch_add(1, std::memory_order_relaxed);
		const int64_t now = (int64_t)os_monotonic_get_ns();
		int64_t start = window_start_ns.load(std::memory_order_relaxed);
		if (start == 0) {
			// First present opens the window (never emits).
			window_start_ns.compare_exchange_strong(start, now, std::memory_order_relaxed);
			return;
		}
		if (now - start < interval_ns) {
			return;
		}
		// Claim the window; the loser just keeps counting into the next one.
		if (!window_start_ns.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
			return;
		}
		const uint32_t w = weaves.exchange(0, std::memory_order_relaxed);
		const uint32_t r = repaints.exchange(0, std::memory_order_relaxed);
		const uint32_t p = presents.exchange(0, std::memory_order_relaxed);
		const uint32_t w3 = weaves_3d.exchange(0, std::memory_order_relaxed);
		const double secs = (double)(now - start) / 1e9;
		if (secs <= 0.0) {
			return;
		}
		const char *mode = (w + r) == 0 ? "idle" : (w3 == 0 ? "2d" : (w3 == w + r ? "3d" : "mixed"));
		U_LOG_W("[WITNESS] site=%s window=%.1fs presents/s=%.1f weaves/s=%.1f repaints/s=%.1f mode=%s", site,
		        secs, (double)p / secs, (double)w / secs, (double)r / secs, mode);
	}
};
