// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 host-readback path for the 'I' key atlas capture.
 *
 * Same staging-texture pattern used by the service compositor's
 * `comp_d3d11_service_capture_frame()` (see
 * src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp:10331), generalised
 * to (a) take an arbitrary swapchain texture from the caller and (b) handle
 * BGRA→RGBA byte-swap (the service atlas is hard-coded RGBA8; cube-app
 * swapchains may be either).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgiformat.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "stb_image_write.h"
#include "atlas_capture.h"

namespace dxr_capture {

namespace {

bool IsBgraFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

}  // namespace

bool CaptureAtlasRegionD3D11(ID3D11Device* device,
                             ID3D11DeviceContext* context,
                             ID3D11Texture2D* srcTex,
                             uint32_t rectX,
                             uint32_t rectY,
                             uint32_t rectW,
                             uint32_t rectH,
                             const std::string& outPath) {
    if (device == nullptr || context == nullptr || srcTex == nullptr) return false;
    if (rectW == 0 || rectH == 0) return false;

    D3D11_TEXTURE2D_DESC desc;
    srcTex->GetDesc(&desc);
    if ((uint64_t)rectX + rectW > desc.Width) return false;
    if ((uint64_t)rectY + rectH > desc.Height) return false;

    // Staging texture matching the source — STAGING + CPU_READ.
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.SampleDesc = {1, 0};      // staging textures must be non-multisampled
    sd.ArraySize = 1;
    sd.MipLevels = 1;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
        return false;
    }

    // CopySubresourceRegion to copy just the requested sub-rect into the
    // staging texture's top-left. (CopyResource would copy the whole image,
    // including any black padding outside the active atlas region.)
    D3D11_BOX box;
    box.left = rectX;
    box.top = rectY;
    box.front = 0;
    box.right = rectX + rectW;
    box.bottom = rectY + rectH;
    box.back = 1;
    context->CopySubresourceRegion(staging, 0, 0, 0, 0, srcTex, 0, &box);

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        staging->Release();
        return false;
    }

    // Tightly-pack RGBA into a contiguous buffer (RowPitch may exceed rectW*4).
    std::vector<uint8_t> rgba((size_t)rectW * rectH * 4u);
    const uint8_t* src = static_cast<const uint8_t*>(m.pData);
    for (uint32_t y = 0; y < rectH; y++) {
        std::memcpy(rgba.data() + (size_t)y * rectW * 4u,
                    src + (size_t)y * m.RowPitch,
                    (size_t)rectW * 4u);
    }
    context->Unmap(staging, 0);
    staging->Release();

    if (IsBgraFormat(desc.Format)) {
        for (size_t i = 0; i < rgba.size(); i += 4) {
            std::swap(rgba[i + 0], rgba[i + 2]);
        }
    }

    int ok = stbi_write_png(outPath.c_str(), (int)rectW, (int)rectH, 4,
                            rgba.data(), (int)rectW * 4);
    return ok != 0;
}

}  // namespace dxr_capture
