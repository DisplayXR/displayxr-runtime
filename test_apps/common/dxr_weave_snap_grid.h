// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A drag snap provider that fetches the Wayland drag-lattice probe as ONE
 *         xrWeaveSnapWindowGridDXR call and answers the per-point callback from
 *         the cached table (runtime#1723).
 *
 * ## Why
 *
 * displayxr-common's window helper builds the Wayland drag lattice at every
 * title-bar press by asking its SnapWindowOriginFn about a 129 x 129 grid of
 * displacements (±192 logical px every 3, plus a small reachable-lattice search
 * around answers the compositor cannot place). Each ask is one
 * xrWeaveSnapWindowRectDXR — a service round trip for an IPC client. 16,641 of
 * them measured 2.3–2.5 s per press. xrWeaveSnapWindowGridDXR (XR_DXR_weave
 * spec v11) runs the same per-point snap in the runtime, next to the display
 * processor, and returns the whole grid in one round trip.
 *
 * The helper's seam stays exactly what it is — a per-point callback — so this
 * class changes nothing in displayxr-common. It is a MEMO: every answer it
 * hands out is the display processor's own answer for that exact target,
 * fetched in bulk a moment earlier. A query it cannot answer from a table goes
 * to xrWeaveSnapWindowRectDXR as before, so a wrong guess about the probe's
 * shape costs speed, never correctness.
 *
 * ## Recognising the probe (the one coupling to the helper)
 *
 * The callback sees single points; to fetch a table it must know the grid. The
 * helper probes with origin (0, 0) and targets = the DEVICE displacement of a
 * logical grid (dxr_wl_lattice.h). Two shapes are recognised:
 *
 *  - A FRESH table (every press, and the mid-drag table on reaching the panel)
 *    is centred on (0, 0), so its first query is the displacement of logical
 *    (-192, -192). At an integer scale s that is exactly (-192 s, -192 s): s is
 *    read off the first query and the table is fetched before the probe asks a
 *    second question. At a fractional scale the first query is still ~-192·s on
 *    both axes, which bounds the table; the dense device-pixel box is fetched.
 *  - An EXTENSION table (the drag outran the first one) starts anywhere. Its
 *    first query is answered per point; the next query on the same row, 3 s
 *    further right, names the step. (A reachable-lattice search around the
 *    first cell can in principle come in between and mislead this at s >= 3;
 *    the memo is then just less useful.)
 *
 * What is fetched per scale — chosen so ONE call covers every question the
 * helper will ask about that table:
 *
 *  - s = 1: the 129 x 129 grid itself (step 3). Every DP answer is a whole
 *    logical pixel at s = 1, so the helper never searches around one.
 *  - integer s >= 2: every s-th device pixel over the table ±6 logical px
 *    (397 x 397 points, step s), which contains the grid AND every point of the
 *    helper's ±2 logical-px search around any answer.
 *  - fractional s: every device pixel over the table plus a margin (bounded by
 *    XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR; beyond it the table is not fetched).
 *
 * Constants below mirror dxr_wl_lattice's kLatticeHalf / kLatticeCell. If
 * displayxr-common changes them, this class stops recognising the probe and
 * every query goes per point — the pre-#1723 behaviour, correct and slow.
 *
 * Thread-safe: the helper probes on a worker thread while the X11 drag and the
 * app's own drop snap call from the main thread.
 */
#pragma once

#include <openxr/openxr.h>
#include <openxr/XR_DXR_weave.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

class DxrWeaveSnapGrid
{
public:
	//! Mirrors displayxr-common dxr_linux_window.cpp kLatticeHalf / kLatticeCell.
	static constexpr int32_t kProbeHalf = 192;
	static constexpr int32_t kProbeCell = 3;
	static constexpr int32_t kProbeCount = 2 * kProbeHalf / kProbeCell + 1; // 129
	//! A table older than this is never served (the DP's answer depends on the
	//! viewing distance, which moves).
	static constexpr std::chrono::milliseconds kTableTtl{5000};

	struct Stats
	{
		uint64_t queries = 0;        //!< callback invocations
		uint64_t from_table = 0;     //!< answered from a fetched table
		uint64_t per_point_ipc = 0;  //!< xrWeaveSnapWindowRectDXR calls
		uint64_t grid_ipc = 0;       //!< xrWeaveSnapWindowGridDXR calls
		uint64_t grid_points = 0;    //!< points those calls returned
		double grid_ms = 0.0;        //!< wall time inside xrWeaveSnapWindowGridDXR
		double grid_ms_max = 0.0;
	};

	/*!
	 * Resolve both entry points. Safe against a runtime without XR_DXR_weave
	 * (identity) or with a pre-v11 one (no grid: per point, as before).
	 */
	void
	attach(XrInstance instance, XrSession session, uint32_t extent_w, uint32_t extent_h)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_session = session;
		m_w = (int32_t)extent_w;
		m_h = (int32_t)extent_h;
		m_pfn_rect = nullptr;
		m_pfn_grid = nullptr;
		m_have_table = false;
		m_have_p1 = false;
		if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE) {
			return;
		}
		PFN_xrVoidFunction fn = nullptr;
		if (xrGetInstanceProcAddr(instance, "xrWeaveSnapWindowRectDXR", &fn) == XR_SUCCESS && fn != nullptr) {
			m_pfn_rect = reinterpret_cast<PFN_xrWeaveSnapWindowRectDXR>(fn);
		}
		fn = nullptr;
		if (xrGetInstanceProcAddr(instance, "xrWeaveSnapWindowGridDXR", &fn) == XR_SUCCESS && fn != nullptr) {
			m_pfn_grid = reinterpret_cast<PFN_xrWeaveSnapWindowGridDXR>(fn);
		}
		if (getenv("DXR_WEAVE_SNAP_GRID") != nullptr && getenv("DXR_WEAVE_SNAP_GRID")[0] == '0') {
			m_pfn_grid = nullptr; // A/B: the per-point probe
		}
	}

	//! attach() with the entry points already resolved (unit tests, or an app
	//! that resolves them itself).
	void
	attach_functions(XrSession session,
	                 PFN_xrWeaveSnapWindowRectDXR pfn_rect,
	                 PFN_xrWeaveSnapWindowGridDXR pfn_grid,
	                 uint32_t extent_w,
	                 uint32_t extent_h)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_session = session;
		m_pfn_rect = pfn_rect;
		m_pfn_grid = pfn_grid;
		m_w = (int32_t)extent_w;
		m_h = (int32_t)extent_h;
		m_have_table = false;
		m_have_p1 = false;
	}

	void
	set_extent(uint32_t w, uint32_t h)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_w = (int32_t)w;
		m_h = (int32_t)h;
	}

	//! The displayxr-common SnapWindowOriginFn. `userdata` is a DxrWeaveSnapGrid*.
	static bool
	callback(void *userdata,
	         int32_t origin_x,
	         int32_t origin_y,
	         int32_t target_x,
	         int32_t target_y,
	         int32_t *out_x,
	         int32_t *out_y)
	{
		DxrWeaveSnapGrid *self = static_cast<DxrWeaveSnapGrid *>(userdata);
		if (self == nullptr) {
			return false;
		}
		std::lock_guard<std::mutex> lock(self->m_mutex);
		return self->snap_locked(origin_x, origin_y, target_x, target_y, out_x, out_y);
	}

	bool
	available() const
	{
		return m_pfn_rect != nullptr;
	}

	bool
	grid_available() const
	{
		return m_pfn_grid != nullptr;
	}

	Stats
	stats() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_total;
	}

	/*!
	 * Log what the current table cost and start counting afresh. Called
	 * automatically when the next table begins; call it at shutdown for the
	 * last one.
	 */
	void
	flush_log()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		log_table_locked();
	}

private:
	struct Table
	{
		int32_t first_x = 0, first_y = 0, step_x = 1, step_y = 1;
		uint32_t count_x = 0, count_y = 0;
		bool declined = false;
		std::vector<XrWeaveSnapGridPointDXR> pts;

		bool
		lookup(int32_t tx, int32_t ty, int32_t *sx, int32_t *sy, bool *declined_out) const
		{
			const int64_t ix = (int64_t)tx - first_x, iy = (int64_t)ty - first_y;
			if (ix < 0 || iy < 0 || ix % step_x != 0 || iy % step_y != 0) {
				return false;
			}
			const int64_t i = ix / step_x, j = iy / step_y;
			if (i >= (int64_t)count_x || j >= (int64_t)count_y) {
				return false;
			}
			if (declined) {
				*declined_out = true;
				return true;
			}
			const XrWeaveSnapGridPointDXR &p = pts[(size_t)j * count_x + (size_t)i];
			if (p.dx == XR_WEAVE_SNAP_GRID_NO_DELTA_DXR) {
				return false; // too far to encode: ask this one per point
			}
			*declined_out = false;
			*sx = tx + p.dx;
			*sy = ty + p.dy;
			return true;
		}
	};

	bool
	per_point_locked(int32_t ox, int32_t oy, int32_t tx, int32_t ty, int32_t *out_x, int32_t *out_y)
	{
		if (m_pfn_rect == nullptr || m_session == XR_NULL_HANDLE) {
			return false;
		}
		XrRect2Di origin = {};
		origin.offset.x = ox;
		origin.offset.y = oy;
		origin.extent.width = m_w;
		origin.extent.height = m_h;
		XrRect2Di target = origin;
		target.offset.x = tx;
		target.offset.y = ty;
		XrRect2Di snapped = {};
		m_table_stats.per_point_ipc++;
		m_total.per_point_ipc++;
		const XrResult res = m_pfn_rect(m_session, &origin, &target, &snapped);
		if (res != XR_SUCCESS) {
			if (!m_failed) {
				m_failed = true;
				fprintf(stderr,
				        "[WARN]  xrWeaveSnapWindowRectDXR failed (%d) — drag falls back to an "
				        "unsnapped window position for the rest of this run\n",
				        (int)res);
				fflush(stderr);
			}
			m_pfn_rect = nullptr;
			m_pfn_grid = nullptr;
			return false;
		}
		*out_x = snapped.offset.x;
		*out_y = snapped.offset.y;
		return true;
	}

	//! One xrWeaveSnapWindowGridDXR (origin (0, 0), the probe's frame).
	bool
	fetch_locked(int32_t first_x, int32_t first_y, int32_t step, uint32_t count_x, uint32_t count_y, const char *why)
	{
		if (m_pfn_grid == nullptr || count_x == 0 || count_y == 0 ||
		    count_x > XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR || count_y > XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR) {
			return false;
		}
		XrWeaveSnapGridInfoDXR info = {};
		info.type = XR_TYPE_WEAVE_SNAP_GRID_INFO_DXR;
		info.originRect.extent.width = m_w;
		info.originRect.extent.height = m_h;
		info.firstTarget.x = first_x;
		info.firstTarget.y = first_y;
		info.step.width = step;
		info.step.height = step;
		info.countX = count_x;
		info.countY = count_y;

		Table t;
		t.first_x = first_x;
		t.first_y = first_y;
		t.step_x = step;
		t.step_y = step;
		t.count_x = count_x;
		t.count_y = count_y;
		t.pts.resize((size_t)count_x * count_y);
		uint32_t n = 0;
		XrBool32 declined = XR_FALSE;
		const auto t0 = std::chrono::steady_clock::now();
		const XrResult res = m_pfn_grid(m_session, &info, (uint32_t)t.pts.size(), &n, t.pts.data(), &declined);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		m_table_stats.grid_ipc++;
		m_total.grid_ipc++;
		if (res != XR_SUCCESS || n != t.pts.size()) {
			fprintf(stderr, "[WARN]  xrWeaveSnapWindowGridDXR failed (%d) — snapping per point from now on\n",
			        (int)res);
			fflush(stderr);
			m_pfn_grid = nullptr;
			return false;
		}
		t.declined = declined == XR_TRUE;
		m_table_stats.grid_points += n;
		m_total.grid_points += n;
		m_table_stats.grid_ms += ms;
		m_total.grid_ms += ms;
		if (ms > m_total.grid_ms_max) {
			m_total.grid_ms_max = ms;
		}
		fprintf(stderr,
		        "[INFO]  grid snap: %s — %ux%u points, step %d px, from (%+d, %+d): %.2f ms in ONE "
		        "xrWeaveSnapWindowGridDXR%s\n",
		        why, count_x, count_y, step, first_x, first_y, ms,
		        t.declined ? " (the display processor DECLINED: nothing to constrain)" : "");
		fflush(stderr);
		m_table = std::move(t);
		m_have_table = true;
		m_table_time = std::chrono::steady_clock::now();
		return true;
	}

	/*!
	 * The table for a probe whose grid corner is (x0, y0) at integer scale s.
	 * s = 1: the grid itself. s >= 2: every s-th pixel, ±6 logical px wider,
	 * so the helper's reachable-lattice search is inside it too.
	 */
	bool
	fetch_integer_locked(int32_t x0, int32_t y0, int32_t s, const char *why)
	{
		if (s == 1) {
			return fetch_locked(x0, y0, kProbeCell, kProbeCount, kProbeCount, why);
		}
		// logical px: the helper searches ±2 around the DP's answer mapped
		// back to logical, and that answer is itself a few px off the grid.
		const int32_t pad = 6;
		const uint32_t n = (uint32_t)(2 * kProbeHalf + 2 * pad + 1);
		return fetch_locked(x0 - pad * s, y0 - pad * s, s, n, n, why);
	}

	//! Every device pixel of [x0, x0 + span] x [y0, y0 + span], plus a margin.
	bool
	fetch_dense_locked(int32_t x0, int32_t y0, int32_t span_x, int32_t span_y, const char *why)
	{
		const int32_t margin = 16; // ±2 logical px of search + the DP's own few px, at any scale < 4
		const int64_t cx = (int64_t)span_x + 2 * margin + 1, cy = (int64_t)span_y + 2 * margin + 1;
		if (cx > XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR || cy > XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR) {
			return false;
		}
		return fetch_locked(x0 - margin, y0 - margin, 1, (uint32_t)cx, (uint32_t)cy, why);
	}

	void
	log_table_locked()
	{
		if (m_table_stats.queries == 0) {
			return;
		}
		const Stats &s = m_table_stats;
		fprintf(stderr,
		        "[INFO]  grid snap: table done — %llu queries, %llu from the fetched table, %llu IPC call(s) "
		        "(%llu grid + %llu per point), %.2f ms in grid fetches\n",
		        (unsigned long long)s.queries, (unsigned long long)s.from_table,
		        (unsigned long long)(s.grid_ipc + s.per_point_ipc), (unsigned long long)s.grid_ipc,
		        (unsigned long long)s.per_point_ipc, s.grid_ms);
		fflush(stderr);
		m_table_stats = Stats{};
	}

	void
	begin_table_locked()
	{
		log_table_locked();
		m_have_table = false;
		m_have_p1 = false;
	}

	bool
	serve_locked(int32_t tx, int32_t ty, int32_t *out_x, int32_t *out_y, bool *answered)
	{
		bool dec = false;
		int32_t sx = tx, sy = ty;
		if (!m_table.lookup(tx, ty, &sx, &sy, &dec)) {
			*answered = false;
			return false;
		}
		*answered = true;
		m_table_stats.from_table++;
		m_total.from_table++;
		if (dec) {
			return false; // the DP declined: exactly what the per-point call would say
		}
		*out_x = sx;
		*out_y = sy;
		return true;
	}

	void
	count_locked()
	{
		m_table_stats.queries++;
		m_total.queries++;
	}

	//! Serve (tx, ty) from the table just fetched, else ask it per point.
	bool
	serve_or_per_point_locked(int32_t tx, int32_t ty, int32_t *out_x, int32_t *out_y)
	{
		bool answered = false;
		if (m_have_table) {
			const bool r = serve_locked(tx, ty, out_x, out_y, &answered);
			if (answered) {
				return r;
			}
		}
		return per_point_locked(0, 0, tx, ty, out_x, out_y);
	}

	bool
	snap_locked(int32_t ox, int32_t oy, int32_t tx, int32_t ty, int32_t *out_x, int32_t *out_y)
	{
		if (ox != 0 || oy != 0 || m_pfn_grid == nullptr) {
			// Not the lattice probe (the X11 drag and the drop snap use real
			// origins), or no grid entry point: the plain per-point call.
			return per_point_locked(ox, oy, tx, ty, out_x, out_y);
		}

		// A fresh table's FIRST query, at an integer scale s: logical
		// (-192, -192) is exactly (-192 s, -192 s) device px.
		const bool fresh_int = tx == ty && tx < 0 && (-tx) % kProbeHalf == 0 && -tx / kProbeHalf <= 4;
		const bool live = m_have_table && std::chrono::steady_clock::now() - m_table_time <= kTableTtl;
		// The corner the live table was fetched for, asked again after the
		// probe has run: a new press at the same scale. Refetch — the answer depends on where the viewer
		// is now. (Any other query a live table answers is served, even one
		// that looks like a corner: mid-table at a fractional scale, some grid
		// point lands on (-192 k, -192 k) too.)
		// The helper's own search around the first cell re-asks the corner a
		// few queries in; a new press comes a whole probe (16,641+) later.
		const bool repeat_corner = live && m_fresh_corner && tx == m_corner_x && ty == m_corner_y &&
		                           m_table_stats.queries > (uint64_t)kProbeCount * 8;

		if (live && !repeat_corner) {
			bool answered = false;
			const bool r = serve_locked(tx, ty, out_x, out_y, &answered);
			if (answered) {
				count_locked();
				return r;
			}
		}

		// The same corner at a FRACTIONAL scale: ~-192 s on both axes (the two
		// differ by at most the placement rounding).
		const bool fresh_frac = !fresh_int && tx <= -kProbeHalf - 1 && ty <= -kProbeHalf - 1 &&
		                        tx >= -4 * kProbeHalf && ty >= -4 * kProbeHalf && std::abs(tx - ty) <= 2;
		if (fresh_int || fresh_frac) {
			begin_table_locked();
			count_locked();
			if (fresh_int) {
				fetch_integer_locked(tx, ty, -tx / kProbeHalf, "fresh drag-lattice table");
			} else {
				// Symmetric about (0, 0): the table spans [tx, -tx] x [ty, -ty].
				fetch_dense_locked(tx, ty, -2 * tx, -2 * ty, "fresh drag-lattice table, fractional scale");
			}
			m_fresh_corner = true;
			m_corner_x = tx;
			m_corner_y = ty;
			return serve_or_per_point_locked(tx, ty, out_x, out_y);
		}

		// Outside any live table: an extension table is starting.
		if (m_have_table) {
			begin_table_locked();
		}
		count_locked();
		if (m_have_p1 && ty == m_p1_y && tx > m_p1_x && tx - m_p1_x <= 4 * kProbeCell + 1) {
			// The second grid query on the table's first row names the step
			// (3 logical px).
			const int32_t step = tx - m_p1_x;
			m_have_p1 = false;
			if (step % kProbeCell == 0) {
				fetch_integer_locked(m_p1_x, m_p1_y, step / kProbeCell, "extension drag-lattice table");
			} else {
				const int32_t span = (int32_t)std::ceil((double)(step + 1) / kProbeCell * 2 * kProbeHalf);
				fetch_dense_locked(m_p1_x, m_p1_y, span, span, "extension drag-lattice table, fractional scale");
			}
			m_fresh_corner = false;
			return serve_or_per_point_locked(tx, ty, out_x, out_y);
		}
		// Keep the first query as the table's corner while the helper searches
		// around its answer (queries within a few px of it); anything else
		// starts over from here.
		if (!m_have_p1 || std::abs(tx - m_p1_x) > 16 || std::abs(ty - m_p1_y) > 16) {
			m_have_p1 = true;
			m_p1_x = tx;
			m_p1_y = ty;
		}
		return per_point_locked(ox, oy, tx, ty, out_x, out_y);
	}

	mutable std::mutex m_mutex;
	PFN_xrWeaveSnapWindowRectDXR m_pfn_rect = nullptr;
	PFN_xrWeaveSnapWindowGridDXR m_pfn_grid = nullptr;
	XrSession m_session = XR_NULL_HANDLE;
	int32_t m_w = 0, m_h = 0;
	bool m_failed = false;

	bool m_have_table = false;
	Table m_table;
	std::chrono::steady_clock::time_point m_table_time;
	bool m_have_p1 = false;
	int32_t m_p1_x = 0, m_p1_y = 0;
	bool m_fresh_corner = false; //!< the live table was fetched for a fresh corner...
	int32_t m_corner_x = 0, m_corner_y = 0; //!< ...this one

	Stats m_table_stats;
	Stats m_total;
};
