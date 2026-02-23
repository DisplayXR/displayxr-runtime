// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Standalone HUD text renderer implementation
 */

#include "hud_renderer.h"
#include "logging.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

bool InitializeHudRenderer(HudRenderer& hud, uint32_t w, uint32_t h) {
    hud.width = w;
    hud.height = h;

    // Create a standalone D3D11 device (hardware, default adapter)
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &hud.device, &featureLevel, &hud.context);
    if (FAILED(hr)) {
        LOG_ERROR("HudRenderer: D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }
    LOG_INFO("HudRenderer: D3D11 device created (feature level 0x%X)", featureLevel);

    // Create render texture (D2D-compatible, RENDER_TARGET bind)
    D3D11_TEXTURE2D_DESC renderDesc = {};
    renderDesc.Width = w;
    renderDesc.Height = h;
    renderDesc.MipLevels = 1;
    renderDesc.ArraySize = 1;
    renderDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    renderDesc.SampleDesc.Count = 1;
    renderDesc.Usage = D3D11_USAGE_DEFAULT;
    renderDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

    hr = hud.device->CreateTexture2D(&renderDesc, nullptr, &hud.renderTex);
    if (FAILED(hr)) {
        LOG_ERROR("HudRenderer: CreateTexture2D (render) failed: 0x%08X", hr);
        return false;
    }

    // Create staging texture (CPU-readable)
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = w;
    stagingDesc.Height = h;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = hud.device->CreateTexture2D(&stagingDesc, nullptr, &hud.stagingTex);
    if (FAILED(hr)) {
        LOG_ERROR("HudRenderer: CreateTexture2D (staging) failed: 0x%08X", hr);
        return false;
    }

    // Scale font sizes proportionally based on HUD dimensions.
    // Base reference: 280px height with 20pt/15pt fonts (D3D11 ext app).
    float fontScale = h / 280.0f;
    float normalFontSize = 20.0f * fontScale;
    float smallFontSize = 15.0f * fontScale;

    if (!InitializeTextOverlay(hud.overlay, normalFontSize, smallFontSize)) {
        LOG_ERROR("HudRenderer: InitializeTextOverlay failed");
        return false;
    }

    LOG_INFO("HudRenderer: initialized (%ux%u)", w, h);
    return true;
}

const void* RenderHudAndMap(HudRenderer& hud, uint32_t* rowPitch,
    const std::wstring& sessionText, const std::wstring& modeText,
    const std::wstring& perfText, const std::wstring& displayInfoText,
    const std::wstring& eyeText,
    const std::wstring& cameraText,
    const std::wstring& stereoText,
    const std::wstring& helpText)
{
    // Clear render texture with semi-transparent black
    ID3D11RenderTargetView* rtv = nullptr;
    hud.device->CreateRenderTargetView(hud.renderTex.Get(), nullptr, &rtv);
    if (rtv) {
        float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.5f};
        hud.context->ClearRenderTargetView(rtv, clearColor);
        rtv->Release();
    }

    // Has extra sections (camera, stereo, help)?
    bool hasExtra = !cameraText.empty() || !stereoText.empty() || !helpText.empty();

    // Scale layout proportionally from base dimensions
    float baseH = hasExtra ? 470.0f : 280.0f;
    float sy = hud.height / baseH;
    float px = 12.0f * (hud.width / 380.0f);   // left padding
    float tw = (float)hud.width - 2.0f * px;    // text width

    RenderText(hud.overlay, hud.device.Get(), hud.renderTex.Get(),
        sessionText, px, 12*sy, tw, 26*sy);

    RenderText(hud.overlay, hud.device.Get(), hud.renderTex.Get(),
        modeText, px, 42*sy, tw, 22*sy, true);

    RenderText(hud.overlay, hud.device.Get(), hud.renderTex.Get(),
        perfText, px, 74*sy, tw, 88*sy, true);

    RenderText(hud.overlay, hud.device.Get(), hud.renderTex.Get(),
        displayInfoText, px, 172*sy, tw, 44*sy, true);

    RenderText(hud.overlay, hud.device.Get(), hud.renderTex.Get(),
        eyeText, px, 222*sy, tw, 44*sy, true);

    if (!cameraText.empty()) {
        RenderText(hud.overlay, hud.device.Get(), hud.renderTex.Get(),
            cameraText, px, 272*sy, tw, 44*sy, true);
    }

    if (!stereoText.empty()) {
        RenderText(hud.overlay, hud.device.Get(), hud.renderTex.Get(),
            stereoText, px, 322*sy, tw, 44*sy, true);
    }

    if (!helpText.empty()) {
        RenderText(hud.overlay, hud.device.Get(), hud.renderTex.Get(),
            helpText, px, 376*sy, tw, 80*sy, true);
    }

    // Copy render texture to staging texture, then map for CPU read
    hud.context->CopyResource(hud.stagingTex.Get(), hud.renderTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = hud.context->Map(hud.stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LOG_ERROR("HudRenderer: Map failed: 0x%08X", hr);
        return nullptr;
    }

    if (rowPitch) *rowPitch = mapped.RowPitch;
    return mapped.pData;
}

void UnmapHud(HudRenderer& hud) {
    hud.context->Unmap(hud.stagingTex.Get(), 0);
}

void CleanupHudRenderer(HudRenderer& hud) {
    CleanupTextOverlay(hud.overlay);
    hud.stagingTex.Reset();
    hud.renderTex.Reset();
    hud.context.Reset();
    hud.device.Reset();
}
