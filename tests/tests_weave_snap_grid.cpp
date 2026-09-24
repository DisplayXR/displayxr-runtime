// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave v11 bulk grid snap (#1723): the runtime's loop over the
 *         per-point DP snap, and the app-side table that serves the Wayland
 *         drag-lattice probe from one grid call.
 *
 * Two layers, both pinned against a fake display processor shaped like the real
 * one (slanted lattice, displacement-only — see tests_dp_vk_snap_window_rect):
 *
 * 1. `u_snap_grid` — the loop every route runs (in-process vk_native, the
 *    comp_multi weave engine behind IPC, the D3D11 service). It must return, in
 *    row-major order, exactly what the per-point slot returns for each target;
 *    stop and report "declined" the moment the slot declines, leaving identity
 *    everywhere; reject every grid outside its bounds BEFORE looping (the counts
 *    come off the IPC wire); and never clamp an unencodable delta into a wrong
 *    position.
 * 2. `DxrWeaveSnapGrid` (test_apps/common) — a memo in front of the per-point
 *    callback. Run through a copy of displayxr-common's lattice probe, it must
 *    produce the SAME table as the per-point probe, at integer and fractional
 *    scales, for fresh and extension tables, in at most three calls into the
 *    runtime per table (the #1723 acceptance bar; the per-point probe makes
 *    16,641+).
 */

#include "catch_amalgamated.hpp"

#include "util/u_snap_grid.h"
#include "xrt/xrt_display_processor_vk.h"

#include "dxr_weave_snap_grid.h"

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace {

/*
 *
 * A fake vendor snap: slanted invariant u = x + y, pitch kPitch, from the origin.
 *
 */

constexpr int32_t kPitch = 7; // odd on purpose: answers land off an even lattice
int g_calls = 0;
int g_decline_after = -1; // >= 0: decline from this call on
int g_far = 0;            // non-zero: add this to x (an unencodable answer)

bool
fake_snap(struct xrt_display_processor_vk *xdp,
          int32_t origin_x,
          int32_t origin_y,
          int32_t target_x,
          int32_t target_y,
          int32_t *out_x,
          int32_t *out_y)
{
	(void)xdp;
	if (g_decline_after >= 0 && g_calls >= g_decline_after) {
		g_calls++;
		return false;
	}
	g_calls++;
	const int32_t dx = target_x - origin_x, dy = target_y - origin_y;
	const int32_t du = dx + dy;
	const int32_t du_snap = (int32_t)std::lround((double)du / kPitch) * kPitch;
	*out_x = origin_x + (du_snap - dy) + g_far;
	*out_y = origin_y + dy;
	return true;
}

struct xrt_display_processor_vk g_dp;

void
reset_dp()
{
	std::memset(&g_dp, 0, sizeof(g_dp));
	g_dp.base.struct_size = sizeof(g_dp);
	g_dp.snap_window_rect = fake_snap;
	g_calls = 0;
	g_decline_after = -1;
	g_far = 0;
}

//! The runtime's per-point snap through the real DP wrapper (the slot gate).
bool
point_fn(void *ud, int32_t ox, int32_t oy, int32_t tx, int32_t ty, int32_t *sx, int32_t *sy)
{
	return xrt_display_processor_vk_snap_window_rect((struct xrt_display_processor_vk *)ud, ox, oy, tx, ty, sx, sy);
}

std::vector<int8_t>
eval(const struct u_snap_grid &g, bool *declined, uint32_t *calls = nullptr, uint32_t *no_delta = nullptr)
{
	std::vector<int8_t> out(2 * (size_t)u_snap_grid_point_count(&g), 99);
	u_snap_grid_eval(&g, point_fn, &g_dp, out.data(), declined, calls, no_delta);
	return out;
}

} // namespace

TEST_CASE("snap_grid: row-major, and every point is the per-point answer")
{
	reset_dp();
	// 129 x 7: rows that are not a power of two, so a row-stride slip shows.
	const struct u_snap_grid g = {1000, -500, 900, -620, 3, 5, 129, 7};
	REQUIRE(u_snap_grid_validate(&g, nullptr));
	bool declined = true;
	uint32_t calls = 0;
	const std::vector<int8_t> out = eval(g, &declined, &calls);
	CHECK_FALSE(declined);
	CHECK(calls == 129u * 7u);

	uint32_t moved = 0;
	for (uint32_t j = 0; j < g.count_y; j++) {
		for (uint32_t i = 0; i < g.count_x; i++) {
			const int32_t tx = g.first_x + (int32_t)i * g.step_x, ty = g.first_y + (int32_t)j * g.step_y;
			int32_t sx = 0, sy = 0;
			REQUIRE(fake_snap(&g_dp, g.origin_x, g.origin_y, tx, ty, &sx, &sy));
			const size_t k = (size_t)j * g.count_x + i;
			INFO("point (" << i << ", " << j << ")");
			CHECK(out[2 * k] == sx - tx);
			CHECK(out[2 * k + 1] == sy - ty);
			moved += (sx != tx) ? 1 : 0;
		}
	}
	CHECK(moved > 0); // the fake actually snaps: the check above is not vacuous
}

TEST_CASE("snap_grid: the absolute frame cancels, as it does per point")
{
	reset_dp();
	const struct u_snap_grid a = {0, 0, -40, -40, 1, 1, 81, 81};
	const struct u_snap_grid b = {12345, -678, 12345 - 40, -678 - 40, 1, 1, 81, 81};
	bool da = true, db = true;
	// Compared outside CHECK: Catch's --success reporter would stringify two
	// 13 KB int8 vectors.
	const bool same = eval(a, &da) == eval(b, &db);
	CHECK(same);
	CHECK_FALSE(da);
	CHECK_FALSE(db);
}

TEST_CASE("snap_grid: a decline stops the loop and leaves identity everywhere")
{
	reset_dp();
	g_decline_after = 200; // mid-row: row 1 of a 129-wide grid
	const struct u_snap_grid g = {0, 0, -192, -192, 3, 3, 129, 129};
	bool declined = false;
	uint32_t calls = 0;
	const std::vector<int8_t> out = eval(g, &declined, &calls);
	CHECK(declined);
	CHECK(calls == 201u); // stopped at the first "no"
	for (int8_t v : out) {
		REQUIRE(v == 0);
	}

	// A DP without the slot (an older plug-in) reads as declined from point 0,
	// through the same wrapper — never dereferenced.
	reset_dp();
	g_dp.base.struct_size = (uint32_t)offsetof(struct xrt_display_processor_vk, snap_window_rect);
	calls = 0;
	const std::vector<int8_t> out2 = eval(g, &declined, &calls);
	CHECK(declined);
	CHECK(calls == 1u);
	CHECK(g_calls == 0);
}

TEST_CASE("snap_grid: an unencodable delta is flagged, never clamped")
{
	reset_dp();
	g_far = 300;
	const struct u_snap_grid g = {0, 0, 0, 0, 1, 1, 4, 2};
	bool declined = true;
	uint32_t no_delta = 0;
	const std::vector<int8_t> out = eval(g, &declined, nullptr, &no_delta);
	CHECK_FALSE(declined);
	CHECK(no_delta == 8u);
	for (int8_t v : out) {
		CHECK(v == U_SNAP_GRID_NO_DELTA);
	}
	// -128 in dx is never a real answer: the encodable range is symmetric.
	g_far = -127;
	const struct u_snap_grid one = {0, 0, 0, 0, 1, 1, 1, 1};
	const std::vector<int8_t> edge = eval(one, &declined);
	CHECK(edge[0] == -127);
}

TEST_CASE("snap_grid: bounds are checked before anything is looped")
{
	const char *why = nullptr;
	struct u_snap_grid g = {0, 0, 0, 0, 1, 1, U_SNAP_GRID_MAX_AXIS, U_SNAP_GRID_MAX_AXIS};
	CHECK(u_snap_grid_validate(&g, &why)); // exactly the maximum is fine
	CHECK(u_snap_grid_point_count(&g) == U_SNAP_GRID_MAX_POINTS);

	struct u_snap_grid bad = g;
	bad.count_x = 0;
	CHECK_FALSE(u_snap_grid_validate(&bad, &why));
	bad = g;
	bad.count_y = U_SNAP_GRID_MAX_AXIS + 1;
	CHECK_FALSE(u_snap_grid_validate(&bad, &why));
	bad = g;
	bad.count_x = 0xFFFFFFFFu; // a wire value whose product would wrap uint32
	bad.count_y = 0xFFFFFFFFu;
	CHECK_FALSE(u_snap_grid_validate(&bad, &why));
	bad = g;
	bad.step_x = 0;
	CHECK_FALSE(u_snap_grid_validate(&bad, &why));
	bad = g;
	bad.step_y = -3;
	CHECK_FALSE(u_snap_grid_validate(&bad, &why));
	bad = g;
	bad.first_x = U_SNAP_GRID_MAX_COORD; // + (count - 1) * step runs past the range
	CHECK_FALSE(u_snap_grid_validate(&bad, &why));
	bad = g;
	bad.origin_y = INT32_MIN;
	CHECK_FALSE(u_snap_grid_validate(&bad, &why));
	bad = g;
	bad.step_x = U_SNAP_GRID_MAX_COORD;
	bad.count_x = 3;
	CHECK_FALSE(u_snap_grid_validate(&bad, &why));
	CHECK(why != nullptr);
	CHECK_FALSE(u_snap_grid_validate(nullptr, &why));
}

TEST_CASE("snap_grid: the largest grid evaluates completely")
{
	reset_dp();
	const struct u_snap_grid g = {0, 0, -512, -512, 1, 1, U_SNAP_GRID_MAX_AXIS, U_SNAP_GRID_MAX_AXIS};
	bool declined = true;
	uint32_t calls = 0;
	const std::vector<int8_t> out = eval(g, &declined, &calls);
	CHECK_FALSE(declined);
	CHECK(calls == U_SNAP_GRID_MAX_POINTS);
	// Spot-check the last point (the end of the buffer).
	const int32_t tx = -512 + 1023, ty = -512 + 1023;
	int32_t sx = 0, sy = 0;
	fake_snap(&g_dp, 0, 0, tx, ty, &sx, &sy);
	CHECK(out[out.size() - 2] == sx - tx);
	CHECK(out[out.size() - 1] == sy - ty);
}


/*
 *
 * The app-side table (test_apps/common/dxr_weave_snap_grid.h).
 *
 */

namespace {

// Fake OpenXR entry points over the fake DP, counting "IPC calls".
int g_rect_calls = 0;
int g_grid_calls = 0;
bool g_xr_declines = false;

XrResult XRAPI_CALL
fake_xr_rect(XrSession, const XrRect2Di *origin, const XrRect2Di *target, XrRect2Di *snapped)
{
	g_rect_calls++;
	int32_t sx = target->offset.x, sy = target->offset.y;
	if (!g_xr_declines) {
		fake_snap(&g_dp, origin->offset.x, origin->offset.y, target->offset.x, target->offset.y, &sx, &sy);
	}
	snapped->offset.x = sx; // the per-point call: identity on a decline
	snapped->offset.y = sy;
	snapped->extent = target->extent;
	return XR_SUCCESS;
}

bool
xr_point_fn(void *, int32_t ox, int32_t oy, int32_t tx, int32_t ty, int32_t *sx, int32_t *sy)
{
	if (g_xr_declines) {
		return false;
	}
	return fake_snap(&g_dp, ox, oy, tx, ty, sx, sy);
}

XrResult XRAPI_CALL
fake_xr_grid(XrSession,
             const XrWeaveSnapGridInfoDXR *info,
             uint32_t cap,
             uint32_t *count,
             XrWeaveSnapGridPointDXR *points,
             XrBool32 *declined)
{
	g_grid_calls++;
	const struct u_snap_grid g = {
	    info->originRect.offset.x, info->originRect.offset.y, info->firstTarget.x, info->firstTarget.y,
	    info->step.width,          info->step.height,         info->countX,        info->countY};
	if (!u_snap_grid_validate(&g, nullptr)) {
		return XR_ERROR_VALIDATION_FAILURE;
	}
	*count = u_snap_grid_point_count(&g);
	if (cap < *count) {
		return XR_ERROR_SIZE_INSUFFICIENT;
	}
	bool d = false;
	u_snap_grid_eval(&g, xr_point_fn, nullptr, (int8_t *)points, &d, nullptr, nullptr);
	*declined = d ? XR_TRUE : XR_FALSE;
	return XR_SUCCESS;
}

/*
 * A copy of displayxr-common v2.23.0's dxr_wl_lattice::probe — the consumer
 * the table is shaped for. Kept verbatim in behaviour: device displacement of a
 * logical grid (Mutter's round-half-away placement), the DP's answer mapped back
 * to logical, and the ±2 logical-px search when it does not map back.
 */
int32_t
logical_to_px(int32_t rel, double scale)
{
	const double v = (double)rel * scale;
	return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

struct Probe
{
	std::vector<std::pair<int32_t, int32_t>> entries;
	size_t probed = 0, fixed = 0;
	bool declined = false;
	bool
	operator==(const Probe &o) const
	{
		return entries == o.entries && probed == o.probed && fixed == o.fixed && declined == o.declined;
	}
};

template <typename Snap>
Probe
helper_probe(const Snap &snap, int32_t rel0, double scale, int32_t cx, int32_t cy)
{
	auto dd = [&](int32_t l) { return logical_to_px(rel0 + l, scale) - logical_to_px(rel0, scale); };
	Probe r;
	std::vector<std::pair<int32_t, int32_t>> seen;
	auto fixed_point = [&](int32_t lx, int32_t ly) {
		const int32_t tx = dd(lx), ty = dd(ly);
		int32_t ox = tx, oy = ty;
		return snap(tx, ty, &ox, &oy) && ox == tx && oy == ty;
	};
	const int32_t half = 192, cell = 3;
	for (int32_t gy = cy - half; gy <= cy + half && !r.declined; gy += cell) {
		for (int32_t gx = cx - half; gx <= cx + half; gx += cell) {
			r.probed++;
			const int32_t tx = dd(gx), ty = dd(gy);
			int32_t sx = tx, sy = ty;
			if (!snap(tx, ty, &sx, &sy)) {
				r.declined = true;
				break;
			}
			if (sx == tx && sy == ty) {
				r.fixed++;
			}
			const int32_t bx = (int32_t)std::lround((double)sx / scale);
			const int32_t by = (int32_t)std::lround((double)sy / scale);
			int32_t ax = 0, ay = 0;
			bool found = dd(bx) == sx && dd(by) == sy;
			if (found) {
				ax = bx;
				ay = by;
			}
			for (int32_t ring = 0; ring <= 2 && !found; ring++) {
				for (int32_t j = -ring; j <= ring && !found; j++) {
					for (int32_t i = -ring; i <= ring && !found; i++) {
						if (std::abs(i) != ring && std::abs(j) != ring) {
							continue;
						}
						if (fixed_point(bx + i, by + j)) {
							ax = bx + i;
							ay = by + j;
							found = true;
						}
					}
				}
			}
			if (!found) {
				continue;
			}
			const std::pair<int32_t, int32_t> key{ax, ay};
			bool dup = false;
			for (auto it = seen.rbegin(); it != seen.rend() && it - seen.rbegin() < 8; ++it) {
				if (*it == key) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				seen.push_back(key);
				r.entries.push_back(key);
			}
		}
	}
	return r;
}

//! The per-point probe (pre-#1723) and the tabled probe, compared.
void
check_probe(int32_t rel0, double scale, int32_t cx, int32_t cy, int max_calls, DxrWeaveSnapGrid *cache = nullptr)
{
	reset_dp();
	g_rect_calls = g_grid_calls = 0;
	auto per_point = [](int32_t tx, int32_t ty, int32_t *ox, int32_t *oy) {
		XrRect2Di o = {}, t = {}, s = {};
		t.offset = {tx, ty};
		fake_xr_rect(XR_NULL_HANDLE, &o, &t, &s);
		*ox = s.offset.x;
		*oy = s.offset.y;
		return !g_xr_declines; // what the helper's own provider reports
	};
	const Probe want = helper_probe(per_point, rel0, scale, cx, cy);
	const int per_point_calls = g_rect_calls;

	DxrWeaveSnapGrid local;
	DxrWeaveSnapGrid &grid = cache != nullptr ? *cache : local;
	if (cache == nullptr) {
		grid.attach_functions((XrSession)1, fake_xr_rect, fake_xr_grid, 640, 480);
	}
	g_rect_calls = g_grid_calls = 0;
	auto tabled = [&grid](int32_t tx, int32_t ty, int32_t *ox, int32_t *oy) {
		return DxrWeaveSnapGrid::callback(&grid, 0, 0, tx, ty, ox, oy);
	};
	const Probe got = helper_probe(tabled, rel0, scale, cx, cy);

	INFO("scale " << scale << " rel0 " << rel0 << " centre (" << cx << ", " << cy << "): per-point probe made "
	              << per_point_calls << " calls; tabled made " << g_grid_calls << " grid + " << g_rect_calls
	              << " per-point");
	CHECK(got == want);
	CHECK(g_grid_calls + g_rect_calls <= max_calls);
	if (!want.declined) {
		CHECK(per_point_calls >= 129 * 129);
		CHECK(want.entries.size() > 0);
	}
}

} // namespace

TEST_CASE("snap_grid table: a fresh table at scale 1 is ONE grid call")
{
	check_probe(0, 1.0, 0, 0, 1);
	check_probe(517, 1.0, 0, 0, 1); // the start's position does not matter at an integer scale
}

TEST_CASE("snap_grid table: integer scales 2 and 3 — one call covers the reachable-lattice search too")
{
	check_probe(0, 2.0, 0, 0, 1);
	check_probe(301, 2.0, 0, 0, 1);
	check_probe(10, 3.0, 0, 0, 1);
}

TEST_CASE("snap_grid table: fractional scales — one dense call")
{
	check_probe(0, 1.5, 0, 0, 1);
	check_probe(7, 1.5, 0, 0, 1);
	check_probe(3, 1.25, 0, 0, 1);
	check_probe(11, 1.75, 0, 0, 1);
}

/*
 * An extension table (the drag outran the first one; built on the helper's
 * worker thread, not at the press) starts anywhere, so its first cell is asked
 * per point, plus the helper's own search around that one answer when it is
 * not a reachable position, before the second grid query names the step. At
 * scale 1 there is never such a search: 1 per-point call + 1 grid call.
 * Otherwise at most 1 + 24 + 1 (a ring-2 search is 25 points, the first of
 * which is the cell itself).
 */
TEST_CASE("snap_grid table: extension tables (off-centre) — one grid call after the first cell")
{
	check_probe(0, 1.0, 150, -40, 2);
	check_probe(0, 2.0, -120, 96, 27);
	check_probe(5, 1.5, 100, 100, 27);
}

TEST_CASE("snap_grid table: a fresh press after an extension refetches, never serves stale")
{
	reset_dp();
	DxrWeaveSnapGrid grid;
	grid.attach_functions((XrSession)1, fake_xr_rect, fake_xr_grid, 640, 480);
	check_probe(0, 1.0, 0, 0, 1, &grid);
	check_probe(0, 1.0, 200, 0, 2, &grid);
	check_probe(0, 1.0, 0, 0, 1, &grid); // the corner query always starts a new table
	const DxrWeaveSnapGrid::Stats s = grid.stats();
	CHECK(s.grid_ipc >= 3);
	CHECK(s.from_table > 3 * 129 * 128);
}

TEST_CASE("snap_grid table: a declining DP is reported declined, from the table")
{
	g_xr_declines = true;
	check_probe(0, 1.0, 0, 0, 1);
	check_probe(0, 2.0, 0, 0, 1);
	g_xr_declines = false;
}

TEST_CASE("snap_grid table: without the grid entry point it is the per-point provider")
{
	reset_dp();
	DxrWeaveSnapGrid grid;
	grid.attach_functions((XrSession)1, fake_xr_rect, nullptr, 640, 480);
	g_rect_calls = g_grid_calls = 0;
	int32_t x = 0, y = 0;
	CHECK(DxrWeaveSnapGrid::callback(&grid, 0, 0, -192, -192, &x, &y));
	CHECK(g_rect_calls == 1);
	CHECK(g_grid_calls == 0);
	// And a real (non-probe) origin always goes per point.
	grid.attach_functions((XrSession)1, fake_xr_rect, fake_xr_grid, 640, 480);
	g_rect_calls = 0;
	CHECK(DxrWeaveSnapGrid::callback(&grid, 100, 50, 113, 57, &x, &y));
	CHECK(g_rect_calls == 1);
	CHECK(g_grid_calls == 0);
}
