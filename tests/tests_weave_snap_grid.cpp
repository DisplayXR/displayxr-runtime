// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave v11 bulk grid snap (#1723): the runtime's loop over the
 *         per-point DP snap.
 *
 * Pinned against a fake display processor shaped like the real one (slanted
 * lattice, displacement-only — see tests_dp_vk_snap_window_rect):
 *
 * `u_snap_grid` — the loop every route runs (in-process vk_native, the
 * comp_multi weave engine behind IPC, the D3D11 service). It must return, in
 * row-major order, exactly what the per-point slot returns for each target;
 * stop and report "declined" the moment the slot declines, leaving identity
 * everywhere; reject every grid outside its bounds BEFORE looping (the counts
 * come off the IPC wire); and never clamp an unencodable delta into a wrong
 * position.
 *
 * The consumer side — that the Wayland drag lattice built from grid calls is
 * the per-point table — is displayxr-common's (tests/linux_window_lattice_test,
 * dxr_wl_lattice::probe_via_grid, v2.24.0); `weave_present_vk_linux
 * --lattice-selftest` checks it end to end against a live service.
 */

#include "catch_amalgamated.hpp"

#include "util/u_snap_grid.h"
#include "xrt/xrt_display_processor_vk.h"

#include <cmath>
#include <cstring>
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
