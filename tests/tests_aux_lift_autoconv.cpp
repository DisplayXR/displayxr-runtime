// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-owned auto-convergence for lifted video (u_lift_autoconv):
 *         the centre-weighted foreground-mode estimator and its temporal filter.
 */

#include "util/u_lift_autoconv.h"

#include "catch_amalgamated.hpp"

#include <cmath>
#include <vector>

using Catch::Approx;

namespace {

constexpr uint64_t kMs = 1000ull * 1000ull;

//! A synthetic depth map in the conversion's own encoding: INVERSE relative
//! samples 0..255 (decoded / 255 = normalised inverse depth, 255 = nearest).
struct Map
{
	uint32_t w, h;
	std::vector<float> s;
	Map(uint32_t w_, uint32_t h_, float fill) : w(w_), h(h_), s((size_t)w_ * h_, fill) {}
	void
	rect(float x0, float y0, float x1, float y1, float v)
	{
		for (uint32_t y = (uint32_t)(y0 * h); y < (uint32_t)(y1 * h); y++) {
			for (uint32_t x = (uint32_t)(x0 * w); x < (uint32_t)(x1 * w); x++) {
				s[(size_t)y * w + x] = v;
			}
		}
	}
	// A rounded subject: v at its centre, v - fall at its rim (a real subject
	// spans several depth bins, not one).
	void
	dome(float x0, float y0, float x1, float y1, float v, float fall)
	{
		const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
		for (uint32_t y = (uint32_t)(y0 * h); y < (uint32_t)(y1 * h); y++) {
			for (uint32_t x = (uint32_t)(x0 * w); x < (uint32_t)(x1 * w); x++) {
				const float dx = ((float)x / (float)w - cx) / (0.5f * (x1 - x0));
				const float dy = ((float)y / (float)h - cy) / (0.5f * (y1 - y0));
				const float r2 = dx * dx + dy * dy;
				s[(size_t)y * w + x] = v - fall * (r2 < 1.0f ? r2 : 1.0f);
			}
		}
	}
	// A soft gradient background so nothing is perfectly flat.
	void
	ramp_background(float top, float bottom)
	{
		for (uint32_t y = 0; y < h; y++) {
			const float t = (float)y / (float)(h - 1);
			for (uint32_t x = 0; x < w; x++) {
				s[(size_t)y * w + x] = top + (bottom - top) * t;
			}
		}
	}
};

//! The clump picker on its own (no percentile, foreground-biased, far end
//! allowed): what the clump tests below exercise.
void
mode_only(u_lift_autoconv_params *p)
{
	p->blend = 0.0f;
	p->fg_bias = 1.0f;
	p->far_exclude = 0.0f;
}

u_lift_autoconv_estimate
estimate(const Map &m, const float *active = nullptr, bool clump_only = true)
{
	static u_lift_autoconv_grid g; // large: keep it off the stack
	REQUIRE(u_lift_autoconv_grid_from_samples(m.s.data(), m.w, m.h, m.w, true, 1.0f / 255.0f, 0.0f, 128, &g));
	u_lift_autoconv_params p;
	u_lift_autoconv_params_default(&p);
	if (clump_only) {
		mode_only(&p);
	}
	u_lift_autoconv_estimate e;
	u_lift_autoconv_find_subject(&g, active, &p, &e);
	return e;
}

u_lift_autoconv_estimate
fake(float convergence, float sig_bin = 4.0f)
{
	u_lift_autoconv_estimate e = {};
	e.valid = true;
	e.convergence = convergence;
	e.mode_h = 1.0f - convergence;
	e.confidence = 0.5f;
	e.sig[(int)sig_bin] = 1.0f;
	return e;
}

} // namespace

TEST_CASE("lift autoconv: source switch", "[lift_autoconv]")
{
	CHECK(u_lift_autoconv_source_parse(nullptr) == U_LIFT_AUTOCONV_SOURCE_RUNTIME);
	CHECK(u_lift_autoconv_source_parse("") == U_LIFT_AUTOCONV_SOURCE_RUNTIME);
	CHECK(u_lift_autoconv_source_parse("runtime") == U_LIFT_AUTOCONV_SOURCE_RUNTIME);
	CHECK(u_lift_autoconv_source_parse("Module") == U_LIFT_AUTOCONV_SOURCE_MODULE);
	CHECK(u_lift_autoconv_source_parse("off") == U_LIFT_AUTOCONV_SOURCE_MODULE);
	CHECK(u_lift_autoconv_source_parse("0") == U_LIFT_AUTOCONV_SOURCE_MODULE);
	CHECK(u_lift_autoconv_source_parse("bogus") == U_LIFT_AUTOCONV_SOURCE_RUNTIME);
}

TEST_CASE("lift autoconv: grid decodes, downsamples and normalises", "[lift_autoconv]")
{
	static u_lift_autoconv_grid g;
	// Linear metric depth 1..4 m: inverse depth 1..0.25 -> h 1..0.
	Map m(512, 256, 1.0f);
	m.rect(0.5f, 0.0f, 1.0f, 1.0f, 4.0f);
	REQUIRE(u_lift_autoconv_grid_from_samples(m.s.data(), m.w, m.h, m.w, false, 1.0f, 0.0f, 128, &g));
	CHECK(g.w == 128);
	CHECK(g.h == 64);
	CHECK(g.v[0] == Approx(1.0f));   // 1 m = nearest
	CHECK(g.v[127] == Approx(0.0f)); // 4 m = farthest

	// A flat frame has no depth to pick from.
	Map flat(64, 32, 100.0f);
	CHECK_FALSE(u_lift_autoconv_grid_from_samples(flat.s.data(), 64, 32, 64, true, 1.0f / 255.0f, 0.0f, 128, &g));
	// Nothing decodes (linear needs d > 0).
	Map zero(64, 32, 0.0f);
	CHECK_FALSE(u_lift_autoconv_grid_from_samples(zero.s.data(), 64, 32, 64, false, 1.0f, 0.0f, 128, &g));
}

TEST_CASE("lift autoconv: a big centred subject beats a small near detail", "[lift_autoconv]")
{
	Map m(512, 288, 0.0f);
	m.ramp_background(20.0f, 70.0f);            // far background, nearer toward the floor
	m.rect(0.30f, 0.20f, 0.70f, 0.95f, 150.0f); // the subject, centred
	m.rect(0.05f, 0.75f, 0.13f, 0.90f, 250.0f); // a small near detail at the edge
	const u_lift_autoconv_estimate e = estimate(m);
	REQUIRE(e.valid);
	// Normalised over the frame's range (background top = 20, detail = 250).
	const float subject_h = (150.0f - 20.0f) / (250.0f - 20.0f);
	CHECK(e.mode_h == Approx(subject_h).margin(1.0f / 64.0f));
	CHECK(e.convergence == Approx(1.0f - e.mode_h));
	CHECK(e.confidence > 0.3f);
	// Naive "nearest content" would have picked the detail.
	CHECK(e.mode_h < 0.9f);
}

TEST_CASE("lift autoconv: a centred subject wins over a large background wall", "[lift_autoconv]")
{
	// A talking head: a rounded head + torso (40 % x 75 % of the frame) in
	// front of a wall that holds most of the frame in a narrow depth band.
	Map m(512, 288, 0.0f);
	m.ramp_background(30.0f, 60.0f);
	m.dome(0.30f, 0.20f, 0.70f, 0.95f, 150.0f, 25.0f);
	const u_lift_autoconv_estimate e = estimate(m);
	REQUIRE(e.valid);
	// Normalised over the frame's range: wall top = 30 (h 0), head = 150 (h 1).
	const float rim_h = (125.0f - 30.0f) / (150.0f - 30.0f);
	const float top_h = 1.0f;
	CHECK(e.median_h < rim_h);               // the wall holds most of the weight...
	CHECK(e.mode_h >= rim_h - 1.0f / 64.0f); // ...the subject is still picked
	CHECK(e.mode_h <= top_h + 1.0f / 64.0f);
}

TEST_CASE("lift autoconv: between comparable clusters the nearer one wins", "[lift_autoconv]")
{
	Map m(512, 288, 0.0f);
	m.rect(0.0f, 0.0f, 0.5f, 1.0f, 60.0f);  // left half, far
	m.rect(0.5f, 0.0f, 1.0f, 1.0f, 200.0f); // right half, near, same mass and centre weight
	const u_lift_autoconv_estimate e = estimate(m);
	REQUIRE(e.valid);
	CHECK(e.mode_h == Approx(1.0f).margin(1.0f / 32.0f));
}

TEST_CASE("lift autoconv: letterbox bars outside the active region are ignored", "[lift_autoconv]")
{
	Map m(512, 288, 0.0f);
	m.ramp_background(30.0f, 60.0f);
	m.rect(0.25f, 0.25f, 0.75f, 0.85f, 140.0f); // subject
	// Bars: 15 % top and bottom, decoding as NEAR (a module may give the
	// black bars any depth at all).
	m.rect(0.0f, 0.0f, 1.0f, 0.15f, 255.0f);
	m.rect(0.0f, 0.85f, 1.0f, 1.0f, 255.0f);

	const float active[4] = {0.0f, 0.15f, 1.0f, 0.85f};
	const u_lift_autoconv_estimate with_crop = estimate(m, active);
	REQUIRE(with_crop.valid);
	const float subject_h = (140.0f - 30.0f) / (255.0f - 30.0f);
	CHECK(with_crop.mode_h == Approx(subject_h).margin(1.0f / 64.0f));
	// Not one bar cell was counted: the coarse signature has nothing at the front.
	CHECK(with_crop.sig[U_LIFT_AUTOCONV_SIG_BINS - 1] == Approx(0.0f));

	// Without the crop the bars are counted (and hold the top signature bin).
	const u_lift_autoconv_estimate no_crop = estimate(m);
	CHECK(no_crop.sig[U_LIFT_AUTOCONV_SIG_BINS - 1] > 0.25f);
}

TEST_CASE("lift autoconv: a step is followed as a ramp, without overshoot", "[lift_autoconv]")
{
	u_lift_autoconv_filter_params p;
	u_lift_autoconv_filter_params_default(&p);
	u_lift_autoconv_filter f;
	u_lift_autoconv_filter_reset(&f);

	float x = -1.0f;
	CHECK_FALSE(u_lift_autoconv_filter_step(&f, &p, 0, &x)); // nothing measured yet

	uint64_t t = 1000 * kMs;
	u_lift_autoconv_estimate e0 = fake(0.40f);
	u_lift_autoconv_filter_measure(&f, &p, &e0, t);
	REQUIRE(u_lift_autoconv_filter_step(&f, &p, t, &x));
	CHECK(x == Approx(0.40f)); // the first measurement locks

	// A 0.15 step (below the cut threshold, same scene signature), measured
	// every 125 ms; the filter steps every 55 ms (one conversion).
	const u_lift_autoconv_estimate e1 = fake(0.55f);
	float prev = x;
	float max_x = x;
	uint64_t next_measure = t;
	bool first_frame_checked = false;
	for (int i = 0; i < 200; i++) {
		t += 55 * kMs;
		if (t >= next_measure) {
			u_lift_autoconv_filter_measure(&f, &p, &e1, t);
			next_measure = t + 125 * kMs;
		}
		REQUIRE(u_lift_autoconv_filter_step(&f, &p, t, &x));
		CHECK(x >= prev - 1e-6f); // monotonic
		if (!first_frame_checked && x > 0.40f + 1e-4f) {
			first_frame_checked = true;
			CHECK(x - 0.40f < 0.05f); // a ramp, not a snap
		}
		prev = x;
		max_x = x > max_x ? x : max_x;
	}
	CHECK(max_x <= 0.55f + 1e-4f); // no overshoot
	CHECK(x == Approx(0.55f).margin(0.005f));
	CHECK(f.cuts == 0);
}

TEST_CASE("lift autoconv: a single outlier and sub-deadband jitter move nothing", "[lift_autoconv]")
{
	u_lift_autoconv_filter_params p;
	u_lift_autoconv_filter_params_default(&p);
	u_lift_autoconv_filter f;
	u_lift_autoconv_filter_reset(&f);
	uint64_t t = 0;
	u_lift_autoconv_estimate e = fake(0.5f);
	u_lift_autoconv_filter_measure(&f, &p, &e, t);
	for (int i = 0; i < 20; i++) {
		t += 125 * kMs;
		const float jitter = (i % 2 == 0 ? 1.0f : -1.0f) * 0.02f;
		e = (i == 7) ? fake(0.95f) : fake(0.5f + jitter); // one wild measurement
		u_lift_autoconv_filter_measure(&f, &p, &e, t);
		float x;
		REQUIRE(u_lift_autoconv_filter_step(&f, &p, t, &x));
		CHECK(x == Approx(0.5f).margin(1e-4f));
	}
	CHECK(f.cuts == 0);

	// No confident mode: hold.
	u_lift_autoconv_estimate none = {};
	u_lift_autoconv_filter_measure(&f, &p, &none, t + 125 * kMs);
	float x;
	REQUIRE(u_lift_autoconv_filter_step(&f, &p, t + 125 * kMs, &x));
	CHECK(x == Approx(0.5f).margin(1e-4f));
	CHECK(f.held == 1);
}

TEST_CASE("lift autoconv: a confirmed scene cut ramps fast, never in one frame", "[lift_autoconv]")
{
	u_lift_autoconv_filter_params p;
	u_lift_autoconv_filter_params_default(&p);

	// Time from the cut until x is within 10 % of the jump, for the normal and
	// the cut follower.
	auto settle_ms = [&](bool new_scene, uint32_t *out_cuts, float *out_first_step) {
		u_lift_autoconv_filter f;
		u_lift_autoconv_filter_reset(&f);
		uint64_t t = 0;
		u_lift_autoconv_estimate e = fake(0.2f, 2.0f);
		u_lift_autoconv_filter_measure(&f, &p, &e, t);
		float x = 0.0f;
		u_lift_autoconv_filter_step(&f, &p, t, &x);
		const uint64_t t_cut = 1000 * kMs;
		t = t_cut;
		uint64_t next_measure = t;
		uint64_t done = 0;
		*out_first_step = -1.0f;
		for (int i = 0; i < 200 && done == 0; i++) {
			if (t >= next_measure) {
				// A different scene: a new mode AND (or not) a new signature.
				e = fake(0.8f, new_scene ? 12.0f : 2.0f);
				u_lift_autoconv_filter_measure(&f, &p, &e, t);
				next_measure = t + 125 * kMs;
			}
			const float before = x;
			u_lift_autoconv_filter_step(&f, &p, t, &x);
			if (*out_first_step < 0.0f && x > before + 1e-4f) {
				*out_first_step = x - before;
			}
			if (x > 0.8f - 0.06f) {
				done = t;
			}
			t += 55 * kMs;
		}
		*out_cuts = f.cuts;
		return (double)(done - t_cut) / (double)kMs;
	};

	uint32_t cuts = 0;
	float first = 0.0f;
	const double cut_ms = settle_ms(true, &cuts, &first);
	CHECK(cuts == 1);
	CHECK(cut_ms < 700.0);      // confirmation (two more samples) + a ~200 ms ramp
	CHECK(first < 0.6f * 0.6f); // the first moving frame covers well under the jump
	CHECK(first > 0.0f);

	// The same jump with the scene signature unchanged is NOT a cut (a subject
	// change inside a shot, or two clusters trading places): followed slowly.
	uint32_t cuts2 = 0;
	float first2 = 0.0f;
	const double ms2 = settle_ms(false, &cuts2, &first2);
	CHECK(cuts2 == 0);
	CHECK(ms2 > 1500.0);

	// Contrast: the normal follower takes seconds for the same step.
	u_lift_autoconv_filter f;
	u_lift_autoconv_filter_reset(&f);
	u_lift_autoconv_estimate e = fake(0.2f);
	u_lift_autoconv_filter_measure(&f, &p, &e, 0);
	f.target = 0.8f; // as if the median had moved without a cut
	float x = 0.0f;
	u_lift_autoconv_filter_step(&f, &p, 600 * kMs, &x);
	CHECK(x < 0.8f - 0.06f);
}

TEST_CASE("lift autoconv: cuts are rate-limited by the refractory period", "[lift_autoconv]")
{
	u_lift_autoconv_filter_params p;
	u_lift_autoconv_filter_params_default(&p);
	u_lift_autoconv_filter f;
	u_lift_autoconv_filter_reset(&f);
	uint64_t t = 0;
	u_lift_autoconv_estimate e = fake(0.2f, 2.0f);
	u_lift_autoconv_filter_measure(&f, &p, &e, t);
	// Scene changes alternating every 4 samples (0.5 s): only cuts at least
	// cut_refractory_s apart may count.
	for (int i = 1; i <= 48; i++) {
		t += 125 * kMs;
		const bool a = ((i / 4) % 2) == 0;
		e = a ? fake(0.2f, 2.0f) : fake(0.8f, 12.0f);
		u_lift_autoconv_filter_measure(&f, &p, &e, t);
	}
	// 6 s of input: at most one cut per 1.5 s.
	CHECK(f.cuts >= 1);
	CHECK(f.cuts <= 4);
}

TEST_CASE("lift autoconv: the followed subject is kept against a comparable rival", "[lift_autoconv]")
{
	// Two comparable clusters: left half far, right half near (same mass).
	Map m(512, 288, 0.0f);
	m.rect(0.0f, 0.0f, 0.5f, 1.0f, 60.0f);
	m.rect(0.5f, 0.0f, 1.0f, 1.0f, 200.0f);
	static u_lift_autoconv_grid g;
	REQUIRE(u_lift_autoconv_grid_from_samples(m.s.data(), m.w, m.h, m.w, true, 1.0f / 255.0f, 0.0f, 128, &g));
	u_lift_autoconv_params p;
	u_lift_autoconv_params_default(&p);
	mode_only(&p);
	p.fg_bias = 0.0f; // equal scores: which one wins is down to hysteresis alone
	u_lift_autoconv_estimate e;
	// Following the near one: it is kept.
	p.prefer_h = 1.0f;
	REQUIRE(u_lift_autoconv_find_subject(&g, nullptr, &p, &e));
	CHECK(e.peak_h == Approx(1.0f).margin(1.0f / 32.0f));
	// Following the far one: it is kept (the other does not win by switch_ratio).
	p.prefer_h = 0.0f;
	REQUIRE(u_lift_autoconv_find_subject(&g, nullptr, &p, &e));
	CHECK(e.peak_h == Approx(0.0f).margin(1.0f / 32.0f));

	// With a foreground bias the near cluster scores ~3x the far one: a clear
	// winner takes over even from a followed subject.
	u_lift_autoconv_params_default(&p);
	mode_only(&p);
	p.prefer_h = 0.0f;
	REQUIRE(u_lift_autoconv_find_subject(&g, nullptr, &p, &e));
	CHECK(e.peak_h == Approx(1.0f).margin(1.0f / 32.0f));
}

TEST_CASE("lift autoconv: a large far background never takes the screen plane", "[lift_autoconv]")
{
	// The failure seen on the panel: sky / distant scenery fills most of the
	// frame as one flat far plateau; the old foreground-biased mode put the
	// screen on it and pushed ~90 % of the frame out.
	Map m(512, 288, 0.0f);
	m.rect(0.0f, 0.0f, 1.0f, 1.0f, 20.0f);      // far plateau everywhere...
	m.rect(0.35f, 0.40f, 0.65f, 0.95f, 140.0f); // ...a centred subject (~16 % of the frame)
	m.rect(0.05f, 0.80f, 0.12f, 0.95f, 250.0f); // a small near detail
	const float subject_h = (140.0f - 20.0f) / (250.0f - 20.0f);

	const u_lift_autoconv_estimate old = estimate(m); // foreground-biased mode, far end allowed
	REQUIRE(old.valid);
	CHECK(old.mode_h < 0.1f); // the plateau wins: the regression this guards

	const u_lift_autoconv_estimate e = estimate(m, nullptr, false); // the default rule
	REQUIRE(e.valid);
	CHECK(e.peak_h == Approx(subject_h).margin(1.0f / 32.0f)); // the far end is excluded
	CHECK(e.mode_h > 0.25f);                                   // the plane moved off the background
	CHECK(e.mode_h <= subject_h + 1.0f / 32.0f);               // ...but not in front of the subject
}

TEST_CASE("lift autoconv: the percentile puts about a quarter of the weight in front", "[lift_autoconv]")
{
	// Two layers, 70 % far / 30 % near (by centre-weighted area): p80 lands in
	// the near layer, i.e. the near layer sits at (or just in front of) the plane.
	Map m(512, 288, 0.0f);
	m.rect(0.0f, 0.0f, 1.0f, 1.0f, 60.0f);
	m.rect(0.0f, 0.0f, 1.0f, 0.30f, 200.0f); // top band near (a bit less centre weight)
	const u_lift_autoconv_estimate e = estimate(m, nullptr, false);
	REQUIRE(e.valid);
	CHECK(e.pct_h > e.median_h);
	CHECK(e.mode_h > 0.0f);
	CHECK(e.mode_h <= 1.0f);
}
