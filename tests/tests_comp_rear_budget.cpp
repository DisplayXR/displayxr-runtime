// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for comp_rear_budget — the per-session rear-depth-budget
 *         runner shared by the native compositors (XR_DXR_depth_budget).
 *
 * `tests_aux_rear_budget` covers the policy and `tests_aux_bg_neutrality` the
 * measurement; what is left — and what this covers — is the *runner*: the
 * cadence rules that sit between them and that each backend used to own a copy
 * of. Three of them are load-bearing and none is visible from either pure
 * layer:
 *
 * - a DP that declines is a source that does not exist, not a neutral one,
 * - a preview whose generation stops advancing is NOT stale (capture sources
 *   deliver only on change, so a quiet desktop is the best case, and
 *   re-analysing it is pure waste),
 * - the policy is ticked EVERY frame while the DP is polled at most every
 *   66 ms — so the ramp must keep moving on frames that asked the DP nothing,
 * - and the DXR_REAR_BUDGET_DUMP picture must survive to the frame that needs
 *   it: a transition fires when a dwell elapses, which is essentially never a
 *   polling frame, so the runner has to keep its own copy of what it analysed.
 *
 * A synthetic BGRA preview stands in for the display processor, so this runs
 * with no GPU, no window and no plug-in.
 */

#include "util/comp_rear_budget.h"

#include "catch_amalgamated.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint64_t MS = 1000000ULL;

//! A DP-shaped background preview backed by a buffer the caller owns.
struct FakePreview
{
	std::vector<uint8_t> bytes;
	xrt_dp_background_preview pv{};

	//! Paint a text-like (1-px vertical stripe) patch into an otherwise flat preview.
	void
	paint_busy_patch(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
	{
		for (uint32_t y = y0; y < y1; y++) {
			for (uint32_t x = x0; x < x1; x++) {
				const uint8_t v = (x & 1u) ? 255 : 0;
				uint8_t *p = &bytes[((size_t)y * pv.width + x) * 4u];
				p[0] = v;
				p[1] = v;
				p[2] = v;
			}
		}
	}

	FakePreview(uint32_t w, uint32_t h, uint32_t generation, bool busy)
	{
		bytes.resize((size_t)w * h * 4u);
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x++) {
				// Busy = 1-px vertical stripes: every horizontal
				// difference sample crosses the edge threshold, which is
				// exactly the cue the rear budget exists to avoid fighting.
				// Neutral = one flat colour, which carries none.
				const uint8_t v = busy ? ((x & 1u) ? 255 : 0) : 128;
				uint8_t *p = &bytes[((size_t)y * w + x) * 4u];
				p[0] = v; // B
				p[1] = v; // G
				p[2] = v; // R
				p[3] = 255;
			}
		}
		xrt_dp_background_preview_init(&pv);
		pv.generation = generation;
		pv.width = w;
		pv.height = h;
		pv.stride_bytes = w * 4u;
		pv.bgra = bytes.data();
	}
};

//! An armed, transparent, opted-in session.
struct Runner
{
	comp_rear_budget b{};

	Runner()
	{
		comp_rear_budget_init(&b, "test");
		comp_rear_budget_set_requested(&b, true);
		comp_rear_budget_arm(&b, /*transparent=*/true);
	}
	~Runner() { comp_rear_budget_fini(&b); }

	Runner(const Runner &) = delete;
	Runner &operator=(const Runner &) = delete;
};

/*!
 * One frame, exactly as a compositor drives it: ask whether this frame is due
 * to poll, call the (fake) DP only if it is, hand over what came back.
 *
 * @param pv NULL models a DP with no source — either no slot at all or a slot
 *           that declined this frame.
 * @return whether this frame actually polled.
 */
bool
step(Runner &r, const xrt_dp_background_preview *pv, uint64_t now_ns)
{
	const bool polled = comp_rear_budget_should_poll(&r.b, now_ns);
	comp_rear_budget_tick(&r.b, polled ? pv : nullptr, polled, /*transparent=*/true, now_ns);
	return polled;
}

//! The ROI the runner last analysed through; fails the test if none has been.
u_bg_roi
roi_of(Runner &r, bool *out_narrowed = nullptr)
{
	u_bg_roi roi{};
	bool narrowed = false;
	REQUIRE(comp_rear_budget_debug_last_roi(&r.b, &roi, &narrowed));
	if (out_narrowed != nullptr) {
		*out_narrowed = narrowed;
	}
	return roi;
}

//! Canvas-normalised content bounds, as xrEndFrame would forward them.
void
bounds(Runner &r, float u0, float v0, float u1, float v1, uint64_t now_ns)
{
	comp_rear_budget_set_content_bounds(&r.b, u0, v0, u1, v1, now_ns);
}

u_rear_budget_out
read(Runner &r)
{
	u_rear_budget_out out{};
	REQUIRE(comp_rear_budget_get(&r.b, &out));
	return out;
}

//! Drive @p ms of frames at 10 ms each, ending on a tick at exactly `start+ms`.
uint64_t
run_for(Runner &r, const xrt_dp_background_preview *pv, uint64_t start_ns, uint64_t ms)
{
	for (uint64_t t = start_ns; t <= start_ns + ms * MS; t += 10 * MS) {
		step(r, pv, t);
	}
	return start_ns + ms * MS;
}

/*!
 * Set an environment variable for the length of a scope and put it back.
 *
 * The kill switch is probed once per runner, from the environment, so the only
 * honest way to test it is through the environment - and the only safe way to
 * do that in a single-process test binary is to restore it afterwards.
 */
struct ScopedEnv
{
	const char *name;
	std::string previous;
	bool had_previous;

	ScopedEnv(const char *n, const char *value) : name(n), had_previous(false)
	{
		const char *old = getenv(n);
		if (old != nullptr) {
			previous = old;
			had_previous = true;
		}
		set(value);
	}
	~ScopedEnv() { set(had_previous ? previous.c_str() : nullptr); }

	void
	set(const char *value)
	{
#ifdef _WIN32
		_putenv_s(name, value != nullptr ? value : "");
#else
		if (value != nullptr) {
			setenv(name, value, 1);
		} else {
			unsetenv(name);
		}
#endif
	}

	ScopedEnv(const ScopedEnv &) = delete;
	ScopedEnv &operator=(const ScopedEnv &) = delete;
};

//! Stands in for the PNG writer so the dump path is observable and writes nothing.
struct DumpSink
{
	int calls = 0;
	uint32_t w = 0, h = 0, stride = 0;
	uint8_t first_pixel_b = 0;

	static void
	fn(void *ctx, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride)
	{
		auto *self = static_cast<DumpSink *>(ctx);
		self->calls++;
		self->w = w;
		self->h = h;
		self->stride = stride;
		self->first_pixel_b = (bgra != nullptr) ? bgra[0] : 0;
	}
};

} // namespace


TEST_CASE("comp_rear_budget: a session that never opted in has no policy to read")
{
	comp_rear_budget b{};
	comp_rear_budget_init(&b, "test");
	comp_rear_budget_arm(&b, /*transparent=*/true);

	// False, NOT "unrestricted": the caller must apply the extension's own
	// zero-default rule, which differs by transparency. Answering here would
	// make this runner a second, quieter source of truth for that rule.
	u_rear_budget_out out{};
	REQUIRE_FALSE(comp_rear_budget_get(&b, &out));

	comp_rear_budget_fini(&b);
}

TEST_CASE("comp_rear_budget: an opaque session answers unrestricted without a tick")
{
	comp_rear_budget b{};
	comp_rear_budget_init(&b, "test");
	comp_rear_budget_set_requested(&b, true);
	comp_rear_budget_arm(&b, /*transparent=*/false);

	// The render thread never ticks for an opaque session, so a value that
	// waited for one would never arrive.
	u_rear_budget_out out{};
	REQUIRE(comp_rear_budget_get(&b, &out));
	CHECK(out.state == U_REAR_BUDGET_UNRESTRICTED_OPAQUE);
	CHECK(out.far_offset_vh > U_REAR_BUDGET_UNRESTRICTED_VH - 0.5f);

	comp_rear_budget_fini(&b);
}

TEST_CASE("comp_rear_budget: no preview clips, for seconds, without ever opening")
{
	Runner r;

	// Before the first tick the runner already answers — the conservative
	// answer, so the app's first frames are unchanged rather than briefly and
	// wrongly open.
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);

	run_for(r, nullptr, 0, 3000);

	const u_rear_budget_out out = read(r);
	CHECK(out.state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);
	CHECK(out.far_offset_vh < 0.001f);
	// "No answer" must never be reported as a measurement.
	CHECK(out.cue_energy < 0.001f);
}

TEST_CASE("comp_rear_budget: an invalid preview is no source at all")
{
	Runner r;

	// A 1-px-wide buffer cannot hold a single horizontal difference sample, so
	// there is nothing to measure. It must read as "no source", never as
	// "neutral" — the whole point of the dwell is that opening is earned.
	FakePreview too_narrow(1, 8, 1, /*busy=*/false);
	run_for(r, &too_narrow.pv, 0, 2000);
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);

	// Same for a source that positively flags its own preview stale.
	Runner r2;
	FakePreview flagged(64, 32, 1, /*busy=*/false);
	flagged.pv.flags |= XRT_DP_BG_PREVIEW_STALE;
	run_for(r2, &flagged.pv, 0, 2000);
	CHECK(read(r2).state == U_REAR_BUDGET_CLIPPED_NO_SOURCE);
}

TEST_CASE("comp_rear_budget: a neutral background opens, and a frozen generation keeps it open")
{
	Runner r;
	FakePreview neutral(64, 32, /*generation=*/7, /*busy=*/false);

	// The dwell (400 ms) then the ramp (300 ms).
	uint64_t t = run_for(r, &neutral.pv, 0, 800);
	u_rear_budget_out out = read(r);
	CHECK(out.state == U_REAR_BUDGET_OPEN);
	CHECK(out.far_offset_vh > U_REAR_BUDGET_UNRESTRICTED_VH - 0.5f);

	// The generation never advances from here — a capture source delivers only
	// when the desktop CHANGES, so this is a quiet desktop, i.e. the best case.
	// It must not decay into staleness.
	run_for(r, &neutral.pv, t + 10 * MS, 5000);
	out = read(r);
	CHECK(out.state == U_REAR_BUDGET_OPEN);
	CHECK(out.far_offset_vh > U_REAR_BUDGET_UNRESTRICTED_VH - 0.5f);
}

TEST_CASE("comp_rear_budget: a busy background closes it, fast")
{
	Runner r;
	FakePreview neutral(64, 32, 1, /*busy=*/false);
	uint64_t t = run_for(r, &neutral.pv, 0, 800);
	REQUIRE(read(r).state == U_REAR_BUDGET_OPEN);

	// A new generation, because a capture that CHANGED is exactly what makes
	// the runner re-measure. Closing is deliberately much quicker than
	// opening: a visible occlusion conflict is worse than a missing rear.
	FakePreview busy(64, 32, 2, /*busy=*/true);
	t = run_for(r, &busy.pv, t + 10 * MS, 400);

	const u_rear_budget_out out = read(r);
	CHECK(out.state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
	CHECK(out.far_offset_vh < 0.001f);
	CHECK(out.cue_energy > 0.0f);
}

TEST_CASE("comp_rear_budget: a busy background never opens in the first place")
{
	Runner r;
	FakePreview busy(64, 32, 1, /*busy=*/true);
	run_for(r, &busy.pv, 0, 3000);
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
}

TEST_CASE("comp_rear_budget: the ramp advances on frames that polled nothing")
{
	Runner r;
	FakePreview neutral(64, 32, 1, /*busy=*/false);

	// Sit out the dwell so the next frames are mid-ramp.
	uint64_t t = run_for(r, &neutral.pv, 0, 410) + 10 * MS;

	// Land on a polling frame, so the following 50 ms provably contains none
	// (the DP throttle is 66 ms).
	while (!step(r, &neutral.pv, t)) {
		t += 10 * MS;
	}
	const float before = read(r).far_offset_vh;
	REQUIRE(before < U_REAR_BUDGET_UNRESTRICTED_VH); // still mid-ramp

	for (int i = 1; i <= 5; i++) {
		CHECK_FALSE(step(r, &neutral.pv, t + (uint64_t)i * 10 * MS));
	}
	const float after = read(r).far_offset_vh;

	// If the runner only advanced the policy on polling frames the clip plane
	// would step ~15 times a second instead of sliding, which is the artefact
	// the ramp exists to avoid.
	CHECK(after > before);
}

TEST_CASE("comp_rear_budget: an unarmed runner does no work")
{
	comp_rear_budget b{};
	comp_rear_budget_init(&b, "test");
	comp_rear_budget_set_requested(&b, false);
	comp_rear_budget_arm(&b, /*transparent=*/true);

	CHECK_FALSE(comp_rear_budget_is_running(&b));
	// An app that never asked must not pay for the poll — this is the gate the
	// extension's "an app that never asks costs nothing" promise rests on.
	CHECK_FALSE(comp_rear_budget_should_poll(&b, 0));
	CHECK_FALSE(comp_rear_budget_should_poll(&b, 10ULL * 1000 * MS));

	comp_rear_budget_fini(&b);
}

TEST_CASE("comp_rear_budget: the dump fires on a transition, from a frame that polled nothing")
{
	Runner r;
	DumpSink sink;
	comp_rear_budget_debug_set_dump_sink(&r.b, DumpSink::fn, &sink);

	FakePreview neutral(64, 32, /*generation=*/1, /*busy=*/false);

	// The preview is retained the moment it is ANALYSED...
	step(r, &neutral.pv, 0);
	uint32_t w = 0, h = 0;
	REQUIRE(comp_rear_budget_debug_last_preview(&r.b, &w, &h));
	CHECK(w == 64);
	CHECK(h == 32);

	// ...and the transition it explains lands ~400 ms later, on a frame chosen
	// by the dwell, not by the poll throttle. Gating the dump on `polled` made
	// it unreachable: 66 ms between polls, 10 ms between these frames.
	CHECK(sink.calls == 0);
	run_for(r, &neutral.pv, 10 * MS, 800);
	REQUIRE(read(r).state == U_REAR_BUDGET_OPEN);

	CHECK(sink.calls == 1);
	CHECK(sink.w == 64);
	CHECK(sink.h == 32);
	// Tightly packed by the runner, whatever stride the DP handed over.
	CHECK(sink.stride == 64 * 4);
	CHECK(sink.first_pixel_b == 128); // the neutral fill
}

TEST_CASE("comp_rear_budget: the retained preview survives non-polling frames and refreshes on a new generation")
{
	Runner r;
	DumpSink sink;
	comp_rear_budget_debug_set_dump_sink(&r.b, DumpSink::fn, &sink);

	// A preview whose backing buffer goes away, exactly like the DP-owned one:
	// it is only valid until the next process_atlas.
	{
		FakePreview transient(64, 32, 1, /*busy=*/false);
		step(r, &transient.pv, 0);
	}

	// Hundreds of frames with nothing to poll from. The copy is the runner's,
	// so it is still there — and still the right size.
	uint32_t w = 0, h = 0;
	for (uint64_t t = 10 * MS; t <= 1500 * MS; t += 10 * MS) {
		step(r, nullptr, t);
	}
	REQUIRE(comp_rear_budget_debug_last_preview(&r.b, &w, &h));
	CHECK(w == 64);
	CHECK(h == 32);

	// A new generation with different dimensions replaces it (and grows the
	// runner's buffer rather than writing past the old one).
	FakePreview bigger(128, 96, /*generation=*/2, /*busy=*/true);
	run_for(r, &bigger.pv, 1510 * MS, 500);
	REQUIRE(comp_rear_budget_debug_last_preview(&r.b, &w, &h));
	CHECK(w == 128);
	CHECK(h == 96);

	// And the transition that new picture explains was dumped with it.
	REQUIRE(sink.calls > 0);
	CHECK(sink.w == 128);
	CHECK(sink.h == 96);
}

TEST_CASE("comp_rear_budget: nothing is retained while the dump is off")
{
	Runner r;
	FakePreview neutral(64, 32, 1, /*busy=*/false);
	run_for(r, &neutral.pv, 0, 800);
	REQUIRE(read(r).state == U_REAR_BUDGET_OPEN);

	// The copy is opt-in: an un-armed session must not pay ~0.5 MB and a memcpy
	// per capture for a picture nobody asked for.
	CHECK_FALSE(comp_rear_budget_debug_last_preview(&r.b, nullptr, nullptr));
}


/*
 * -----------------------------------------------------------------------------
 * v2: the content-bounds ROI (#1365)
 * -----------------------------------------------------------------------------
 */

TEST_CASE("comp_rear_budget: with no content bounds the ROI is the whole preview")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);
	step(r, &neutral.pv, 0);

	// v1's behaviour, and the floor every failure path in v2 falls back to.
	bool narrowed = true;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.x == 0);
	CHECK(roi.y == 0);
	CHECK(roi.w == 200);
	CHECK(roi.h == 100);
	CHECK_FALSE(narrowed);
}

TEST_CASE("comp_rear_budget: content bounds map to preview pixels and dilate")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	// Canvas 0.4..0.6 x 0.4..0.6 over a 200x100 preview covering the whole
	// canvas = px 80..120 x 40..60. Dilation is max(4% of 200, 8) = 8 px per
	// side, so 72..128 x 32..68.
	bounds(r, 0.4f, 0.4f, 0.6f, 0.6f, 0);
	step(r, &neutral.pv, 0);

	bool narrowed = false;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.x == 72);
	CHECK(roi.y == 32);
	CHECK(roi.w == 56);
	CHECK(roi.h == 36);
	CHECK(narrowed);
}

TEST_CASE("comp_rear_budget: the ROI maps through a preview that carries a margin")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);
	// A display processor that captured 25% beyond the canvas on every side:
	// the preview spans canvas -0.25..1.25, so canvas 0.5 sits at px 100 of
	// 200 only because the mapping accounts for it. Reading the bounds as if
	// the preview were the canvas would aim 33 px to the left.
	neutral.pv.canvas_u0 = -0.25f;
	neutral.pv.canvas_v0 = -0.25f;
	neutral.pv.canvas_u1 = 1.25f;
	neutral.pv.canvas_v1 = 1.25f;

	// u 0.25..0.75 -> (0.25+0.25)/1.5*200 = 66.67 .. (0.75+0.25)/1.5*200 = 133.3
	// dilate 8 -> 58.67..141.3 -> floor/ceil -> 58..142.
	bounds(r, 0.25f, 0.25f, 0.75f, 0.75f, 0);
	step(r, &neutral.pv, 0);

	const u_bg_roi roi = roi_of(r);
	CHECK(roi.x == 58);
	CHECK(roi.w == 142 - 58);
}

TEST_CASE("comp_rear_budget: the ROI clamps to the preview")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	// Content filling the canvas: dilation pushes the rect past every edge and
	// the preview, not the arithmetic, is what stops it.
	bounds(r, 0.0f, 0.0f, 1.0f, 1.0f, 0);
	step(r, &neutral.pv, 0);

	bool narrowed = true;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.x == 0);
	CHECK(roi.y == 0);
	CHECK(roi.w == 200);
	CHECK(roi.h == 100);
	CHECK_FALSE(narrowed); // it IS the whole preview, however it got there
}

TEST_CASE("comp_rear_budget: bounds that clamp away entirely fall back to the whole preview")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);
	// The preview only covers the left fifth of the canvas; content on the
	// right of the canvas is nowhere in it. A degenerate ROI must never be
	// measured - an empty region reads as neutral, and would open the budget
	// over a desktop nobody looked at.
	neutral.pv.canvas_u0 = 0.0f;
	neutral.pv.canvas_v0 = 0.0f;
	neutral.pv.canvas_u1 = 0.2f;
	neutral.pv.canvas_v1 = 1.0f;

	bounds(r, 0.9f, 0.1f, 0.95f, 0.9f, 0);
	step(r, &neutral.pv, 0);

	bool narrowed = true;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.w == 200);
	CHECK(roi.h == 100);
	CHECK_FALSE(narrowed);
}

TEST_CASE("comp_rear_budget: an unknown extent is the whole preview")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	// What oxr forwards for a malformed or absent rect.
	bounds(r, 0.0f, 0.0f, 0.0f, 0.0f, 0);
	step(r, &neutral.pv, 0);

	bool narrowed = true;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.w == 200);
	CHECK_FALSE(narrowed);
}

TEST_CASE("comp_rear_budget: bounds older than a second stop narrowing")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	bounds(r, 0.4f, 0.4f, 0.6f, 0.6f, 0);
	step(r, &neutral.pv, 0);
	REQUIRE(roi_of(r).w == 56);

	// The app stopped chaining. A rect from a second ago describes geometry
	// that has since moved, so it is worse than no rect at all.
	run_for(r, &neutral.pv, 10 * MS, 1500);

	bool narrowed = true;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.w == 200);
	CHECK(roi.h == 100);
	CHECK_FALSE(narrowed);
}

TEST_CASE("comp_rear_budget: busy pixels outside the ROI keep the budget open")
{
	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	// A text-like patch in the right-hand quarter - the empty-window menu bar
	// that read cue 0.93 on the v1 panel run.
	pv.paint_busy_patch(150, 0, 200, 100);

	// The content sits on the LEFT: canvas 0.05..0.35 -> px 10..70, dilated by
	// 8 -> 2..78. The patch starts at 150, well clear of it.
	bounds(r, 0.05f, 0.1f, 0.35f, 0.9f, 0);
	run_for(r, &pv.pv, 0, 800);

	bool narrowed = false;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(narrowed);
	CHECK(roi.x + roi.w <= 150);

	const u_rear_budget_out out = read(r);
	// v1 measured this whole preview and closed. That is the regression this
	// feature exists to remove.
	CHECK(out.state == U_REAR_BUDGET_OPEN);
	CHECK(out.far_offset_vh > U_REAR_BUDGET_UNRESTRICTED_VH - 0.5f);
}

TEST_CASE("comp_rear_budget: busy pixels inside the ROI still close it")
{
	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	pv.paint_busy_patch(150, 0, 200, 100);

	// Same preview, same patch - the content is now over it.
	bounds(r, 0.75f, 0.1f, 0.95f, 0.9f, 0);
	run_for(r, &pv.pv, 0, 800);

	bool narrowed = false;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(narrowed);
	CHECK(roi.x < 150);
	CHECK(roi.x + roi.w > 150);

	const u_rear_budget_out out = read(r);
	CHECK(out.state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
	CHECK(out.far_offset_vh < 0.001f);
	CHECK(out.cue_energy > 0.0f);
}

TEST_CASE("comp_rear_budget: moving the content re-measures the same capture")
{
	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	pv.paint_busy_patch(150, 0, 200, 100);

	bounds(r, 0.05f, 0.1f, 0.35f, 0.9f, 0);
	uint64_t t = run_for(r, &pv.pv, 0, 800);
	REQUIRE(read(r).state == U_REAR_BUDGET_OPEN);

	// The desktop never changes again, so the generation never advances - on a
	// quiet desktop it never will. Gating re-analysis on the generation alone
	// would leave the verdict pinned to where the content USED to be.
	for (uint64_t u = t + 10 * MS; u <= t + 500 * MS; u += 10 * MS) {
		bounds(r, 0.75f, 0.1f, 0.95f, 0.9f, u);
		step(r, &pv.pv, u);
	}

	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
}

TEST_CASE("comp_rear_budget: DXR_REAR_BUDGET_ROI=0 disables the narrowing")
{
	ScopedEnv off("DXR_REAR_BUDGET_ROI", "0");

	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	pv.paint_busy_patch(150, 0, 200, 100);

	// The same setup that stays OPEN with the ROI on. With the switch armed
	// the analysis sees the whole preview again, so the A/B has two visibly
	// different arms rather than one arm and a no-op.
	bounds(r, 0.05f, 0.1f, 0.35f, 0.9f, 0);
	run_for(r, &pv.pv, 0, 800);

	bool narrowed = true;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.w == 200);
	CHECK(roi.h == 100);
	CHECK_FALSE(narrowed);
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
}
