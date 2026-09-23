// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  X11 window-PLACEMENT granularity: is a window origin expressible in
 *         single device pixels, and if not, which positions are reachable?
 * @ingroup aux_util
 *
 * ## The rule
 *
 * **A weave phase is a function of the window's absolute position in physical
 * panel pixels, so a window that cannot LAND on an arbitrary pixel cannot be
 * phase-snapped to one.**
 *
 * `u_wayland_geom.h` is the sibling rule for the other half of the same
 * problem: it converts a *reported* geometry from logical to device pixels.
 * This header is about a geometry we *request*. The two failures are
 * independent and neither implies the other — the measured 2026-09-20 session
 * had a panel whose X11 rect was exactly its native size (so every size check
 * passed, and the reported origin really was device pixels) while window
 * placement was still quantised to 2 px. Any output above 100% does that —
 * the panel's own 200% is enough on its own, and so is a 166% laptop next to
 * a panel at 100%.
 *
 * ## Why one scaled monitor breaks an unscaled one
 *
 * XWayland applies **one global scale to the entire X screen**, not a
 * per-output scale, and that factor is driven by the most-scaled output
 * (Mutter: the integer ceiling of the largest monitor scale). An X11 client
 * asks for root coordinate `x`; the compositor divides by the global scale `G`
 * to get a logical position, rounds it to an integer, and multiplies back. So
 * only positions that are multiples of `G` survive the round trip, and the
 * surviving set is decided by a monitor the 3D app may not even be on.
 *
 * Three configurations measured on one box, laptop eDP-1 2880x1800 next to an
 * external 3840x2160 3D panel:
 *
 * | laptop | panel | X11 sees panel | X11 sees laptop | root      | G | verdict |
 * |--------|-------|----------------|-----------------|-----------|---|---------|
 * | 166%   | 200%  | 3840x2160 (=native!) | 3456x2160 | 7296x2160 | 2 | quantised — and every panel-only check passes |
 * | 166%   | 100%  | 7680x4320      | 3456x2160       | 11136x4320| 2 | quantised |
 * | 100%   | 100%  | 3840x2160      | 2880x1800       | 6720x2160 | 1 | device pixels |
 *
 * Row 1 is the reason @ref u_x11_scale_solve looks at **every** output rather
 * than the panel: the panel alone cannot tell, because its own numbers are
 * perfect.
 *
 * ## The solver
 *
 * For each output we have two numbers: what X11 reports, and the true mode the
 * DRM/KMS connector is running. Their ratio is `G / output_scale`, which alone
 * separates neither factor — but `G` is an integer, `output_scale >= 1`, and
 * `G == max(ceil(output_scale))`, and those three constraints pin `G` for
 * every mixed configuration. @ref u_x11_scale_solve searches `G` upward and
 * takes the smallest consistent value.
 *
 * **Its one blind spot, stated plainly:** when *every* output runs at the same
 * integer scale, the X11 sizes all equal their native modes and `G = 1` is
 * consistent too, so the solver answers "device pixels" when the truth is a
 * quantum of `G`. Geometry cannot distinguish those two worlds. Only a
 * measurement can — request an odd position, read back where the window
 * landed — which is what @ref u_x11_placement_probe is for, and why the probe
 * is authoritative over the solver wherever both have spoken.
 *
 * ## Searching the reachable lattice
 *
 * Under a quantum `q` a window at origin `O` can only reach `O + q*Z^2`. A
 * phase-preserving snap must therefore be searched **on that sublattice**,
 * which is what @ref u_x11_lattice_candidate enumerates — outward in Chebyshev
 * rings, so a caller stops at the first acceptable point and that point is the
 * nearest acceptable one. Why a search rather than arithmetic: the lens
 * lattice belongs to the vendor display processor (ADR-019) and is never
 * published, so the only way to ask "is this position correctly phased?" is to
 * offer it to `snap_window_rect` and see whether it comes back unchanged. A
 * fixed point of the snap is a position the display processor would not
 * improve on.
 *
 * Pure arithmetic: no X11, no platform guard, no dependency. The rules above
 * are worth strictly more when a host test can pin them than when they compile
 * only on the one platform they run on. See `tests/tests_aux_x11_scale.cpp`.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The two sizes of one output: what the X server reports for it, and the mode
 * the hardware is actually scanning out.
 *
 * Both are *sizes*, never positions — a position would additionally depend on
 * the monitor arrangement, and the solver deliberately needs no arrangement.
 *
 * @ingroup aux_util
 */
struct u_x11_output_sizes
{
	//! RandR output name, e.g. `"HDMI-1"`. Diagnostics only; the solver
	//! never matches on it (the caller has already done the DRM pairing).
	char name[64];

	//! Size of this output's rect in the X root window, in X11 pixels.
	uint32_t x11_w, x11_h;

	//! The mode the DRM/KMS connector is running, in device pixels.
	//! 0 in either axis ⟹ unknown, and the output is skipped by the solver.
	uint32_t native_w, native_h;
};

/*!
 * What @ref u_x11_scale_solve concluded about the X root coordinate space.
 *
 * @ingroup aux_util
 */
enum u_x11_scale_state
{
	//! Nothing to measure — no output had a known native mode, or no
	//! candidate global scale was consistent. Never degrade on ignorance:
	//! a caller seeing this must keep whatever behaviour it already had.
	U_X11_SCALE_UNKNOWN = 0,

	//! The X root is device pixels and every pixel is addressable.
	//! Placement quantum 1. This is the supported configuration.
	U_X11_SCALE_DEVICE_PIXELS,

	//! A global scale is in effect: window origins are quantised to
	//! @ref u_x11_scale_verdict::quantum X11 pixels.
	U_X11_SCALE_QUANTIZED,
};

/*!
 * @ingroup aux_util
 */
struct u_x11_scale_verdict
{
	enum u_x11_scale_state state;

	//! Placement quantum in X11 pixels. 1 when device pixels, 0 when
	//! unknown, >1 when quantised.
	uint32_t quantum;

	//! Index into the caller's array of the most-scaled output, whose
	//! ceiling IS the global factor. Not the only culprit: any output above
	//! 100% forces a factor >= 2, so a message should point at the per-output
	//! list, not just this one. -1 when none / unknown.
	int32_t culprit;

	//! Derived scale of @ref culprit, e.g. 1.6667. 0 when unknown.
	double culprit_scale;

	//! How many outputs had a known native mode and took part.
	uint32_t outputs_compared;
};

/*!
 * Smallest integer global scale consistent with every output's
 * (X11 size, native mode) pair. See the file comment for the model and for
 * the uniform-integer-scale blind spot.
 *
 * @param outs  Per-output sizes; outputs with an unknown native mode are
 *              skipped rather than failing the solve.
 * @param count Number of entries in @p outs.
 * @param[out] out Verdict; always written, zeroed first.
 *
 * @ingroup aux_util
 */
static inline void
u_x11_scale_solve(const struct u_x11_output_sizes *outs, uint32_t count, struct u_x11_scale_verdict *out)
{
	if (out == NULL) {
		return;
	}
	out->state = U_X11_SCALE_UNKNOWN;
	out->quantum = 0;
	out->culprit = -1;
	out->culprit_scale = 0.0;
	out->outputs_compared = 0;

	if (outs == NULL || count == 0) {
		return;
	}

	// Count the usable outputs first: a solve over zero of them would
	// vacuously "succeed" at G = 1 and report device pixels, which is
	// exactly the confident-wrong answer this module exists to avoid.
	for (uint32_t i = 0; i < count; i++) {
		if (outs[i].x11_w > 0 && outs[i].x11_h > 0 && outs[i].native_w > 0 && outs[i].native_h > 0) {
			out->outputs_compared++;
		}
	}
	if (out->outputs_compared == 0) {
		return;
	}

	/*
	 * A monitor scale is a user setting, not a continuum: GNOME offers up
	 * to 300%, so a global factor above 4 is not a configuration, it is a
	 * bad measurement. Bounding the search keeps a garbage input from
	 * being reported as a plausible quantum.
	 */
	const uint32_t max_global = 4;
	// Slack for a fractional scale that does not divide evenly: at 1.6667 a
	// 2880 px panel is 1728 logical, and 1728 * 1.6667 is 2880.05.
	const double eps = 0.02;

	for (uint32_t g = 1; g <= max_global; g++) {
		bool consistent = true;
		uint32_t highest_ceil = 0;
		double best_scale = 0.0;
		int32_t best_index = -1;

		for (uint32_t i = 0; i < count; i++) {
			const struct u_x11_output_sizes *o = &outs[i];
			if (o->x11_w == 0 || o->x11_h == 0 || o->native_w == 0 || o->native_h == 0) {
				continue;
			}
			// scale = native * G / x11. Both axes must agree, or this
			// output is rotated / panel-fitted and cannot be reasoned
			// about — which invalidates the whole candidate rather than
			// just the output, because a wrong pair would otherwise be
			// silently dropped and the solve would narrow to a
			// confident wrong answer.
			const double sx = ((double)o->native_w * (double)g) / (double)o->x11_w;
			const double sy = ((double)o->native_h * (double)g) / (double)o->x11_h;
			const double diff = sx > sy ? sx - sy : sy - sx;
			if (diff > 0.05) {
				consistent = false;
				break;
			}
			const double s = (sx + sy) * 0.5;
			// A display server does not scale a desktop DOWN, so a
			// derived scale below 1 means this candidate G is too small.
			if (s < 1.0 - eps) {
				consistent = false;
				break;
			}
			uint32_t c = (uint32_t)(s - eps) + 1u; // ceil with slack
			if (s <= 1.0 + eps) {
				c = 1u;
			}
			if (c > highest_ceil) {
				highest_ceil = c;
			}
			if (s > best_scale) {
				best_scale = s;
				best_index = (int32_t)i;
			}
		}

		// The closing constraint: XWayland's global factor IS the ceiling
		// of the largest monitor scale. A candidate that does not
		// reproduce itself from the scales it implies is not the one.
		if (!consistent || highest_ceil != g) {
			continue;
		}

		out->state = (g == 1) ? U_X11_SCALE_DEVICE_PIXELS : U_X11_SCALE_QUANTIZED;
		out->quantum = g;
		out->culprit = (g == 1) ? -1 : best_index;
		out->culprit_scale = (g == 1) ? 0.0 : best_scale;
		return;
	}
}

/*!
 * Round @p v onto the lattice of positions reachable from @p anchor under
 * @p quantum, choosing the nearest.
 *
 * @ingroup aux_util
 */
static inline int32_t
u_x11_reachable_round(int32_t anchor, int32_t v, uint32_t quantum)
{
	if (quantum <= 1) {
		return v;
	}
	const int32_t q = (int32_t)quantum;
	int32_t d = v - anchor;
	// Round-half-away-from-zero, symmetric so a drag left and a drag right
	// quantise the same way.
	const int32_t half = q / 2;
	if (d >= 0) {
		d = ((d + half) / q) * q;
	} else {
		d = -(((-d) + half) / q) * q;
	}
	return anchor + d;
}

/*!
 * Number of candidates @ref u_x11_lattice_candidate produces for @p rings
 * rings, ring 0 being the single point (0, 0).
 *
 * @ingroup aux_util
 */
static inline uint32_t
u_x11_lattice_candidate_count(uint32_t rings)
{
	const uint32_t side = 2u * rings + 1u;
	return side * side;
}

/*!
 * The @p index'th lattice offset in Chebyshev-ring order — (0,0), then the 8
 * points at distance 1, then the 16 at distance 2, and so on. Multiply by the
 * placement quantum to get an offset in X11 pixels.
 *
 * Ring order matters: a caller testing candidates in this order and stopping
 * at the first acceptable one has found the *nearest* acceptable one, so the
 * window never travels further from the pointer than it must.
 *
 * @ingroup aux_util
 */
static inline void
u_x11_lattice_candidate(uint32_t index, int32_t *out_i, int32_t *out_j)
{
	if (out_i == NULL || out_j == NULL) {
		return;
	}
	// Ring r holds 8r points (4 sides of 2r), ring 0 holds 1.
	uint32_t r = 0;
	uint32_t base = 0;
	for (;;) {
		const uint32_t in_ring = (r == 0) ? 1u : 8u * r;
		if (index < base + in_ring) {
			break;
		}
		base += in_ring;
		r++;
	}
	if (r == 0) {
		*out_i = 0;
		*out_j = 0;
		return;
	}
	const int32_t ri = (int32_t)r;
	const uint32_t k = index - base;
	const uint32_t side = 2u * r;
	if (k < side) { // top edge, left to right
		*out_i = -ri + (int32_t)k;
		*out_j = -ri;
	} else if (k < 2u * side) { // right edge, top to bottom
		*out_i = ri;
		*out_j = -ri + (int32_t)(k - side);
	} else if (k < 3u * side) { // bottom edge, right to left
		*out_i = ri - (int32_t)(k - 2u * side);
		*out_j = ri;
	} else { // left edge, bottom to top
		*out_i = -ri;
		*out_j = ri - (int32_t)(k - 3u * side);
	}
}

/*!
 * The running result of measuring where windows actually land.
 *
 * This is the authoritative half of the module. The solver reasons from
 * geometry and has a blind spot; this reasons from what happened, and has
 * none — it catches any cause of a placement quantum, including ones nobody
 * has met yet.
 *
 * @ingroup aux_util
 */
struct u_x11_placement_probe
{
	//! Moves requested since the probe was reset.
	uint32_t moves;

	//! Moves whose landed position differed from the requested one.
	uint32_t diverged;

	//! Greatest single-axis divergence seen, X11 px.
	uint32_t worst_delta;

	//! Placement quantum inferred from the landed positions: the GCD of
	//! every landed offset from the first landed position. 0 until at
	//! least one move has landed somewhere new.
	uint32_t inferred_quantum;

	//! @private Anchor for the GCD, and whether it is set.
	int32_t anchor_x, anchor_y;
	bool have_anchor;

	//! Set once, so a caller can report the finding exactly one time.
	bool reported;
};

/*! @private GCD helper for @ref u_x11_placement_probe_note. */
static inline uint32_t
u_x11_gcd(uint32_t a, uint32_t b)
{
	while (b != 0) {
		const uint32_t t = a % b;
		a = b;
		b = t;
	}
	return a;
}

/*!
 * Record one move: we asked for (@p want_x, @p want_y) and the window landed
 * at (@p got_x, @p got_y).
 *
 * The inferred quantum is the GCD of every landed offset from the first landed
 * position — which converges on the true stride from below and is never larger
 * than it, so acting on it can only ever under-correct.
 *
 * @ingroup aux_util
 */
static inline void
u_x11_placement_probe_note(
    struct u_x11_placement_probe *p, int32_t want_x, int32_t want_y, int32_t got_x, int32_t got_y)
{
	if (p == NULL) {
		return;
	}
	p->moves++;
	if (got_x != want_x || got_y != want_y) {
		p->diverged++;
		const uint32_t dx = (uint32_t)(got_x > want_x ? got_x - want_x : want_x - got_x);
		const uint32_t dy = (uint32_t)(got_y > want_y ? got_y - want_y : want_y - got_y);
		const uint32_t d = dx > dy ? dx : dy;
		if (d > p->worst_delta) {
			p->worst_delta = d;
		}
	}
	if (!p->have_anchor) {
		p->anchor_x = got_x;
		p->anchor_y = got_y;
		p->have_anchor = true;
		return;
	}
	const uint32_t ox = (uint32_t)(got_x > p->anchor_x ? got_x - p->anchor_x : p->anchor_x - got_x);
	const uint32_t oy = (uint32_t)(got_y > p->anchor_y ? got_y - p->anchor_y : p->anchor_y - got_y);
	if (ox != 0) {
		p->inferred_quantum = u_x11_gcd(p->inferred_quantum, ox);
	}
	if (oy != 0) {
		p->inferred_quantum = u_x11_gcd(p->inferred_quantum, oy);
	}
}

/*!
 * True once the probe has seen enough to state that placement is quantised:
 * several moves, a divergence on a clear majority of them, and an inferred
 * stride above 1.
 *
 * Deliberately conservative — a single stale read or a window manager nudge
 * must not be enough to change how the drag behaves.
 *
 * @ingroup aux_util
 */
static inline bool
u_x11_placement_probe_is_quantized(const struct u_x11_placement_probe *p)
{
	if (p == NULL || p->moves < 6) {
		return false;
	}
	if (p->inferred_quantum <= 1) {
		return false;
	}
	// A quantum q makes roughly (q-1)/q of arbitrary requests miss. Half is
	// the weakest majority that cannot be a handful of unlucky reads.
	return p->diverged * 2u >= p->moves;
}

#ifdef __cplusplus
}
#endif
