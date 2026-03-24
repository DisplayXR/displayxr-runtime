// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Panel text rendering implementation using D2D + DirectWrite
 */

#include "panel_render.h"
#include "logging.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

bool InitializePanelRenderer(PanelRenderer& renderer) {
    ID2D1Factory* pFactory = nullptr;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pFactory);
    renderer.d2dFactory.Attach(pFactory);
    if (FAILED(hr)) {
        LOG_ERROR("PanelRenderer: D2D1CreateFactory failed: 0x%08X", hr);
        return false;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(renderer.dwriteFactory.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ERROR("PanelRenderer: DWriteCreateFactory failed: 0x%08X", hr);
        return false;
    }

    // Title font: bold, 16pt
    hr = renderer.dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        16.0f, L"en-us", &renderer.titleFormat);
    if (FAILED(hr)) return false;

    // Body font: regular, 13pt
    hr = renderer.dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13.0f, L"en-us", &renderer.bodyFormat);
    if (FAILED(hr)) return false;

    // Small font: regular, 11pt
    hr = renderer.dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        11.0f, L"en-us", &renderer.smallFormat);
    if (FAILED(hr)) return false;

    renderer.initialized = true;
    LOG_INFO("PanelRenderer initialized");
    return true;
}

void RenderPanelContent(
    PanelRenderer& renderer,
    ID3D11Device* device,
    ID3D11Texture2D* texture,
    const wchar_t* title,
    const wchar_t* body,
    const float bgColor[4],
    const float accentColor[3],
    float alpha,
    bool selected
) {
    if (!renderer.initialized || !texture) return;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    float w = (float)desc.Width;
    float h = (float)desc.Height;

    // Swapchain textures may be TYPELESS (ADR-011). D2D requires a typed surface,
    // so render to a temporary R8G8B8A8_UNORM texture and copy back.
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = desc.Width;
    stagingDesc.Height = desc.Height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_DEFAULT;
    stagingDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> stagingTex;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    if (FAILED(hr)) return;

    ComPtr<IDXGISurface> surface;
    hr = stagingTex->QueryInterface(__uuidof(IDXGISurface), (void**)surface.GetAddressOf());
    if (FAILED(hr)) return;

    // Create D2D render target from typed staging surface
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    ComPtr<ID2D1RenderTarget> rt;
    hr = renderer.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.Get(), &rtProps, &rt);
    if (FAILED(hr)) return;

    rt->BeginDraw();

    // Background
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    rt->CreateSolidColorBrush(
        D2D1::ColorF(bgColor[0], bgColor[1], bgColor[2], bgColor[3] * alpha), &bgBrush);
    rt->FillRectangle(D2D1::RectF(0, 0, w, h), bgBrush.Get());

    // Title bar accent strip (top 32px)
    float titleBarH = 32.0f;
    ComPtr<ID2D1SolidColorBrush> accentBrush;
    rt->CreateSolidColorBrush(
        D2D1::ColorF(accentColor[0], accentColor[1], accentColor[2], 0.9f * alpha), &accentBrush);
    rt->FillRectangle(D2D1::RectF(0, 0, w, titleBarH), accentBrush.Get());

    // Title text (white on accent bar)
    ComPtr<ID2D1SolidColorBrush> textBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha), &textBrush);
    rt->DrawText(title, (UINT32)wcslen(title), renderer.titleFormat.Get(),
        D2D1::RectF(10, 6, w - 10, titleBarH), textBrush.Get());

    // Body text (light gray below title bar)
    ComPtr<ID2D1SolidColorBrush> bodyBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.85f, 0.88f, alpha), &bodyBrush);
    rt->DrawText(body, (UINT32)wcslen(body), renderer.bodyFormat.Get(),
        D2D1::RectF(10, titleBarH + 8, w - 10, h - 8), bodyBrush.Get());

    // Selection highlight border
    if (selected) {
        ComPtr<ID2D1SolidColorBrush> borderBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.8f, 1.0f, 1.0f), &borderBrush);
        rt->DrawRectangle(D2D1::RectF(1, 1, w - 1, h - 1), borderBrush.Get(), 3.0f);
    }

    rt->EndDraw();

    // Copy rendered content from typed staging texture to swapchain texture
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    ctx->CopyResource(texture, stagingTex.Get());
}

void CleanupPanelRenderer(PanelRenderer& renderer) {
    renderer.smallFormat.Reset();
    renderer.bodyFormat.Reset();
    renderer.titleFormat.Reset();
    renderer.dwriteFactory.Reset();
    renderer.d2dFactory.Reset();
    renderer.initialized = false;
}
