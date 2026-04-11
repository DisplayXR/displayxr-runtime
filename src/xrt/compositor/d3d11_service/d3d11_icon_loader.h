// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PNG/JPEG → D3D11 texture loader for the shell launcher tiles.
 *
 * Decodes an image file via stb_image and creates a shader resource view bound
 * to an ID3D11Texture2D. Used by the shell's spatial launcher to load app
 * tile icons (both 2D `icon` and stereoscopic `icon_3d`).
 *
 * The loader does not interpret side-by-side / top-bottom layouts — it always
 * produces a single 2D texture matching the source image dimensions. The
 * launcher render shader samples per-eye sub-rects at draw time based on the
 * sidecar's `icon_3d_layout`.
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

/*!
 * Load an image file (PNG, JPEG, BMP, TGA — whatever stb_image supports) into
 * a newly-created D3D11 2D texture and shader resource view.
 *
 * On success returns true and writes the SRV pointer to @p out_srv (the caller
 * owns the reference and must call Release). Also writes the source
 * dimensions in pixels to @p out_width and @p out_height if non-null.
 *
 * On failure returns false; @p out_srv is set to nullptr.
 *
 * The texture is created as DXGI_FORMAT_R8G8B8A8_UNORM_SRGB so that sampling
 * applies the standard sRGB → linear conversion, matching the rest of the
 * compositor's color pipeline.
 */
bool
d3d11_icon_load_from_file(ID3D11Device *device,
                          const char *path,
                          ID3D11ShaderResourceView **out_srv,
                          uint32_t *out_width,
                          uint32_t *out_height);
