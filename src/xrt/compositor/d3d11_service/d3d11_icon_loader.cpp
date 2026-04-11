// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PNG/JPEG → D3D11 texture loader implementation (stb_image-backed).
 * @ingroup comp_d3d11_service
 */

#include "d3d11_icon_loader.h"

#include "util/u_logging.h"

#include <d3d11.h>
#include <cstring>

// This is the single translation unit that instantiates stb_image.
// STBI_NO_STDIO is deliberately NOT defined — we want stbi_load(path).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

bool
d3d11_icon_load_from_file(ID3D11Device *device,
                          const char *path,
                          ID3D11ShaderResourceView **out_srv,
                          uint32_t *out_width,
                          uint32_t *out_height)
{
	if (out_srv != nullptr) {
		*out_srv = nullptr;
	}
	if (out_width != nullptr) {
		*out_width = 0;
	}
	if (out_height != nullptr) {
		*out_height = 0;
	}

	if (device == nullptr || path == nullptr || out_srv == nullptr) {
		return false;
	}

	int w = 0;
	int h = 0;
	int channels_in_file = 0;
	// Force RGBA8 output regardless of source channel count.
	stbi_uc *pixels = stbi_load(path, &w, &h, &channels_in_file, 4);
	if (pixels == nullptr) {
		U_LOG_W("d3d11_icon_load_from_file: stbi_load failed for '%s': %s", path,
		        stbi_failure_reason());
		return false;
	}
	if (w <= 0 || h <= 0) {
		stbi_image_free(pixels);
		U_LOG_W("d3d11_icon_load_from_file: invalid dims for '%s'", path);
		return false;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = static_cast<UINT>(w);
	desc.Height = static_cast<UINT>(h);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	D3D11_SUBRESOURCE_DATA init = {};
	init.pSysMem = pixels;
	init.SysMemPitch = static_cast<UINT>(w) * 4u;
	init.SysMemSlicePitch = 0;

	ID3D11Texture2D *tex = nullptr;
	HRESULT hr = device->CreateTexture2D(&desc, &init, &tex);
	stbi_image_free(pixels);

	if (FAILED(hr) || tex == nullptr) {
		U_LOG_W("d3d11_icon_load_from_file: CreateTexture2D failed 0x%08lX for '%s'",
		        static_cast<unsigned long>(hr), path);
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = desc.Format;
	srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MostDetailedMip = 0;
	srv_desc.Texture2D.MipLevels = 1;

	ID3D11ShaderResourceView *srv = nullptr;
	hr = device->CreateShaderResourceView(tex, &srv_desc, &srv);
	tex->Release();

	if (FAILED(hr) || srv == nullptr) {
		U_LOG_W("d3d11_icon_load_from_file: CreateShaderResourceView failed 0x%08lX for '%s'",
		        static_cast<unsigned long>(hr), path);
		return false;
	}

	*out_srv = srv;
	if (out_width != nullptr) {
		*out_width = static_cast<uint32_t>(w);
	}
	if (out_height != nullptr) {
		*out_height = static_cast<uint32_t>(h);
	}
	return true;
}
