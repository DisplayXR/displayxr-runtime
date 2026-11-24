// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Weave QA test content: side-by-side stereo images shown one per view.
 *
 * tools/weave_qa shows test patterns through this app and photographs the panel with a
 * stereo camera standing in for a viewer. Each image is side by side: the left half is
 * view 0 (left eye), the right half view 1 (right eye). Each half is stretched to fill its
 * whole view, so a pattern made for one eye at the panel's size comes out pixel for pixel
 * in full screen.
 *
 *   DISPLAYXR_WEAVE_PATTERN=a.png;b.png   images to show, in turn (';' separated)
 *   DISPLAYXR_WEAVE_PATTERN_PERIOD_MS=N   how long each is shown (default 3000)
 *
 * Bytes are written as they are in the file, as the stereo photo does: the analysis
 * calibrates the camera from the pattern's own reference row, so it does not depend on
 * the swapchain's transfer function.
 *
 * Header-only; the app supplies stbi_load (test_apps' common library compiles stb_image).
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "stb_image.h"

namespace dxr_test {

struct WeavePattern
{
	struct Image
	{
		int w = 0, h = 0;          //!< whole side-by-side image
		std::vector<uint8_t> rgba; //!< w * h * 4
		std::string path;
	};

	std::vector<Image> images;
	int periodMs = 3000;
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

	// Per view, the current image resampled to the view's size, in the swapchain's byte order.
	struct Cache
	{
		int image = -1;
		uint32_t w = 0, h = 0;
		bool bgra = false;
		std::vector<uint8_t> px;
	} cache[2];

	bool active() const { return !images.empty(); }

	/*!
	 * Reads DISPLAYXR_WEAVE_PATTERN[_PERIOD_MS]. Returns false when the variable is not set.
	 * \p error names the first file that could not be loaded (and nothing is shown then).
	 */
	bool loadFromEnvironment(std::string& error)
	{
		const char* list = getenv("DISPLAYXR_WEAVE_PATTERN");
		if (list == nullptr || list[0] == '\0') return false;
		if (const char* p = getenv("DISPLAYXR_WEAVE_PATTERN_PERIOD_MS")) {
			const int v = atoi(p);
			if (v > 0) periodMs = v;
		}
		std::string all = list;
		size_t at = 0;
		while (at <= all.size()) {
			const size_t end = all.find(';', at);
			const std::string path = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
			at = end == std::string::npos ? all.size() + 1 : end + 1;
			if (path.empty()) continue;
			Image img;
			int channels = 0;
			unsigned char* data = stbi_load(path.c_str(), &img.w, &img.h, &channels, 4);
			if (data == nullptr || img.w < 2 || img.h < 1) {
				if (data) stbi_image_free(data);
				error = path;
				images.clear();
				return true;
			}
			img.rgba.assign(data, data + (size_t)img.w * img.h * 4);
			stbi_image_free(data);
			img.path = path;
			images.push_back(std::move(img));
		}
		start = std::chrono::steady_clock::now();
		return true;
	}

	//! Index of the image shown now.
	int current() const
	{
		if (images.size() <= 1) return 0;
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		return (int)((ms / periodMs) % (long long)images.size());
	}

	/*!
	 * Pixels of view \p eye (0 left, 1 right; any other view gets the nearer half) at
	 * \p w x \p h, tightly packed, RGBA or BGRA. Resampled only when the image or size changes.
	 */
	const std::vector<uint8_t>& view(int eye, uint32_t w, uint32_t h, bool bgra)
	{
		const int e = eye <= 0 ? 0 : 1;
		const int idx = current();
		Cache& c = cache[e];
		if (c.image == idx && c.w == w && c.h == h && c.bgra == bgra) return c.px;
		c.image = idx;
		c.w = w;
		c.h = h;
		c.bgra = bgra;
		const Image& img = images[idx];
		const int half = img.w / 2, x0 = e * half;
		c.px.resize((size_t)w * h * 4);
		// Nearest neighbour: a pattern at the view's size maps 1:1, and edges stay sharp.
		for (uint32_t y = 0; y < h; y++) {
			const int sy = (int)(((uint64_t)y * img.h) / h);
			const uint8_t* srow = &img.rgba[(size_t)sy * img.w * 4];
			uint8_t* drow = &c.px[(size_t)y * w * 4];
			for (uint32_t x = 0; x < w; x++) {
				const int sx = x0 + (int)(((uint64_t)x * half) / w);
				const uint8_t* s = srow + (size_t)sx * 4;
				uint8_t* d = drow + (size_t)x * 4;
				d[0] = bgra ? s[2] : s[0];
				d[1] = s[1];
				d[2] = bgra ? s[0] : s[2];
				d[3] = 255;
			}
		}
		return c.px;
	}
};

} // namespace dxr_test
