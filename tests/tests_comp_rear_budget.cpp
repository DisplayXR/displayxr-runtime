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

//! Which rule produced the last ROI.
comp_rear_budget_roi_src
src_of(Runner &r)
{
	return comp_rear_budget_debug_last_roi_src(&r.b);
}

//! Window-normalised content bounds, as xrEndFrame would forward them.
void
bounds(Runner &r, float u0, float v0, float u1, float v1, uint64_t now_ns)
{
	comp_rear_budget_set_content_bounds(&r.b, u0, v0, u1, v1, now_ns);
}

/*!
 * The frame's 3D display zones, window-normalised — what each compositor's
 * per-frame layer scan publishes. An empty list is a frame with no zones, i.e.
 * "the whole canvas is the 3D zone".
 */
void
zones(Runner &r, const std::vector<u_bg_rect_norm> &rects, uint64_t now_ns)
{
	comp_rear_budget_set_zone_rects(&r.b, rects.empty() ? nullptr : rects.data(), (uint32_t)rects.size(),
	                                now_ns);
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


/*
 * -----------------------------------------------------------------------------
 * v2: clamping the ROI to the frame's 3D zones (#1365)
 *
 * The bounds contract is WINDOW-normalised, and a zoned app can get that wrong
 * in a way no validation catches - a zone-normalised projection chained as
 * window-normalised, or animation bounds projecting outside the frustum. The
 * background preview covers the whole window, so the analysis then measures
 * desktop behind a Local2D 2D band that 3D content never covers, and the
 * verdict still reads authoritative. These pin the runtime's defence.
 *
 * Geometry shared by the cases below: a 200x100 preview over the whole canvas,
 * one 3D zone occupying the LEFT HALF of the window (u 0..0.5 -> px 0..100),
 * and the right half a 2D band.
 * -----------------------------------------------------------------------------
 */

TEST_CASE("comp_rear_budget: bounds reaching past a 3D zone are clipped to it")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, 0);
	// Content bounds that spill 0.3 of the window past the zone's right edge.
	bounds(r, 0.3f, 0.2f, 0.8f, 0.8f, 0);
	step(r, &neutral.pv, 0);

	// Intersection is u 0.3..0.5 x v 0.2..0.8 -> px 60..100 x 20..80, dilated
	// by 8 -> 52..108 x 12..88, and the zone box (px 0..100) is what stops the
	// right edge at 100 - the dilation is clamped too, because a band is just
	// as able to reach into the 2D strip as the bounds were.
	bool narrowed = false;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.x == 52);
	CHECK(roi.y == 12);
	CHECK(roi.w == 100 - 52);
	CHECK(roi.h == 88 - 12);
	CHECK(narrowed);
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_BOUNDS_IN_ZONES);
}

TEST_CASE("comp_rear_budget: bounds entirely outside every 3D zone measure the zone union")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, 0);
	// Squarely inside the 2D band - the app's zone->window rebase is missing.
	bounds(r, 0.6f, 0.2f, 0.9f, 0.8f, 0);
	step(r, &neutral.pv, 0);

	// Never the whole window (that is the bug), never neutral (that would open
	// the budget over a desktop nobody measured): the zone union itself.
	bool narrowed = false;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.x == 0);
	CHECK(roi.y == 0);
	CHECK(roi.w == 100);
	CHECK(roi.h == 100);
	CHECK(narrowed);
	// The branch that emits the one-shot "check the app's zone->window rebase"
	// WARN. Re-deriving it must stay on the same branch - the log is one-shot,
	// the clamp is not.
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_ZONES_BOUNDS_OUTSIDE);

	bounds(r, 0.6f, 0.2f, 0.9f, 0.8f, 100 * MS);
	zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, 100 * MS);
	run_for(r, &neutral.pv, 100 * MS, 200);
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_ZONES_BOUNDS_OUTSIDE);
	CHECK(roi_of(r).w == 100);
}

TEST_CASE("comp_rear_budget: a frame with no zones is unchanged v2 behaviour")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	// The full-window apps (modelviewer, gauss): the whole canvas IS the 3D
	// zone, so an empty zone list must clamp nothing at all.
	zones(r, {}, 0);
	bounds(r, 0.4f, 0.4f, 0.6f, 0.6f, 0);
	step(r, &neutral.pv, 0);

	const u_bg_roi roi = roi_of(r);
	CHECK(roi.x == 72);
	CHECK(roi.y == 32);
	CHECK(roi.w == 56);
	CHECK(roi.h == 36);
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_BOUNDS);
}

TEST_CASE("comp_rear_budget: zones older than a second stop clamping")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, 0);

	// The app stopped chaining zones. A layout from a second ago describes a
	// window that has since moved, so it is worse than no layout at all - and
	// the bounds, which ARE fresh, are then trusted as they were in v2.
	bounds(r, 0.6f, 0.2f, 0.9f, 0.8f, 2000 * MS);
	step(r, &neutral.pv, 2000 * MS);

	// px 120..180 x 20..80, dilated by 8, with no zone box to stop it.
	const u_bg_roi roi = roi_of(r);
	CHECK(roi.x == 112);
	CHECK(roi.w == 188 - 112);
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_BOUNDS);
}

TEST_CASE("comp_rear_budget: a busy 2D band under bounds spanning both bands stays open")
{
	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	// The Local2D band's desktop, text-like. Nothing the 3D content can ever
	// occlude - it is not woven there.
	pv.paint_busy_patch(150, 0, 200, 100);

	zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, 0);
	// Bounds that span the 3D zone AND the 2D band, which is exactly what the
	// zoned Unity app reported on the panel.
	bounds(r, 0.3f, 0.1f, 0.9f, 0.9f, 0);
	run_for(r, &pv.pv, 0, 800);

	const u_bg_roi roi = roi_of(r);
	CHECK(roi.x + roi.w <= 100); // the clamp excludes the patch entirely
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_BOUNDS_IN_ZONES);

	// Unclamped, this closed the budget on pixels behind the 2D band. That is
	// the regression #1365 exists to remove.
	const u_rear_budget_out out = read(r);
	CHECK(out.state == U_REAR_BUDGET_OPEN);
	CHECK(out.far_offset_vh > U_REAR_BUDGET_UNRESTRICTED_VH - 0.5f);
}

TEST_CASE("comp_rear_budget: zones with no usable bounds measure the zones, not the window")
{
	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	pv.paint_busy_patch(150, 0, 200, 100);

	// An app that chains zones but no content bounds. v2 measured the whole
	// preview here; outside a 3D zone there is nothing to occlude, so the
	// honest default region is what the frame actually weaves.
	zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, 0);
	run_for(r, &pv.pv, 0, 800);

	const u_bg_roi roi = roi_of(r);
	CHECK(roi.w == 100);
	CHECK(roi.h == 100);
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_ZONES);
	CHECK(read(r).state == U_REAR_BUDGET_OPEN);
}

TEST_CASE("comp_rear_budget: two 3D zones clamp to the box around both")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	// A zones app with a gap between its zones. The ROI is one rect, so what
	// is carried forward is the box around the surviving intersections - the
	// tightest rect that cannot exclude woven content.
	zones(r, {{0.0f, 0.0f, 0.2f, 1.0f}, {0.6f, 0.0f, 0.8f, 1.0f}}, 0);
	bounds(r, 0.1f, 0.2f, 0.7f, 0.8f, 0);
	step(r, &neutral.pv, 0);

	// Intersections: u 0.1..0.2 and u 0.6..0.7 -> box u 0.1..0.7 -> px 20..140,
	// dilated to 12..148, then clamped by the zone box (px 0..160).
	const u_bg_roi roi = roi_of(r);
	CHECK(roi.x == 12);
	CHECK(roi.w == 148 - 12);
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_BOUNDS_IN_ZONES);
}

TEST_CASE("comp_rear_budget: DXR_REAR_BUDGET_ROI=0 also disables the zone clamp")
{
	ScopedEnv off("DXR_REAR_BUDGET_ROI", "0");

	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	pv.paint_busy_patch(150, 0, 200, 100);

	// The kill switch is the A/B's other arm, and an arm that still clamps is
	// not the whole-preview arm it claims to be.
	zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, 0);
	bounds(r, 0.3f, 0.1f, 0.9f, 0.9f, 0);
	run_for(r, &pv.pv, 0, 800);

	bool narrowed = true;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.w == 200);
	CHECK(roi.h == 100);
	CHECK_FALSE(narrowed);
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_WHOLE_PREVIEW);
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
}


/*
 * -----------------------------------------------------------------------------
 * v3: the silhouette occupancy mask ROI (#1365)
 *
 * A rect around a character is roughly two thirds background the character
 * never covers, and any horizontal structure in that surplus closes a budget
 * the silhouette itself would have left open. The app already computes the
 * union-over-views silhouette every frame - it is its click-through window
 * region - so v3 measures under THAT.
 *
 * Shared geometry below: a 200x100 preview over the whole canvas, so the
 * runtime dilation is max(4% of 200, 8 px) = 8 px on every side, and a 20x10
 * mask grid whose cells are 10x10 preview pixels each.
 * -----------------------------------------------------------------------------
 */

namespace {

//! An XrContentMaskDXR-shaped grid, window-normalised like the bounds are.
struct MaskGrid
{
	uint32_t w, h;
	std::vector<uint8_t> cells;

	MaskGrid(uint32_t w_, uint32_t h_) : w(w_), h(h_) { cells.assign((size_t)w_ * h_, 0); }

	//! Half-open cell range, matching how the runtime reads the grid.
	void
	set(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
	{
		for (uint32_t y = y0; y < y1; y++) {
			for (uint32_t x = x0; x < x1; x++) {
				cells[(size_t)y * w + x] = 1;
			}
		}
	}
};

//! The app's content mask, as xrEndFrame would forward it.
void
mask(Runner &r, const MaskGrid &m, uint64_t now_ns, float margin = 0.0f)
{
	comp_rear_budget_set_content_mask(&r.b, m.cells.data(), m.w, m.h, m.w, margin, now_ns);
}

//! Nonzero preview pixels of the dilated mask the last analysis measured through.
uint32_t
mask_px(Runner &r)
{
	uint32_t px = 0;
	if (!comp_rear_budget_debug_last_mask(&r.b, nullptr, nullptr, nullptr, &px)) {
		return 0;
	}
	return px;
}

//! Whether preview pixel (x, y) is inside the dilated mask.
bool
mask_at(Runner &r, uint32_t x, uint32_t y)
{
	const uint8_t *m = nullptr;
	uint32_t w = 0, h = 0;
	if (!comp_rear_budget_debug_last_mask(&r.b, &m, &w, &h, nullptr)) {
		return false;
	}
	if (x >= w || y >= h) {
		return false;
	}
	return m[(size_t)y * w + x] != 0;
}

//! A dump sink that keeps the whole image, so the tint can be inspected.
struct TintSink
{
	int calls = 0;
	uint32_t w = 0, h = 0;
	std::vector<uint8_t> image;

	static void
	fn(void *ctx, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride)
	{
		auto *self = static_cast<TintSink *>(ctx);
		self->calls++;
		self->w = w;
		self->h = h;
		self->image.assign(bgra, bgra + (size_t)stride * h);
	}

	const uint8_t *
	at(uint32_t x, uint32_t y) const
	{
		return image.data() + ((size_t)y * w + x) * 4u;
	}
};

} // namespace

TEST_CASE("comp_rear_budget: one nonzero cell marks every preview pixel it overlaps")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	// Cell (5, 5) of a 20x10 grid covers u 0.25..0.30 and v 0.5..0.6 - preview
	// px 50..59 x 50..59 - and the dilation adds 8 on every side.
	MaskGrid m(20, 10);
	m.set(5, 5, 6, 6);
	mask(r, m, 0);
	step(r, &neutral.pv, 0);

	REQUIRE(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_MASK);
	const u_bg_roi roi = roi_of(r);
	CHECK(roi.x == 42);
	CHECK(roi.y == 42);
	CHECK(roi.w == 26); // 42..67
	CHECK(roi.h == 26);
	CHECK(mask_px(r) == 26u * 26u);

	// Any-coverage means the WHOLE cell is marked, not just the pixel under
	// its centre: under-reporting the silhouette is the one error that opens
	// the budget over something it never measured.
	CHECK(mask_at(r, 50, 50));
	CHECK(mask_at(r, 59, 59));
	CHECK(mask_at(r, 42, 42)); // the dilation band
	CHECK_FALSE(mask_at(r, 41, 50));
	CHECK_FALSE(mask_at(r, 68, 50));
}

TEST_CASE("comp_rear_budget: a mask cell finer than a preview pixel still marks one")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	// A 512x512 grid over a 200x100 preview: one cell is 0.39 x 0.20 px. A
	// pixel-first "which cell is under my centre" resample would drop it
	// entirely, and a silhouette that vanishes reads as no region at all.
	MaskGrid m(512, 512);
	m.set(256, 256, 257, 257);
	mask(r, m, 0);
	step(r, &neutral.pv, 0);

	REQUIRE(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_MASK);
	const u_bg_roi roi = roi_of(r);
	// One pixel at (100, 50), dilated by 8 on every side.
	CHECK(roi.x == 92);
	CHECK(roi.y == 42);
	CHECK(roi.w == 17);
	CHECK(roi.h == 17);
}

TEST_CASE("comp_rear_budget: the app's margin widens the dilation")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	MaskGrid m(20, 10);
	m.set(5, 5, 6, 6);
	// 0.1 of the window = 20 preview px, which is wider than the runtime's
	// own 8 - marginNormalized is ON TOP of the default, not instead of it,
	// so the wider of the two wins.
	mask(r, m, 0, /*margin=*/0.1f);
	step(r, &neutral.pv, 0);

	const u_bg_roi roi = roi_of(r);
	CHECK(roi.x == 30);          // 50 - 20
	CHECK(roi.x + roi.w == 80);  // 59 + 20, half-open
}

TEST_CASE("comp_rear_budget: the mask is clamped to the frame's 3D zones")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	// One 3D zone on the left half (px 0..100); the right half is a 2D band.
	zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, 0);
	// A silhouette spanning both: cells x 8..15 -> px 80..159, y 2..7 -> px 20..79.
	MaskGrid m(20, 10);
	m.set(8, 2, 16, 8);
	mask(r, m, 0);
	step(r, &neutral.pv, 0);

	REQUIRE(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_MASK);
	const u_bg_roi roi = roi_of(r);
	// Clamped to px 80..99, dilated to 72..107, and clamped AGAIN - the band
	// reaches into the 2D strip just as readily as the silhouette could.
	CHECK(roi.x == 72);
	CHECK(roi.x + roi.w == 100);
	CHECK_FALSE(mask_at(r, 100, 50));
	CHECK(mask_at(r, 99, 50));
}

TEST_CASE("comp_rear_budget: a mask entirely outside every 3D zone falls back to the bounds")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, 0);
	// Squarely inside the 2D band: cells x 12..15 -> px 120..159.
	MaskGrid m(20, 10);
	m.set(12, 2, 16, 8);
	mask(r, m, 0);
	bounds(r, 0.3f, 0.2f, 0.45f, 0.8f, 0);
	step(r, &neutral.pv, 0);

	// The mask names no measurable region, so the next authority down answers.
	// Never "neutral": an unmeasurable region must not open the budget.
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_BOUNDS_IN_ZONES);
	CHECK(mask_px(r) == 0);
}

TEST_CASE("comp_rear_budget: a mask too small to measure falls back rather than refusing")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	// A 3D zone of 4 x 5 preview px. Whatever the mask says, at most 20 px
	// survive the clamp - under the analysis's 64-sample floor. The analysis
	// would REFUSE that, and a refusal mid-tick freezes the previous verdict
	// instead of producing one, so the runner checks first.
	zones(r, {{0.0f, 0.0f, 0.02f, 0.05f}}, 0);
	MaskGrid m(20, 10);
	m.set(0, 0, 1, 1);
	mask(r, m, 0);
	step(r, &neutral.pv, 0);

	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_ZONES);
	CHECK(mask_px(r) == 0);
}

TEST_CASE("comp_rear_budget: an all-zero mask is absent, not empty")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	// "The app rendered nothing here" is absence. Measuring it would measure
	// nothing, and nothing measures as neutral - which would open the budget
	// over a desktop nobody looked at.
	MaskGrid empty(20, 10);
	mask(r, empty, 0);
	bounds(r, 0.4f, 0.4f, 0.6f, 0.6f, 0);
	step(r, &neutral.pv, 0);

	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_BOUNDS);
	CHECK(roi_of(r).w == 56); // the v2 answer, unchanged
}

TEST_CASE("comp_rear_budget: precedence is mask, then bounds, then zones, then the preview")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);

	zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, 0);
	bounds(r, 0.1f, 0.2f, 0.4f, 0.8f, 0);
	MaskGrid m(20, 10);
	m.set(2, 2, 6, 8);
	mask(r, m, 0);
	step(r, &neutral.pv, 0);
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_MASK);

	// The app stops chaining the mask but keeps chaining bounds and zones. A
	// silhouette from a second ago describes a pose that has since moved.
	for (uint64_t t = 10 * MS; t <= 1500 * MS; t += 10 * MS) {
		bounds(r, 0.1f, 0.2f, 0.4f, 0.8f, t);
		zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, t);
		step(r, &neutral.pv, t);
	}
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_BOUNDS_IN_ZONES);

	// Then it stops chaining bounds too, and only the frame's own zones are
	// left - a fact of the frame rather than a claim by the app.
	for (uint64_t t = 1510 * MS; t <= 3000 * MS; t += 10 * MS) {
		zones(r, {{0.0f, 0.0f, 0.5f, 1.0f}}, t);
		step(r, &neutral.pv, t);
	}
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_ZONES);

	// And with the zones gone as well there is nothing left but v1.
	for (uint64_t t = 3010 * MS; t <= 4600 * MS; t += 10 * MS) {
		step(r, &neutral.pv, t);
	}
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_WHOLE_PREVIEW);
	CHECK(roi_of(r).w == 200);
}

TEST_CASE("comp_rear_budget: a busy patch beside the silhouette keeps the budget open")
{
	// The v3 case end to end. A wide bounds rect around a character with text
	// beside it - an engine's animation-set AABB, or scenery unioned in -
	// measures the text and closes. The silhouette does not touch it.
	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	pv.paint_busy_patch(150, 0, 200, 100);

	// Bounds spanning the whole window, patch included.
	bounds(r, 0.05f, 0.1f, 0.95f, 0.9f, 0);
	// The character: a vertical bar at px 20..39, nowhere near the patch.
	MaskGrid m(20, 10);
	m.set(2, 1, 4, 9);
	mask(r, m, 0);
	run_for(r, &pv.pv, 0, 800);

	REQUIRE(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_MASK);
	const u_bg_roi roi = roi_of(r);
	CHECK(roi.x == 12);
	CHECK(roi.x + roi.w == 48);
	CHECK(mask_px(r) > 0);

	const u_rear_budget_out out = read(r);
	CHECK(out.state == U_REAR_BUDGET_OPEN);
	CHECK(out.far_offset_vh > U_REAR_BUDGET_UNRESTRICTED_VH - 0.5f);
}

TEST_CASE("comp_rear_budget: the same frame with the mask disabled closes")
{
	// The other arm of the A/B, and the reason DXR_REAR_BUDGET_MASK is
	// separate from DXR_REAR_BUDGET_ROI: this asks "is the silhouette better
	// than the box", not "is a region better than the canvas".
	ScopedEnv off("DXR_REAR_BUDGET_MASK", "0");

	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	pv.paint_busy_patch(150, 0, 200, 100);

	bounds(r, 0.05f, 0.1f, 0.95f, 0.9f, 0);
	MaskGrid m(20, 10);
	m.set(2, 1, 4, 9);
	mask(r, m, 0);
	run_for(r, &pv.pv, 0, 800);

	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_BOUNDS);
	CHECK(mask_px(r) == 0);
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
}

TEST_CASE("comp_rear_budget: DXR_REAR_BUDGET_ROI=0 disables the mask as well")
{
	ScopedEnv off("DXR_REAR_BUDGET_ROI", "0");

	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	pv.paint_busy_patch(150, 0, 200, 100);

	MaskGrid m(20, 10);
	m.set(2, 1, 4, 9);
	mask(r, m, 0);
	run_for(r, &pv.pv, 0, 800);

	bool narrowed = true;
	const u_bg_roi roi = roi_of(r, &narrowed);
	CHECK(roi.w == 200);
	CHECK_FALSE(narrowed);
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_WHOLE_PREVIEW);
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
}

TEST_CASE("comp_rear_budget: a silhouette that changes inside an unchanged rect is re-measured")
{
	Runner r;
	FakePreview pv(200, 100, /*generation=*/1, /*busy=*/false);
	pv.paint_busy_patch(100, 0, 140, 100);

	// Two cells at opposite ends fix the bounding rect; the desktop never
	// changes, so the generation never advances either. Neither the rect nor
	// the capture can tell the runner to look again - only the mask can.
	MaskGrid clear_of_it(20, 10);
	clear_of_it.set(2, 1, 4, 9);
	clear_of_it.set(19, 1, 20, 9);
	mask(r, clear_of_it, 0);
	uint64_t t = run_for(r, &pv.pv, 0, 800);
	REQUIRE(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_MASK);
	const u_bg_roi before = roi_of(r);
	REQUIRE(read(r).state == U_REAR_BUDGET_OPEN);

	// An arm comes down over the text. Same bounding rect, different pixels.
	MaskGrid over_it(20, 10);
	over_it.set(2, 1, 4, 9);
	over_it.set(19, 1, 20, 9);
	over_it.set(11, 3, 13, 7);
	for (uint64_t u = t + 10 * MS; u <= t + 500 * MS; u += 10 * MS) {
		mask(r, over_it, u);
		step(r, &pv.pv, u);
	}

	const u_bg_roi after = roi_of(r);
	CHECK(after.x == before.x);
	CHECK(after.w == before.w); // the rect really did not move
	CHECK(read(r).state == U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND);
}

TEST_CASE("comp_rear_budget: an oversized or malformed mask is refused, not truncated")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);
	bounds(r, 0.4f, 0.4f, 0.6f, 0.6f, 0);

	std::vector<uint8_t> cells((size_t)600 * 4, 1);

	SECTION("wider than the 512-cell limit")
	{
		comp_rear_budget_set_content_mask(&r.b, cells.data(), 600, 4, 600, 0.0f, 0);
	}
	SECTION("stride below the width")
	{
		comp_rear_budget_set_content_mask(&r.b, cells.data(), 64, 4, 32, 0.0f, 0);
	}
	SECTION("null cells")
	{
		comp_rear_budget_set_content_mask(&r.b, nullptr, 64, 4, 64, 0.0f, 0);
	}
	SECTION("zero dims")
	{
		comp_rear_budget_set_content_mask(&r.b, cells.data(), 0, 0, 0, 0.0f, 0);
	}

	step(r, &neutral.pv, 0);

	// Truncating to the first 512 columns would measure a region the app never
	// described. The bounds are still fresh, so they answer.
	CHECK(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_BOUNDS);
	CHECK(mask_px(r) == 0);
}

TEST_CASE("comp_rear_budget: the dump tints the mask the analysis judged")
{
	Runner r;
	TintSink sink;
	comp_rear_budget_debug_set_dump_sink(&r.b, TintSink::fn, &sink);

	FakePreview neutral(200, 100, /*generation=*/1, /*busy=*/false); // fills 128
	MaskGrid m(20, 10);
	m.set(5, 5, 6, 6); // px 50..59, dilated to 42..67
	mask(r, m, 0);
	run_for(r, &neutral.pv, 0, 800);
	REQUIRE(read(r).state == U_REAR_BUDGET_OPEN);
	REQUIRE(sink.calls > 0);
	REQUIRE(sink.w == 200);

	// Inside the dilated mask: 50% toward green. Without this the PNG answers
	// "what did the desktop look like" but not "what did the runtime measure",
	// and for a mask those are completely different pictures - a bar and the
	// box around it share a bounding rect and share nothing else.
	const uint8_t *judged = sink.at(50, 50);
	CHECK(judged[0] == 64);                    // B halved
	CHECK(judged[1] == (128 + 255) / 2);       // G toward 255
	CHECK(judged[2] == 64);                    // R halved

	// Outside it, the preview is untouched - the tint is the region, not a
	// filter over the whole picture.
	const uint8_t *untouched = sink.at(10, 10);
	CHECK(untouched[0] == 128);
	CHECK(untouched[1] == 128);
	CHECK(untouched[2] == 128);
}

TEST_CASE("comp_rear_budget: the mask maps through a preview that carries a margin")
{
	Runner r;
	FakePreview neutral(200, 100, 1, /*busy=*/false);
	// A display processor capturing 25% beyond the window on every side: the
	// preview spans window -0.25..1.25. Reading the grid as if the preview
	// were the window would aim the silhouette a third of a screen to the left.
	neutral.pv.canvas_u0 = -0.25f;
	neutral.pv.canvas_v0 = -0.25f;
	neutral.pv.canvas_u1 = 1.25f;
	neutral.pv.canvas_v1 = 1.25f;

	// Cell (5,5) of 20x10 = window u 0.25..0.30 -> (0.25+0.25)/1.5*200 = 66.67
	// .. (0.30+0.25)/1.5*200 = 73.33, so px 66..73; dilated by 8 -> 58..81.
	MaskGrid m(20, 10);
	m.set(5, 5, 6, 6);
	mask(r, m, 0);
	step(r, &neutral.pv, 0);

	REQUIRE(src_of(r) == COMP_REAR_BUDGET_ROI_SRC_MASK);
	const u_bg_roi roi = roi_of(r);
	CHECK(roi.x == 58);
	CHECK(roi.x + roi.w == 82);
}
