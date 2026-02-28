// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Panel text rendering using D2D + DirectWrite
 *
 * Renders panel content (titles, text, colored backgrounds) directly onto
 * D3D11 textures that back the OpenXR swapchain images.
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

using Microsoft::WRL::ComPtr;

struct PanelRenderer {
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> dwriteFactory;
    ComPtr<IDWriteTextFormat> titleFormat;
    ComPtr<IDWriteTextFormat> bodyFormat;
    ComPtr<IDWriteTextFormat> smallFormat;
    bool initialized = false;
};

// Initialize D2D/DirectWrite factories and text formats
bool InitializePanelRenderer(PanelRenderer& renderer);

// Render panel content to a D3D11 texture
// device: the D3D11 device that owns the texture
// texture: target texture from swapchain
// title: panel title text
// body: panel body text
// bgColor: RGBA background color
// accentColor: RGB accent for title bar
// alpha: overall opacity
// selected: draw highlight border
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
);

// Release resources
void CleanupPanelRenderer(PanelRenderer& renderer);
