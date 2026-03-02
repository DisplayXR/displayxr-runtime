// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  On-screen text rendering implementation
 */

#include "text_overlay.h"
#include "logging.h"
#include <d2d1_1.h>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

bool InitializeTextOverlay(TextOverlay& overlay, float normalFontSize, float smallFontSize) {
    HRESULT hr;

    // Create D2D factory
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&overlay.d2dFactory));
    if (FAILED(hr)) {
        LOG_ERROR("D2D1CreateFactory failed: 0x%08X", hr);
        return false;
    }
    LOG_DEBUG("D2D factory created");

    // Create DirectWrite factory
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(overlay.dwriteFactory.GetAddressOf())
    );
    if (FAILED(hr)) {
        LOG_ERROR("DWriteCreateFactory failed: 0x%08X", hr);
        return false;
    }
    LOG_DEBUG("DirectWrite factory created");

    // Create text format for normal text
    hr = overlay.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        normalFontSize,
        L"en-us",
        &overlay.textFormat
    );
    if (FAILED(hr)) {
        LOG_ERROR("CreateTextFormat (normal) failed: 0x%08X", hr);
        return false;
    }

    // Create text format for small text
    hr = overlay.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        smallFontSize,
        L"en-us",
        &overlay.smallTextFormat
    );
    if (FAILED(hr)) {
        LOG_ERROR("CreateTextFormat (small) failed: 0x%08X", hr);
        return false;
    }

    overlay.initialized = true;
    LOG_INFO("Text overlay initialized successfully");
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
            LOG_ERROR("RenderText: overlay not initialized (%d) or texture (%p) / device (%p) null",
                overlay.initialized, texture, device);
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
            LOG_ERROR("RenderText: QueryInterface for IDXGISurface failed: 0x%08X", hr);
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
            LOG_ERROR("RenderText: CreateDxgiSurfaceRenderTarget failed: 0x%08X", hr);
            g_d2dErrorLogged = true;
        }
        return;
    }

    // Create brush for text
    ComPtr<ID2D1SolidColorBrush> textBrush;
    hr = d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &textBrush);
    if (FAILED(hr)) return;

    // Draw
    d2dRenderTarget->BeginDraw();

    // Draw text (no per-text background - the overall HUD container background is sufficient)
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

std::wstring FormatPerformanceInfo(float fps, float frameTimeMs, uint32_t renderW, uint32_t renderH, uint32_t windowW, uint32_t windowH) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << L"FPS: " << fps << L"\n";
    oss << L"Frame: " << frameTimeMs << L"ms\n";
    if (windowW > 0 && windowH > 0) {
        oss << L"Render: " << renderW << L"x" << renderH << L"\n";
        oss << L"Window: " << windowW << L"x" << windowH;
    } else {
        oss << L"Window: " << renderW << L"x" << renderH;
    }
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

std::wstring FormatDisplayInfo(float widthM, float heightM, float nomX, float nomY, float nomZ) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(0);
    oss << L"Display: (" << (widthM * 1000.0f) << L"x" << (heightM * 1000.0f) << L") mm\n";
    oss << L"Nominal: (" << (nomX * 1000.0f) << L"," << (nomY * 1000.0f) << L"," << (nomZ * 1000.0f) << L") mm";
    return oss.str();
}

std::wstring FormatEyeTrackingInfo(float lx, float ly, float lz, float rx, float ry, float rz,
    bool active, bool isTracking, uint32_t activeMode, uint32_t supportedModes) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(0);
    if (active) {
        oss << L"L eye: (" << (lx * 1000.0f) << L"," << (ly * 1000.0f) << L"," << (lz * 1000.0f) << L") mm\n";
        oss << L"R eye: (" << (rx * 1000.0f) << L"," << (ry * 1000.0f) << L"," << (rz * 1000.0f) << L") mm";
    } else {
        oss << L"Eyes: inactive";
    }

    // Eye tracking mode info (v6)
    const wchar_t* modeName = (activeMode == 1) ? L"RAW" : L"SMOOTH";
    oss << L"\nTracking: " << (isTracking ? L"YES" : L"NO") << L" [" << modeName << L"]";
    // Show supported modes
    oss << L"  (";
    bool first = true;
    if (supportedModes & 0x1) { oss << L"SMOOTH"; first = false; }
    if (supportedModes & 0x2) { if (!first) oss << L"|"; oss << L"RAW"; }
    if (supportedModes == 0) { oss << L"none"; }
    oss << L") [T]";
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

std::wstring FormatOutputMode(int outputMode, bool simDisplayAvailable) {
    if (!simDisplayAvailable) {
        return L"Output: Weaved";
    }
    const wchar_t* modeNames[] = {L"SBS", L"Anaglyph", L"Blend"};
    const wchar_t* name = (outputMode >= 0 && outputMode <= 2) ? modeNames[outputMode] : L"?";
    std::wostringstream oss;
    oss << L"Output: " << name << L" [1/2/3]";
    return oss.str();
}

std::wstring FormatCameraInfo(float cameraPosX, float cameraPosY, float cameraPosZ,
    float forwardX, float forwardY, float forwardZ, bool cameraMode) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << (cameraMode ? L"Virtual Camera: (" : L"Virtual Display: (")
        << cameraPosX << L", " << cameraPosY << L", " << cameraPosZ << L")\n";
    oss << L"Forward: (" << forwardX << L", " << forwardY << L", " << forwardZ << L")";
    return oss.str();
}

std::wstring FormatStereoParams(float ipdFactor, float parallaxFactor,
    float perspectiveFactor, float scaleFactor, bool cameraMode) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << L"IPD: " << ipdFactor << L"  Parallax: " << parallaxFactor
        << L"  Scale: " << scaleFactor;
    return oss.str();
}

std::wstring FormatScaleInfo(float scaleX, float scaleY) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << L"Scale: " << scaleX << L" x " << scaleY;
    return oss.str();
}

std::wstring FormatHelpText(bool simDisplayAvailable, bool cameraMode) {
    if (simDisplayAvailable) {
        return L"WASD/QE=Move  Drag=Look  Space=Reset\n"
               L"Scroll=Scale  Shift+Scroll=IPD+Parallax\n"
               L"V=2D/3D  T=EyeMode  1/2/3=Output  Tab=HUD  F11=Full  ESC=Quit";
    }
    return L"WASD/QE=Move  Drag=Look  Space=Reset\n"
           L"Scroll=Scale  Shift+Scroll=IPD+Parallax\n"
           L"V=2D/3D  T=EyeMode  Tab=HUD  F11=Full  ESC=Quit";
}
