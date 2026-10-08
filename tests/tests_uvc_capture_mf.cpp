// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-043 Amendment 4, Windows: the Media Foundation UVC backend's
 *         SCALED output is a plain per-axis resize — never letterboxed.
 *
 * The first real-webcam run of the GPU path asked the video processor for
 * 2560x720 from a 16:9 3840x2160 half-SBS frame and got the picture
 * PILLARBOXED (the processor preserves display aspect): a centred ~1280 px
 * image between black bars, so each "eye" was black + half the picture.
 *
 * This writes a small uncompressed YUY2 AVI (a uniformly bright texture,
 * 16:9) and opens it through the real backend (`file:` id) asking for a 32:9
 * output — the same shape change — on BOTH decode paths (the hardware policy:
 * a D3D11 device manager + GPU video processor when an adapter qualifies; the
 * software policy: MF's CPU video processor). It asserts the frame arrives at
 * the requested size and that its outer 5 % columns are as bright as the
 * centre. No camera. SKIPs where Media Foundation cannot read the file.
 */

#include "catch_amalgamated.hpp"

#include "os/os_uvc_capture.h"
#include "util/u_stereo_uvc.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <windows.h>

namespace {

void
put32(std::vector<uint8_t> &b, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		b.push_back((uint8_t)(v >> (8 * i)));
	}
}

void
put16(std::vector<uint8_t> &b, uint16_t v)
{
	b.push_back((uint8_t)v);
	b.push_back((uint8_t)(v >> 8));
}

void
fourcc(std::vector<uint8_t> &b, const char *c)
{
	b.insert(b.end(), c, c + 4);
}

void
patch32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		b[at + i] = (uint8_t)(v >> (8 * i));
	}
}

//! A minimal uncompressed YUY2 AVI: @p frames frames of a bright texture.
std::vector<uint8_t>
make_yuy2_avi(uint32_t w, uint32_t h, uint32_t frames, uint32_t fps)
{
	const uint32_t frame_bytes = w * h * 2;
	std::vector<uint8_t> f;
	fourcc(f, "RIFF");
	size_t riff_size = f.size();
	put32(f, 0);
	fourcc(f, "AVI ");

	fourcc(f, "LIST");
	size_t hdrl_size = f.size();
	put32(f, 0);
	fourcc(f, "hdrl");
	fourcc(f, "avih");
	put32(f, 56);
	put32(f, 1000000 / fps);     // dwMicroSecPerFrame
	put32(f, frame_bytes * fps); // dwMaxBytesPerSec
	put32(f, 0);                 // dwPaddingGranularity
	put32(f, 0x10);              // AVIF_HASINDEX
	put32(f, frames);            // dwTotalFrames
	put32(f, 0);                 // dwInitialFrames
	put32(f, 1);                 // dwStreams
	put32(f, frame_bytes);       // dwSuggestedBufferSize
	put32(f, w);
	put32(f, h);
	for (int i = 0; i < 4; i++) {
		put32(f, 0);
	}
	fourcc(f, "LIST");
	size_t strl_size = f.size();
	put32(f, 0);
	fourcc(f, "strl");
	fourcc(f, "strh");
	put32(f, 56);
	fourcc(f, "vids");
	fourcc(f, "YUY2");
	put32(f, 0);           // dwFlags
	put16(f, 0);           // wPriority
	put16(f, 0);           // wLanguage
	put32(f, 0);           // dwInitialFrames
	put32(f, 1);           // dwScale
	put32(f, fps);         // dwRate
	put32(f, 0);           // dwStart
	put32(f, frames);      // dwLength
	put32(f, frame_bytes); // dwSuggestedBufferSize
	put32(f, 0xffffffffu); // dwQuality
	put32(f, 0);           // dwSampleSize
	put16(f, 0);
	put16(f, 0);
	put16(f, (uint16_t)w);
	put16(f, (uint16_t)h);
	fourcc(f, "strf");
	put32(f, 40);
	put32(f, 40); // biSize
	put32(f, w);
	put32(f, h);
	put16(f, 1);  // biPlanes
	put16(f, 16); // biBitCount
	fourcc(f, "YUY2");
	put32(f, frame_bytes);
	put32(f, 0);
	put32(f, 0);
	put32(f, 0);
	put32(f, 0);
	patch32(f, strl_size, (uint32_t)(f.size() - strl_size - 4));
	patch32(f, hdrl_size, (uint32_t)(f.size() - hdrl_size - 4));

	fourcc(f, "LIST");
	size_t movi_size = f.size();
	put32(f, 0);
	size_t movi_fcc = f.size();
	fourcc(f, "movi");
	std::vector<uint32_t> offsets;
	for (uint32_t n = 0; n < frames; n++) {
		offsets.push_back((uint32_t)(f.size() - movi_fcc));
		fourcc(f, "00db");
		put32(f, frame_bytes);
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x += 2) {
				// Bright everywhere (luma 110..210): any black bar is unmistakable.
				auto lum = [&](uint32_t px) {
					return (uint8_t)(160 + 50 * std::sin(px * 0.21 + n) * std::cos(y * 0.17));
				};
				f.push_back(lum(x));
				f.push_back(128);
				f.push_back(lum(x + 1));
				f.push_back(128);
			}
		}
	}
	patch32(f, movi_size, (uint32_t)(f.size() - movi_size - 4));
	fourcc(f, "idx1");
	put32(f, frames * 16);
	for (uint32_t n = 0; n < frames; n++) {
		fourcc(f, "00db");
		put32(f, 0x10); // AVIIF_KEYFRAME
		put32(f, offsets[n]);
		put32(f, frame_bytes);
	}
	patch32(f, riff_size, (uint32_t)(f.size() - 8));
	return f;
}

double
mean_luma(const u_stereo_uvc_raw_frame &r, uint32_t x0, uint32_t x1)
{
	double s = 0.0;
	uint64_t n = 0;
	const uint32_t step = r.pixel == U_STEREO_UVC_PIXEL_YUY2 ? 2 : 1;
	for (uint32_t y = 0; y < r.height; y++) {
		const uint8_t *row = r.planes[0] + (size_t)y * r.pitches[0];
		for (uint32_t x = x0; x < x1; x++) {
			s += row[(size_t)x * step];
			n++;
		}
	}
	return n > 0 ? s / (double)n : 0.0;
}

} // namespace

TEST_CASE("uvc capture (MF): a scaled output is a plain resize, never letterboxed", "[uvc_capture_mf]")
{
	char tmp[MAX_PATH];
	GetTempPathA(sizeof(tmp), tmp);
	std::string path = std::string(tmp) + "dxr_tests_uvc_aspect.avi";
	std::vector<uint8_t> avi = make_yuy2_avi(1280, 720, 30, 30); // 16:9
	FILE *fp = std::fopen(path.c_str(), "wb");
	REQUIRE(fp != nullptr);
	std::fwrite(avi.data(), 1, avi.size(), fp);
	std::fclose(fp);

	u_stereo_uvc_backend b;
	REQUIRE(os_uvc_capture_backend(&b));
	const std::string id = "file:" + path;
	u_stereo_uvc_mode modes[8];
	uint32_t nm = b.list_modes(b.ctx, id.c_str(), modes, 8);
	if (nm == 0) {
		std::remove(path.c_str());
		SKIP("Media Foundation cannot read the test AVI here");
	}
	REQUIRE(modes[0].width == 1280);
	REQUIRE(modes[0].height == 720);

	for (const char *policy : {"hardware", "software"}) {
		_putenv_s("DXR_STEREO_CAMERA_UVC_DECODER", policy);
		void *h = nullptr;
		// 1280x360 from 1280x720 = 32:9 from 16:9, the 3840x2160 -> 2560x720 shape.
		if (!b.open(b.ctx, id.c_str(), &modes[0], 1280, 360, &h)) {
			std::printf("[uvc aspect] %s policy: no such path on this machine — skipped\n", policy);
			continue;
		}
		u_stereo_uvc_raw_frame r;
		bool got = false;
		uint32_t timeouts = 0, errors = 0;
		for (int i = 0; i < 40 && !got; i++) {
			uint32_t rr = b.read(h, 500ll * 1000 * 1000, &r);
			got = rr == U_STEREO_UVC_READ_OK;
			timeouts += rr == U_STEREO_UVC_READ_TIMEOUT ? 1 : 0;
			if (rr == U_STEREO_UVC_READ_ERROR) {
				errors++;
				break;
			}
		}
		INFO(policy << " policy: " << timeouts << " timeout(s), " << errors << " error(s) before a frame");
		REQUIRE(got);
		const uint32_t edge = r.width / 20; // 5 %
		const double left = mean_luma(r, 0, edge);
		const double right = mean_luma(r, r.width - edge, r.width);
		const double centre = mean_luma(r, r.width / 4, r.width * 3 / 4);
		std::printf(
		    "[uvc aspect] %s policy: delivered %ux%u, mean luma outer-left %.1f / centre %.1f / "
		    "outer-right %.1f\n",
		    policy, r.width, r.height, left, centre, right);
		CHECK(r.width == 1280);
		CHECK(r.height == 360);
		CHECK(centre > 120.0);
		// A letterboxed frame has ~16 (black) here; the picture is ~160 everywhere.
		CHECK(left > 0.8 * centre);
		CHECK(right > 0.8 * centre);
		b.close(h);
	}
	_putenv_s("DXR_STEREO_CAMERA_UVC_DECODER", "");
	std::remove(path.c_str());
}
