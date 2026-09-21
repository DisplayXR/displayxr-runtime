// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pins the X11 placement-quantum rules in util/u_x11_scale.h to the
 *         three configurations measured on the DS1 box on 2026-09-20.
 */

#include "catch_amalgamated.hpp"

#include "util/u_x11_scale.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>

namespace {

// The measured box: laptop eDP-1 2880x1800 next to an Acer SpatialLabs DS1.
constexpr uint32_t LAPTOP_W = 2880, LAPTOP_H = 1800;
constexpr uint32_t PANEL_W = 3840, PANEL_H = 2160;

u_x11_output_sizes
out(const char *name, uint32_t x11_w, uint32_t x11_h, uint32_t native_w, uint32_t native_h)
{
	u_x11_output_sizes o = {};
	std::strncpy(o.name, name, sizeof(o.name) - 1);
	o.x11_w = x11_w;
	o.x11_h = x11_h;
	o.native_w = native_w;
	o.native_h = native_h;
	return o;
}

} // namespace

TEST_CASE("x11_scale: laptop 166% + panel 200% is QUANTISED even though the panel looks native")
{
	// The dangerous case. X11 reports the panel at exactly its native size,
	// so every panel-only check passes; the laptop is what gives it away.
	const u_x11_output_sizes outs[] = {
	    out("eDP-1", 3456, 2160, LAPTOP_W, LAPTOP_H),
	    out("HDMI-1", PANEL_W, PANEL_H, PANEL_W, PANEL_H),
	};
	u_x11_scale_verdict v = {};
	u_x11_scale_solve(outs, 2, &v);
	CHECK(v.state == U_X11_SCALE_QUANTIZED);
	CHECK(v.quantum == 2);
	// The most-scaled output is the panel itself (200%); the laptop's 166%
	// ALSO ceils to 2, so lowering only one of them does not fix it.
	CHECK(v.culprit == 1);
	CHECK(v.culprit_scale == Catch::Approx(2.0).margin(0.01));
	CHECK(v.outputs_compared == 2);

	SECTION("the panel alone cannot see it — the reason every output is checked")
	{
		u_x11_scale_verdict pv = {};
		u_x11_scale_solve(&outs[1], 1, &pv);
		CHECK(pv.state == U_X11_SCALE_DEVICE_PIXELS);
	}
}

TEST_CASE("x11_scale: laptop 166% + panel 100% is QUANTISED at 2")
{
	const u_x11_output_sizes outs[] = {
	    out("eDP-1", 3456, 2160, LAPTOP_W, LAPTOP_H),
	    out("HDMI-1", 7680, 4320, PANEL_W, PANEL_H),
	};
	u_x11_scale_verdict v = {};
	u_x11_scale_solve(outs, 2, &v);
	CHECK(v.state == U_X11_SCALE_QUANTIZED);
	CHECK(v.quantum == 2);
	CHECK(v.culprit == 0);
	CHECK(v.culprit_scale == Catch::Approx(5.0 / 3.0).margin(0.01));
}

TEST_CASE("x11_scale: both at 100% is DEVICE PIXELS")
{
	const u_x11_output_sizes outs[] = {
	    out("eDP-1", LAPTOP_W, LAPTOP_H, LAPTOP_W, LAPTOP_H),
	    out("HDMI-1", PANEL_W, PANEL_H, PANEL_W, PANEL_H),
	};
	u_x11_scale_verdict v = {};
	u_x11_scale_solve(outs, 2, &v);
	CHECK(v.state == U_X11_SCALE_DEVICE_PIXELS);
	CHECK(v.quantum == 1);
	CHECK(v.culprit == -1);
}

TEST_CASE("x11_scale: never confident without evidence")
{
	u_x11_scale_verdict v = {};
	u_x11_scale_solve(nullptr, 0, &v);
	CHECK(v.state == U_X11_SCALE_UNKNOWN);

	// No DRM match for any output → unknown, not "device pixels".
	const u_x11_output_sizes outs[] = {out("HDMI-1", 3840, 2160, 0, 0)};
	u_x11_scale_solve(outs, 1, &v);
	CHECK(v.state == U_X11_SCALE_UNKNOWN);
	CHECK(v.quantum == 0);

	// An unmatched output is skipped, not fatal.
	const u_x11_output_sizes mixed[] = {
	    out("eDP-1", 3456, 2160, LAPTOP_W, LAPTOP_H),
	    out("DP-9", 1920, 1080, 0, 0),
	};
	u_x11_scale_solve(mixed, 2, &v);
	CHECK(v.state == U_X11_SCALE_QUANTIZED);
	CHECK(v.outputs_compared == 1);
}

TEST_CASE("x11_scale: documented blind spot — a uniform integer scale reads as device pixels")
{
	// Both outputs at 200%: X11 sizes equal native, G = 1 is consistent.
	// Pinned so nobody "fixes" the solver into claiming otherwise; the
	// landing probe is what covers this.
	const u_x11_output_sizes outs[] = {
	    out("eDP-1", LAPTOP_W, LAPTOP_H, LAPTOP_W, LAPTOP_H),
	    out("HDMI-1", PANEL_W, PANEL_H, PANEL_W, PANEL_H),
	};
	u_x11_scale_verdict v = {};
	u_x11_scale_solve(outs, 2, &v);
	CHECK(v.state == U_X11_SCALE_DEVICE_PIXELS);
}

TEST_CASE("x11_scale: a rotated / panel-fitted output is not reasoned about")
{
	const u_x11_output_sizes outs[] = {out("HDMI-1", 2160, 3840, PANEL_W, PANEL_H)};
	u_x11_scale_verdict v = {};
	u_x11_scale_solve(outs, 1, &v);
	CHECK(v.state == U_X11_SCALE_UNKNOWN);
}

TEST_CASE("x11_scale: reachable_round lands on origin + q*Z")
{
	CHECK(u_x11_reachable_round(100, 101, 1) == 101);
	CHECK(u_x11_reachable_round(100, 101, 2) == 102); // half away from zero
	CHECK(u_x11_reachable_round(100, 99, 2) == 98);
	CHECK(u_x11_reachable_round(100, 100, 2) == 100);
	CHECK(u_x11_reachable_round(101, 104, 2) == 105); // anchored at the ORIGIN's parity
	CHECK(u_x11_reachable_round(0, -3, 3) == -3);
	for (int v = -20; v <= 20; v++) {
		const int32_t r = u_x11_reachable_round(7, v, 2);
		CHECK(((r - 7) % 2) == 0);
		CHECK(std::abs(r - v) <= 1);
	}
}

TEST_CASE("x11_scale: lattice candidates cover each ring exactly once, nearest first")
{
	const uint32_t rings = 3;
	const uint32_t n = u_x11_lattice_candidate_count(rings);
	CHECK(n == 49);
	std::set<std::pair<int32_t, int32_t>> seen;
	int32_t last_ring = 0;
	for (uint32_t k = 0; k < n; k++) {
		int32_t i = 99, j = 99;
		u_x11_lattice_candidate(k, &i, &j);
		const int32_t ring = std::max(std::abs(i), std::abs(j));
		CHECK(ring >= last_ring); // monotone in distance
		CHECK(ring <= (int32_t)rings);
		last_ring = ring;
		CHECK(seen.insert({i, j}).second); // no duplicates
	}
	CHECK(seen.size() == n);
	int32_t i0 = 1, j0 = 1;
	u_x11_lattice_candidate(0, &i0, &j0);
	CHECK(i0 == 0);
	CHECK(j0 == 0);
}

TEST_CASE("x11_scale: the landing probe infers the quantum from where windows land")
{
	SECTION("quantum 2: odd requests land on even pixels")
	{
		u_x11_placement_probe p = {};
		// Requests as the DP snapper made them tonight: ~half odd.
		const int want[][2] = {{1001, 501}, {1004, 502}, {1007, 505}, {1010, 506}, {1013, 509}, {1016, 510}};
		for (const auto &w : want) {
			const int gx = w[0] & ~1, gy = w[1] & ~1; // server rounds to even
			u_x11_placement_probe_note(&p, w[0], w[1], gx, gy);
		}
		CHECK(p.moves == 6);
		CHECK(p.inferred_quantum == 2);
		CHECK(p.worst_delta == 1);
		CHECK(u_x11_placement_probe_is_quantized(&p));
	}

	SECTION("quantum 1: every request lands")
	{
		u_x11_placement_probe p = {};
		for (int k = 0; k < 10; k++) {
			u_x11_placement_probe_note(&p, 1000 + 3 * k, 500 + k, 1000 + 3 * k, 500 + k);
		}
		CHECK(p.diverged == 0);
		CHECK(p.inferred_quantum == 1);
		CHECK_FALSE(u_x11_placement_probe_is_quantized(&p));
	}

	SECTION("a handful of stale reads is not a quantum")
	{
		u_x11_placement_probe p = {};
		for (int k = 0; k < 10; k++) {
			const int want = 1000 + 2 * k;
			const int got = (k == 3) ? want - 2 : want;
			u_x11_placement_probe_note(&p, want, 500, got, 500);
		}
		CHECK_FALSE(u_x11_placement_probe_is_quantized(&p));
	}

	SECTION("too few moves never decides")
	{
		u_x11_placement_probe p = {};
		u_x11_placement_probe_note(&p, 1001, 1, 1000, 0);
		u_x11_placement_probe_note(&p, 1003, 3, 1002, 2);
		CHECK_FALSE(u_x11_placement_probe_is_quantized(&p));
	}
}
