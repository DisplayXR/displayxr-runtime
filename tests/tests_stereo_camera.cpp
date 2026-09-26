// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) R1: the platform-neutral pieces the
 *         service's camera manager is built from, pinned host-side.
 *
 *  - the per-stream latest-wins ring: a pinned slot is never written, the
 *    writer always finds a free slot, NOT READY until something newer lands,
 *    skipped frames are counted, suspension clears the pin;
 *  - the decimator: exact ratios (30 -> 15, 30 -> 20), no burst after a pause;
 *  - plane layout + format conversion round trips;
 *  - the persistent id: stable, per consumer, never the raw identity;
 *  - the sim_display FAKE scene: the disparity probe measures exactly the
 *    disparities the scene was built with, and the frame counter differs
 *    between frames;
 *  - the plug-in iface: the camera slots sit right after the ADR-042 lift
 *    slot (or its placeholder), so the two branches cannot share an offset.
 */

#include "catch_amalgamated.hpp"

#include "util/u_stereo_camera.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_stereo_camera.h"

#include "sim_display_stereo_camera_pattern.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <set>
#include <vector>

TEST_CASE("stereo camera ring: latest-wins with one pin", "[stereo_camera]")
{
	u_stereo_camera_ring r;
	u_stereo_camera_ring_init(&r);

	int32_t slot = -1;
	uint64_t seq = 0;
	CHECK_FALSE(u_stereo_camera_ring_acquire(&r, &slot, &seq)); // nothing yet

	int32_t w1 = u_stereo_camera_ring_begin_write(&r);
	REQUIRE(w1 >= 0);
	u_stereo_camera_ring_publish(&r, w1, 1);
	REQUIRE(u_stereo_camera_ring_acquire(&r, &slot, &seq));
	CHECK(slot == w1);
	CHECK(seq == 1);
	CHECK_FALSE(u_stereo_camera_ring_acquire(&r, &slot, &seq)); // not newer

	// Many publishes while the consumer holds its pin: the writer must never
	// be handed the pinned slot, and every unacquired frame counts as skipped.
	for (uint64_t s = 2; s < 50; s++) {
		int32_t w = u_stereo_camera_ring_begin_write(&r);
		REQUIRE(w >= 0);
		REQUIRE(w != r.pinned);
		REQUIRE(w != r.latest);
		u_stereo_camera_ring_publish(&r, w, s);
	}
	CHECK(r.skipped == 47); // 2..48 superseded, 49 is the latest
	REQUIRE(u_stereo_camera_ring_acquire(&r, &slot, &seq));
	CHECK(seq == 49);
	CHECK(r.acquired == 2);

	// Suspension: pin dropped, latest forgotten, next publish is not a skip.
	u_stereo_camera_ring_clear(&r);
	CHECK(r.pinned == -1);
	CHECK_FALSE(u_stereo_camera_ring_acquire(&r, &slot, &seq));
	int32_t w = u_stereo_camera_ring_begin_write(&r);
	u_stereo_camera_ring_publish(&r, w, 50);
	CHECK(r.skipped == 47);
	REQUIRE(u_stereo_camera_ring_acquire(&r, &slot, &seq));
	CHECK(seq == 50);
}

TEST_CASE("stereo camera ring: a pinned slot stays byte-stable", "[stereo_camera]")
{
	// Simulate the payload: each slot holds the sequence last written into it.
	u_stereo_camera_ring r;
	u_stereo_camera_ring_init(&r);
	uint64_t payload[U_STEREO_CAMERA_RING_SLOTS] = {0, 0, 0};
	int32_t slot = -1;
	uint64_t seq = 0, held = 0;
	int32_t held_slot = -1;
	for (uint64_t s = 1; s <= 1000; s++) {
		int32_t w = u_stereo_camera_ring_begin_write(&r);
		payload[w] = s;
		u_stereo_camera_ring_publish(&r, w, s);
		if (held_slot >= 0) {
			REQUIRE(payload[held_slot] == held); // never overwritten under the pin
		}
		if (s % 7 == 0 && u_stereo_camera_ring_acquire(&r, &slot, &seq)) {
			REQUIRE(payload[slot] == seq);
			held = seq;
			held_slot = slot;
		}
	}
}

TEST_CASE("stereo camera decimator", "[stereo_camera]")
{
	const int64_t p30 = 33333333;
	auto count = [&](float max_rate, int frames, int64_t pause_at = -1) {
		u_stereo_camera_decimator d;
		u_stereo_camera_decimator_init(&d, max_rate, 30.0f);
		int n = 0;
		int64_t t = 1000000000;
		for (int i = 0; i < frames; i++) {
			if (i == pause_at) {
				t += 2000000000; // a 2 s gap (suspended source)
			}
			n += u_stereo_camera_decimator_accept(&d, t) ? 1 : 0;
			t += p30;
		}
		return n;
	};
	CHECK(count(0.0f, 300) == 300);
	CHECK(count(30.0f, 300) == 300);
	CHECK(count(60.0f, 300) == 300); // never interpolates up
	CHECK(count(15.0f, 300) == 150);
	CHECK(count(20.0f, 300) == 200);
	CHECK(count(10.0f, 300) == 100);
	// After a pause the gate re-anchors: no burst of "missed" frames.
	int with_pause = count(15.0f, 300, 150);
	CHECK(with_pause >= 150);
	CHECK(with_pause <= 151);
}

TEST_CASE("stereo camera layout", "[stereo_camera]")
{
	u_stereo_camera_planes l;
	REQUIRE(u_stereo_camera_layout(1, 1280, 480, &l));
	CHECK(l.plane_count == 1);
	CHECK(l.pitch[0] == 1280);
	CHECK(l.size == 1280ull * 480);
	REQUIRE(u_stereo_camera_layout(2, 1280, 480, &l));
	CHECK(l.plane_count == 2);
	CHECK(l.offset[1] == 1280ull * 480);
	CHECK(l.size == 1280ull * 480 * 3 / 2);
	REQUIRE(u_stereo_camera_layout(3, 1000, 10, &l));
	CHECK(l.pitch[0] == 4032);                             // 4000 rounded up to 64
	CHECK_FALSE(u_stereo_camera_layout(2, 1281, 480, &l)); // odd NV12
	CHECK_FALSE(u_stereo_camera_layout(9, 64, 64, &l));
}

TEST_CASE("stereo camera format conversion", "[stereo_camera]")
{
	const uint32_t W = 64, H = 8;
	std::vector<uint8_t> gray(W * H);
	for (uint32_t i = 0; i < W * H; i++) {
		gray[i] = (uint8_t)(i * 7);
	}
	const uint8_t *src[2] = {gray.data(), nullptr};
	const uint32_t sp[2] = {W, 0};

	SECTION("gray -> nv12 keeps luma, neutral chroma")
	{
		u_stereo_camera_planes l;
		REQUIRE(u_stereo_camera_layout(2, W, H, &l));
		std::vector<uint8_t> out(l.size);
		REQUIRE(u_stereo_camera_convert(1, src, sp, 2, out.data(), &l, W, H));
		CHECK(std::memcmp(out.data(), gray.data(), W) == 0);
		for (uint64_t i = l.offset[1]; i < l.size; i++) {
			REQUIRE(out[i] == 128);
		}
		// and back to gray
		u_stereo_camera_planes g;
		REQUIRE(u_stereo_camera_layout(1, W, H, &g));
		std::vector<uint8_t> back(g.size);
		const uint8_t *nsrc[2] = {out.data(), out.data() + l.offset[1]};
		REQUIRE(u_stereo_camera_convert(2, nsrc, l.pitch, 1, back.data(), &g, W, H));
		CHECK(std::memcmp(back.data(), gray.data(), W * H) == 0);
	}
	SECTION("gray -> bgra -> gray is lossless")
	{
		u_stereo_camera_planes b;
		REQUIRE(u_stereo_camera_layout(3, W, H, &b));
		std::vector<uint8_t> bgra(b.size);
		REQUIRE(u_stereo_camera_convert(1, src, sp, 3, bgra.data(), &b, W, H));
		CHECK(bgra[3] == 255);
		u_stereo_camera_planes g;
		REQUIRE(u_stereo_camera_layout(1, W, H, &g));
		std::vector<uint8_t> back(g.size);
		const uint8_t *bsrc[2] = {bgra.data(), nullptr};
		REQUIRE(u_stereo_camera_convert(3, bsrc, b.pitch, 1, back.data(), &g, W, H));
		CHECK(std::memcmp(back.data(), gray.data(), W * H) == 0);
	}
	SECTION("bgra -> nv12 -> bgra stays within video-range rounding")
	{
		u_stereo_camera_planes b;
		REQUIRE(u_stereo_camera_layout(3, W, H, &b));
		std::vector<uint8_t> bgra(b.size, 0);
		for (uint32_t y = 0; y < H; y++) {
			for (uint32_t x = 0; x < W; x++) {
				uint8_t *p = &bgra[y * b.pitch[0] + 4 * x];
				p[0] = 60; // uniform colour: chroma subsampling is exact
				p[1] = 120;
				p[2] = 200;
				p[3] = 255;
			}
		}
		u_stereo_camera_planes n;
		REQUIRE(u_stereo_camera_layout(2, W, H, &n));
		std::vector<uint8_t> nv(n.size);
		const uint8_t *bs[2] = {bgra.data(), nullptr};
		REQUIRE(u_stereo_camera_convert(3, bs, b.pitch, 2, nv.data(), &n, W, H));
		std::vector<uint8_t> rt(b.size);
		const uint8_t *ns[2] = {nv.data(), nv.data() + n.offset[1]};
		REQUIRE(u_stereo_camera_convert(2, ns, n.pitch, 3, rt.data(), &b, W, H));
		for (int c = 0; c < 3; c++) {
			int d = (int)rt[c] - (int)bgra[c];
			CHECK(d >= -3);
			CHECK(d <= 3);
		}
	}
	CHECK_FALSE(u_stereo_camera_convert(1, src, sp, 7, gray.data(), nullptr, W, H));
}

TEST_CASE("stereo camera persistent id", "[stereo_camera]")
{
	char a[64], b[64], c[64];
	u_stereo_camera_persistent_id("SERIAL-123", "C:/apps/chrome.exe", a);
	u_stereo_camera_persistent_id("SERIAL-123", "C:/apps/chrome.exe", b);
	u_stereo_camera_persistent_id("SERIAL-123", "C:/apps/other.exe", c);
	CHECK(std::strcmp(a, b) == 0);
	CHECK(std::strcmp(a, c) != 0);
	CHECK(std::strncmp(a, "dxrcam-", 7) == 0);
	CHECK(std::strlen(a) == 7 + 32);
	CHECK(std::strstr(a, "SERIAL") == nullptr);
}

TEST_CASE("sim_display fake scene has the disparities it was built with", "[stereo_camera]")
{
	sim_stereo_camera_scene s;
	// 640 px per eye, 68 deg HFOV, 50 mm, 2.0 m / 0.6 m — the fake's defaults.
	const double fx = 320.0 / std::tan(34.0 * 3.14159265358979323846 / 180.0);
	sim_stereo_camera_scene_init(&s, 640, 480, fx, 50.0, 2.0, 0.6);
	CHECK(s.bg_disparity == 12);
	CHECK(s.bar_disparity == 40);

	const uint32_t pitch = 1280;
	std::vector<uint8_t> f1(pitch * 480), f2(pitch * 480);
	sim_stereo_camera_render_gray(&s, 1, f1.data(), pitch);
	sim_stereo_camera_render_gray(&s, 2, f2.data(), pitch);
	CHECK(std::memcmp(f1.data(), f2.data(), f1.size()) != 0); // the counter moved

	float d = 0.0f;
	// Inside the bar (left-eye coordinates).
	REQUIRE(u_stereo_camera_estimate_disparity(f1.data(), pitch, 640, 480, 272, 200, 64, 64, 64, &d));
	CHECK(d == Catch::Approx(40.0f).margin(0.5f));
	// Background, right of the bar, below the counter.
	REQUIRE(u_stereo_camera_estimate_disparity(f1.data(), pitch, 640, 480, 480, 300, 64, 64, 64, &d));
	CHECK(d == Catch::Approx(12.0f).margin(0.5f));
	// Out-of-range region is refused.
	CHECK_FALSE(u_stereo_camera_estimate_disparity(f1.data(), pitch, 640, 480, 600, 0, 64, 64, 64, &d));
}

TEST_CASE("plug-in iface: camera slots follow the ADR-042 lift slot", "[stereo_camera]")
{
#ifdef XRT_PLUGIN_IFACE_HAS_D3D11_LIFT_FACTORY
	const size_t lift_end =
	    offsetof(xrt_plugin_iface, create_dp_d3d11_lift) + sizeof(((xrt_plugin_iface *)0)->create_dp_d3d11_lift);
#else
	const size_t lift_end = offsetof(xrt_plugin_iface, reserved_adr042_create_dp_d3d11_lift) +
	                        sizeof(((xrt_plugin_iface *)0)->reserved_adr042_create_dp_d3d11_lift);
#endif
	// The lift slot (or its placeholder) is the one right after the #1243 vk
	// fingerprint, and the camera block starts right after it.
	CHECK(offsetof(xrt_plugin_iface, stereo_camera_enumerate) == lift_end);
	CHECK(offsetof(xrt_plugin_iface, stereo_camera_close) + sizeof(void *) == sizeof(xrt_plugin_iface));

	xrt_plugin_iface iface{};
	iface.struct_size = (uint32_t)offsetof(xrt_plugin_iface, stereo_camera_enumerate);
	CHECK_FALSE(xrt_plugin_iface_has_stereo_camera(&iface)); // an older plug-in
	CHECK_FALSE(xrt_plugin_iface_has_stereo_camera(nullptr));
}

TEST_CASE("stereo camera value types agree with the plug-in encodings", "[stereo_camera]")
{
	STATIC_REQUIRE((int)XRT_STEREO_CAMERA_FORMAT_GRAY8 == (int)XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8);
	STATIC_REQUIRE((int)XRT_STEREO_CAMERA_FORMAT_NV12 == (int)XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12);
	STATIC_REQUIRE((int)XRT_STEREO_CAMERA_FORMAT_BGRA8 == (int)XRT_PLUGIN_STEREO_CAMERA_FORMAT_BGRA8);
	STATIC_REQUIRE(XRT_STEREO_CAMERA_RING_SLOTS == U_STEREO_CAMERA_RING_SLOTS);
}
