// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Embedded DisplayXR white logo PNG (doc/displayxr_white.png).
 *
 * The byte array is generated at build time from the source PNG via
 * embed_binary.cmake, so the binary artifact does not need to ship the image
 * file alongside it. Decoded at runtime by stb_image through
 * d3d11_icon_load_from_memory().
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
extern const uint8_t displayxr_white_png[];
extern const size_t displayxr_white_png_size;
}
