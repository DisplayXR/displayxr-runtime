// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Standalone HUD text renderer with CPU pixel readback
 *
 * Uses a private D3D11 device + DirectWrite/D2D to render text to a staging
 * texture, then maps it for CPU read. Non-D3D11 apps (D3D12, OpenGL, Vulkan)
 * call RenderHudAndMap() to get pixel data, then upload via their native API.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>

#include "text_overlay.h"

using Microsoft::WRL::ComPtr;

// One button drawn over the HUD overlay. Coordinates are in HUD-pixel space
// (the HUD texture is hudWidth × hudHeight pixels).
struct HudButton {
    std::wstring label;
    float x = 0, y = 0;
    float width = 0, height = 0;
    bool hovered = false;
};

struct HudRenderer {
    ComPtr<ID3D11Device> device;          // standalone D3D11 device (text-only)
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> renderTex;    // D2D-compatible (RENDER_TARGET)
    ComPtr<ID3D11Texture2D> stagingTex;   // CPU-readable (STAGING, CPU_ACCESS_READ)
    TextOverlay overlay;
    uint32_t width = 0;
    uint32_t height = 0;
    float normalFontSize = 0;
    float smallFontSize = 0;
};

// Create a standalone D3D11 device, render + staging textures, and DirectWrite resources.
// `fontBaseHeight`: pixel height used to scale fonts. 0 = use `h` (legacy behavior).
// Pass an explicit value when the texture is taller than the original layout target
// (e.g. a HUD layer that spans the full window so buttons can sit at top and body at bottom).
bool InitializeHudRenderer(HudRenderer& hud, uint32_t w, uint32_t h, uint32_t fontBaseHeight = 0);

// Render text to internal textures, copy to staging, map for CPU read.
// Returns pixel pointer (R8G8B8A8_UNORM) and row pitch in bytes.
// Caller must call UnmapHud() after consuming the pixels.
//
// `drawBody`: when false, skip the text sections and emit only the buttons.
// Lets a HUD-toggle key hide the info panel without losing always-visible
// chrome buttons. When buttons are present, body text is laid out below the
// button band so the two never overlap.
//
// `bodyAtBottom`: when true, anchor the body block to the bottom of the
// texture (instead of immediately under the buttons) and clear the texture
// to fully transparent (alpha=0) instead of a translucent black backdrop.
// A tight semi-transparent rect is drawn behind the body region only.
// Used when the HUD swapchain spans the full window so the empty middle
// stays invisible (requires the compositor to honor source alpha).
const void* RenderHudAndMap(HudRenderer& hud, uint32_t* rowPitch,
    const std::wstring& sessionText, const std::wstring& modeText,
    const std::wstring& perfText, const std::wstring& displayInfoText,
    const std::wstring& eyeText,
    const std::wstring& cameraText = L"",
    const std::wstring& stereoText = L"",
    const std::wstring& helpText = L"",
    const std::vector<HudButton>& buttons = {},
    bool drawBody = true,
    bool bodyAtBottom = false);

// Unmap the staging texture after pixel upload
void UnmapHud(HudRenderer& hud);

// Release all resources
void CleanupHudRenderer(HudRenderer& hud);
