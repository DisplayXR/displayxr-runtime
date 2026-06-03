// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CPU box-filter mip-chain generation for RGBA8 textures.
 *
 * Backend-agnostic helper so every cube test app uploads a full mip chain
 * and the wood-crate texture stays smooth under minification — matching
 * cube_handle_gl_win's glGenerateMipmap. Used by the D3D11 and D3D12
 * renderers, which have no GPU auto-mip path as convenient as GL's.
 */
#pragma once

#include <vector>

namespace dxr_mip {

struct MipLevel
{
	std::vector<unsigned char> pixels; //!< RGBA8, tightly packed (width*height*4)
	int width;
	int height;
};

// Build a full box-filtered mip chain for a tightly-packed RGBA8 image.
// Level 0 is a copy of the input; each subsequent level is half size (floored,
// min 1) down to 1x1.
inline std::vector<MipLevel>
GenerateMipChainRGBA8(const unsigned char *src, int w, int h)
{
	std::vector<MipLevel> levels;
	levels.push_back({std::vector<unsigned char>(src, src + (size_t)w * h * 4), w, h});

	while (levels.back().width > 1 || levels.back().height > 1) {
		const MipLevel &prev = levels.back();
		const int pw = prev.width, ph = prev.height;
		const int nw = pw > 1 ? pw / 2 : 1;
		const int nh = ph > 1 ? ph / 2 : 1;

		MipLevel next;
		next.width = nw;
		next.height = nh;
		next.pixels.resize((size_t)nw * nh * 4);

		for (int y = 0; y < nh; y++) {
			const int y0 = y * 2;
			const int y1 = (y0 + 1 < ph) ? y0 + 1 : y0;
			for (int x = 0; x < nw; x++) {
				const int x0 = x * 2;
				const int x1 = (x0 + 1 < pw) ? x0 + 1 : x0;
				for (int c = 0; c < 4; c++) {
					const int s = prev.pixels[((size_t)y0 * pw + x0) * 4 + c] +
					              prev.pixels[((size_t)y0 * pw + x1) * 4 + c] +
					              prev.pixels[((size_t)y1 * pw + x0) * 4 + c] +
					              prev.pixels[((size_t)y1 * pw + x1) * 4 + c];
					next.pixels[((size_t)y * nw + x) * 4 + c] = (unsigned char)(s / 4);
				}
			}
		}
		levels.push_back(std::move(next));
	}
	return levels;
}

} // namespace dxr_mip
