// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for u_bg_neutrality — the horizontal-cue metric behind the
 *         rear depth budget (XR_DXR_depth_budget).
 *
 * The whole point of the metric is that it is BLIND to vertical structure: a
 * vertical gradient or horizontal stripes carry no horizontal disparity cue,
 * so rear content over them is fine. These tests pin that asymmetry, because
 * "measures edges" is exactly the description a symmetric gradient magnitude
 * would also satisfy — and that one would be wrong.
 */

#include "util/u_bg_neutrality.h"

#include "catch_amalgamated.hpp"

#include <cstdint>
#include <vector>

namespace {

struct Img
{
	uint32_t w = 0, h = 0, stride = 0;
	std::vector<uint8_t> px;

	Img(uint32_t w_, uint32_t h_, uint32_t pad_bytes = 0) : w(w_), h(h_), stride(w_ * 4 + pad_bytes)
	{
		px.assign((size_t)stride * h, 0);
	}

	void
	set(uint32_t x, uint32_t y, uint8_t b, uint8_t g, uint8_t r)
	{
		uint8_t *p = px.data() + (size_t)y * stride + (size_t)x * 4;
		p[0] = b;
		p[1] = g;
		p[2] = r;
		p[3] = 255;
	}

	void
	fill(uint8_t b, uint8_t g, uint8_t r)
	{
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x++) {
				set(x, y, b, g, r);
			}
		}
	}

	const uint8_t *
	data() const
	{
		return px.data();
	}
};

u_bg_neutrality_result
analyse(const Img &img, const u_bg_roi *roi = nullptr)
{
	u_bg_neutrality_result out{};
	REQUIRE(u_bg_neutrality_analyse(img.data(), img.w, img.h, img.stride, roi, nullptr, &out));
	return out;
}

} // namespace

TEST_CASE("bg_neutrality: solid colour is neutral")
{
	Img img(64, 32);
	img.fill(90, 140, 200);

	auto r = analyse(img);
	CHECK(r.neutral);
	CHECK(r.edge_fraction == 0.0f);
	CHECK(r.max_column_density == 0.0f);
	CHECK(r.cue_energy == 0.0f);
}

TEST_CASE("bg_neutrality: vertical gradient (rows constant) is neutral")
{
	// Luma varies strongly DOWN the image and not at all ACROSS it. A
	// symmetric gradient metric would call this busy; the eye does not.
	Img img(64, 64);
	for (uint32_t y = 0; y < img.h; y++) {
		const uint8_t v = (uint8_t)(y * 4);
		for (uint32_t x = 0; x < img.w; x++) {
			img.set(x, y, v, v, v);
		}
	}

	auto r = analyse(img);
	CHECK(r.neutral);
	CHECK(r.edge_fraction == 0.0f);
}

TEST_CASE("bg_neutrality: horizontal stripes are neutral")
{
	// Maximal vertical contrast, zero horizontal contrast.
	Img img(64, 64);
	for (uint32_t y = 0; y < img.h; y++) {
		const uint8_t v = (y % 2) ? 255 : 0;
		for (uint32_t x = 0; x < img.w; x++) {
			img.set(x, y, v, v, v);
		}
	}

	auto r = analyse(img);
	CHECK(r.neutral);
	CHECK(r.max_column_density == 0.0f);
}

TEST_CASE("bg_neutrality: smooth horizontal gradient below the step threshold is neutral")
{
	// 256 px wide, 1 level per column → ΔY ≈ 0.004 per step, far under the
	// 0.06 default. A gentle wallpaper wash must not shut the budget.
	Img img(256, 32);
	for (uint32_t y = 0; y < img.h; y++) {
		for (uint32_t x = 0; x < img.w; x++) {
			const uint8_t v = (uint8_t)x;
			img.set(x, y, v, v, v);
		}
	}

	auto r = analyse(img);
	CHECK(r.neutral);
	CHECK(r.edge_fraction == 0.0f);
}

TEST_CASE("bg_neutrality: one 1-px vertical line spanning the ROI is NOT neutral")
{
	// The case an area-only metric misses: two edge columns out of 511 is a
	// tiny edge FRACTION, but every row of those columns is an edge, so the
	// column-density term is what has to catch it.
	Img img(512, 64);
	img.fill(20, 20, 20);
	for (uint32_t y = 0; y < img.h; y++) {
		img.set(300, y, 240, 240, 240);
	}

	auto r = analyse(img);
	CHECK_FALSE(r.neutral);
	CHECK(r.max_column_density == 1.0f);
	CHECK(r.cue_energy == 1.0f);
}

TEST_CASE("bg_neutrality: a text-like checkerboard patch is NOT neutral")
{
	// ~5% of the ROI covered in 1-px checker — the edge-FRACTION term.
	Img img(200, 100);
	img.fill(30, 30, 30);
	const uint32_t pw = 45, ph = 22; // 990 px ≈ 4.95% of 20000
	for (uint32_t y = 0; y < ph; y++) {
		for (uint32_t x = 0; x < pw; x++) {
			const uint8_t v = ((x + y) % 2) ? 255 : 0;
			img.set(10 + x, 10 + y, v, v, v);
		}
	}

	auto r = analyse(img);
	CHECK_FALSE(r.neutral);
	CHECK(r.edge_fraction > 0.003f);
	CHECK(r.cue_energy == 1.0f);
}

TEST_CASE("bg_neutrality: the ROI actually selects a sub-rect")
{
	// Busy on the left, flat on the right. Same buffer, opposite verdicts —
	// which is the only way to prove the ROI is honoured rather than ignored.
	Img img(256, 64);
	img.fill(40, 40, 40);
	for (uint32_t y = 0; y < img.h; y++) {
		for (uint32_t x = 0; x < 100; x++) {
			const uint8_t v = ((x + y) % 2) ? 255 : 0;
			img.set(x, y, v, v, v);
		}
	}

	u_bg_roi busy = {0, 0, 100, 64};
	u_bg_roi calm = {128, 0, 128, 64};

	CHECK_FALSE(analyse(img, &busy).neutral);
	CHECK(analyse(img, &calm).neutral);
}

TEST_CASE("bg_neutrality: stride larger than width*4 is honoured")
{
	// 64 bytes of padding per row. If the row walk used w*4 it would slide
	// diagonally through the image and manufacture edges out of nothing.
	Img padded(64, 32, 64);
	padded.fill(90, 140, 200);
	// Poison the padding so a wrong stride cannot accidentally pass.
	for (uint32_t y = 0; y < padded.h; y++) {
		for (uint32_t b = padded.w * 4; b < padded.stride; b++) {
			padded.px[(size_t)y * padded.stride + b] = (uint8_t)(b * 7);
		}
	}

	auto r = analyse(padded);
	CHECK(r.neutral);
	CHECK(r.edge_fraction == 0.0f);
}

TEST_CASE("bg_neutrality: unusable input returns false, never 'neutral'")
{
	Img img(64, 32);
	img.fill(0, 0, 0);
	u_bg_neutrality_result out{};

	SECTION("null buffer")
	{
		CHECK_FALSE(u_bg_neutrality_analyse(nullptr, 64, 32, 256, nullptr, nullptr, &out));
	}
	SECTION("zero-area ROI")
	{
		u_bg_roi roi = {0, 0, 0, 0};
		CHECK_FALSE(u_bg_neutrality_analyse(img.data(), img.w, img.h, img.stride, &roi, nullptr, &out));
	}
	SECTION("single-column ROI holds no horizontal difference")
	{
		u_bg_roi roi = {5, 5, 1, 10};
		CHECK_FALSE(u_bg_neutrality_analyse(img.data(), img.w, img.h, img.stride, &roi, nullptr, &out));
	}
	SECTION("ROI leaves the buffer")
	{
		u_bg_roi roi = {40, 0, 40, 10};
		CHECK_FALSE(u_bg_neutrality_analyse(img.data(), img.w, img.h, img.stride, &roi, nullptr, &out));
	}
	SECTION("stride below the row size")
	{
		CHECK_FALSE(u_bg_neutrality_analyse(img.data(), img.w, img.h, img.w * 4 - 1, nullptr, nullptr, &out));
	}

	// Whatever the reason, the failure is not a quiet "neutral".
	CHECK_FALSE(out.neutral);
}

TEST_CASE("bg_neutrality: custom thresholds are respected")
{
	Img img(256, 64);
	img.fill(40, 40, 40);
	for (uint32_t y = 0; y < img.h; y++) {
		img.set(100, y, 60, 60, 60); // ΔY ≈ 0.078 — over the default 0.06
	}

	u_bg_neutrality_params p;
	u_bg_neutrality_params_default(&p);
	u_bg_neutrality_result strict{}, loose{};

	REQUIRE(u_bg_neutrality_analyse(img.data(), img.w, img.h, img.stride, nullptr, &p, &strict));
	CHECK_FALSE(strict.neutral);

	p.edge_threshold = 0.5f; // only a hard edge counts now
	REQUIRE(u_bg_neutrality_analyse(img.data(), img.w, img.h, img.stride, nullptr, &p, &loose));
	CHECK(loose.neutral);
}
