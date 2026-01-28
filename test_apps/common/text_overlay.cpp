// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  On-screen text rendering implementation
 */

#include "text_overlay.h"
#include <d2d1_1.h>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

bool InitializeTextOverlay(TextOverlay& overlay) {
    HRESULT hr;

    // Create D2D factory
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&overlay.d2dFactory));
    if (FAILED(hr)) return false;

    // Create DirectWrite factory
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(overlay.dwriteFactory.GetAddressOf())
    );
    if (FAILED(hr)) return false;

    // Create text format for normal text
    hr = overlay.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        16.0f,
        L"en-us",
        &overlay.textFormat
    );
    if (FAILED(hr)) return false;

    // Create text format for small text
    hr = overlay.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f,
        L"en-us",
        &overlay.smallTextFormat
    );
    if (FAILED(hr)) return false;

    overlay.initialized = true;
    return true;
}

void CleanupTextOverlay(TextOverlay& overlay) {
    overlay.textFormat.Reset();
    overlay.smallTextFormat.Reset();
    overlay.dwriteFactory.Reset();
    overlay.d2dFactory.Reset();
    overlay.initialized = false;
}

// Track if we've logged D2D errors already (to avoid spam)
static bool g_d2dErrorLogged = false;

void RenderText(
    TextOverlay& overlay,
    ID3D11Device* device,
    ID3D11Texture2D* texture,
    const std::wstring& text,
    float x, float y,
    float width, float height,
    bool useSmallFont
) {
    if (!overlay.initialized || !texture || !device) {
        if (!g_d2dErrorLogged) {
            OutputDebugStringA("RenderText: overlay not initialized or texture/device null\n");
            g_d2dErrorLogged = true;
        }
        return;
    }

    // Flush D3D11 context before D2D access (required for interop)
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (context) {
        context->Flush();
    }

    // Get DXGI surface from texture
    ComPtr<IDXGISurface> surface;
    HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(&surface));
    if (FAILED(hr)) {
        if (!g_d2dErrorLogged) {
            char buf[256];
            sprintf_s(buf, "RenderText: QueryInterface for IDXGISurface failed: 0x%08X\n", hr);
            OutputDebugStringA(buf);
            g_d2dErrorLogged = true;
        }
        return;
    }

    // Create D2D render target for this surface
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    ComPtr<ID2D1RenderTarget> d2dRenderTarget;
    hr = overlay.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.Get(), &rtProps, &d2dRenderTarget);
    if (FAILED(hr)) {
        if (!g_d2dErrorLogged) {
            char buf[256];
            sprintf_s(buf, "RenderText: CreateDxgiSurfaceRenderTarget failed: 0x%08X\n", hr);
            OutputDebugStringA(buf);
            g_d2dErrorLogged = true;
        }
        return;
    }

    // Create brush for text
    ComPtr<ID2D1SolidColorBrush> textBrush;
    hr = d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &textBrush);
    if (FAILED(hr)) return;

    // Create brush for background
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    hr = d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f), &bgBrush);
    if (FAILED(hr)) return;

    // Draw
    d2dRenderTarget->BeginDraw();

    // Draw semi-transparent background
    D2D1_RECT_F bgRect = D2D1::RectF(x - 5, y - 2, x + width + 5, y + height + 2);
    d2dRenderTarget->FillRectangle(bgRect, bgBrush.Get());

    // Draw text
    D2D1_RECT_F textRect = D2D1::RectF(x, y, x + width, y + height);
    d2dRenderTarget->DrawText(
        text.c_str(),
        (UINT32)text.length(),
        useSmallFont ? overlay.smallTextFormat.Get() : overlay.textFormat.Get(),
        textRect,
        textBrush.Get()
    );

    d2dRenderTarget->EndDraw();
}

std::wstring FormatSessionState(int state) {
    // XrSessionState values
    const wchar_t* stateNames[] = {
        L"UNKNOWN",
        L"IDLE",
        L"READY",
        L"SYNCHRONIZED",
        L"VISIBLE",
        L"FOCUSED",
        L"STOPPING",
        L"LOSS_PENDING",
        L"EXITING"
    };

    if (state >= 0 && state < 9) {
        return stateNames[state];
    }
    return L"INVALID";
}

std::wstring FormatPerformanceInfo(float fps, float frameTimeMs, uint32_t width, uint32_t height) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << L"FPS: " << fps << L"\n";
    oss << L"Frame: " << frameTimeMs << L"ms\n";
    oss << L"Window: " << width << L"x" << height;
    return oss.str();
}

std::wstring FormatInputInfo(const std::string& lastKey, int mouseX, int mouseY, const std::string& mouseButtons) {
    std::wostringstream oss;
    oss << L"Input:\n";
    oss << L"  Key: " << std::wstring(lastKey.begin(), lastKey.end()) << L"\n";
    oss << L"  Mouse: (" << mouseX << L", " << mouseY << L")\n";
    oss << L"  Buttons: " << std::wstring(mouseButtons.begin(), mouseButtons.end());
    return oss.str();
}

std::wstring FormatEyeTrackingInfo(float posX, float posY, bool active) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << L"Eye Tracking:\n";
    if (active) {
        oss << L"  Pos: (" << posX << L", " << posY << L")\n";
        oss << L"  Active: Yes";
    } else {
        oss << L"  Active: No";
    }
    return oss.str();
}

std::wstring FormatParallaxInfo(bool parallaxEnabled, float eyePosX, float eyePosY) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << L"Parallax: " << (parallaxEnabled ? L"ON" : L"OFF") << L"\n";
    if (parallaxEnabled) {
        oss << L"  Eye X: " << eyePosX << L"\n";
        oss << L"  Eye Y: " << eyePosY;
    }
    return oss.str();
}
