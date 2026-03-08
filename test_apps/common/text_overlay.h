// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  On-screen text rendering using DirectWrite
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <d3d11.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

struct TextOverlay {
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> dwriteFactory;
    ComPtr<IDWriteTextFormat> textFormat;
    ComPtr<IDWriteTextFormat> smallTextFormat;

    bool initialized = false;
};

// Initialize DirectWrite resources (font sizes default to 20pt normal, 15pt small)
bool InitializeTextOverlay(TextOverlay& overlay, float normalFontSize = 20.0f, float smallFontSize = 15.0f);

// Clean up resources
void CleanupTextOverlay(TextOverlay& overlay);

// Render text to a D3D11 render target
// Note: This creates temporary D2D resources per-frame for simplicity
// A production implementation would cache these
void RenderText(
    TextOverlay& overlay,
    ID3D11Device* device,
    ID3D11Texture2D* texture,
    const std::wstring& text,
    float x, float y,
    float width, float height,
    bool useSmallFont = false
);

// Helper to format various overlay texts
std::wstring FormatSessionState(int state);
std::wstring FormatPerformanceInfo(float fps, float frameTimeMs, uint32_t renderW, uint32_t renderH, uint32_t windowW = 0, uint32_t windowH = 0);
std::wstring FormatInputInfo(const std::string& lastKey, int mouseX, int mouseY, const std::string& mouseButtons);
std::wstring FormatDisplayInfo(float widthM, float heightM, float nomX, float nomY, float nomZ);
std::wstring FormatEyeTrackingInfo(float lx, float ly, float lz, float rx, float ry, float rz,
    bool active, bool isTracking, uint32_t activeMode, uint32_t supportedModes);
std::wstring FormatParallaxInfo(bool parallaxEnabled, float eyePosX, float eyePosY);
std::wstring FormatOutputMode(int outputMode, bool simDisplayAvailable, const char* modeName = nullptr, uint32_t modeCount = 0);
std::wstring FormatCameraInfo(float cameraPosX, float cameraPosY, float cameraPosZ,
    float forwardX, float forwardY, float forwardZ, bool cameraMode = false);
std::wstring FormatStereoParams(float ipdFactor, float parallaxFactor,
    float perspectiveFactor, float scaleFactor, bool cameraMode = false);
std::wstring FormatScaleInfo(float scaleX, float scaleY);
std::wstring FormatHelpText(bool simDisplayAvailable, bool cameraMode = false, uint32_t modeCount = 0);
