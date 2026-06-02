// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Shared helpers for the windowspace_handle_* test apps (#389).
 *
 * Pure CPU helpers: parse the layer-count argument and generate a distinct
 * solid RGBA8 color per layer. No graphics-API dependency.
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <vector>

namespace wsl {

// Per-layer geometry constants. N layers stacked as a vertical column of
// "buttons" down the left edge so a dropped/flickering layer is obvious.
static const float    kLayerX        = 0.03f;
static const float    kLayerW        = 0.20f;
static const float    kLayerH        = 0.11f;
static const float    kLayerYStart   = 0.03f;
static const float    kLayerYStride  = 0.14f;
static const uint32_t kMinLayers     = 1;
static const uint32_t kMaxLayers     = 12;
static const uint32_t kDefaultLayers = 6;

// Per-layer swapchain pixel dimensions.
static const uint32_t kLayerPxWidth  = 256;
static const uint32_t kLayerPxHeight = 128;

// Background fill for the projection swapchain (flat dark, RGBA8).
static const uint8_t  kBgR = 20, kBgG = 20, kBgB = 28, kBgA = 255;

// Parse N from a WinMain command line (use __argc/__argv). Defaults to 6,
// clamped to [1,12].
inline uint32_t ParseLayerCount(int argc, char** argv) {
    uint32_t n = kDefaultLayers;
    if (argc > 1 && argv && argv[1]) {
        long v = strtol(argv[1], nullptr, 10);
        if (v >= 1) n = (uint32_t)v;
    }
    if (n < kMinLayers) n = kMinLayers;
    if (n > kMaxLayers) n = kMaxLayers;
    return n;
}

// HSV (h in [0,1)) -> RGB, all in [0,1].
inline void HsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    float i = std::floor(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch (((int)i) % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

// Distinct bright RGBA8 color for layer i out of n (hue spread across the wheel).
inline void LayerColor(uint32_t i, uint32_t n, uint8_t out[4]) {
    float h = (n > 0) ? ((float)i / (float)n) : 0.0f;
    float r, g, b;
    HsvToRgb(h, 0.85f, 1.0f, r, g, b);
    out[0] = (uint8_t)(r * 255.0f + 0.5f);
    out[1] = (uint8_t)(g * 255.0f + 0.5f);
    out[2] = (uint8_t)(b * 255.0f + 0.5f);
    out[3] = 255;
}

// Fill a tightly-packed RGBA8 buffer (w*h*4 bytes) with a solid color.
inline void FillSolid(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    buf.resize((size_t)w * h * 4);
    for (size_t px = 0; px < (size_t)w * h; ++px) {
        buf[px * 4 + 0] = rgba[0];
        buf[px * 4 + 1] = rgba[1];
        buf[px * 4 + 2] = rgba[2];
        buf[px * 4 + 3] = rgba[3];
    }
}

} // namespace wsl
