// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CPU-side HUD text rasterizer for macOS — Core Graphics + Core Text.
 *
 * Mirrors test_apps/common/hud_renderer.{h,cpp} (the Windows DirectWrite/D2D
 * version): produces an R8G8B8A8 CPU buffer that the caller uploads into its
 * graphics-API-specific HUD swapchain image (Metal `replaceRegion:`,
 * GL `glTexSubImage2D`, Vulkan staging-buffer copy).
 */

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

// Mirror Windows HudButton struct so future button-band features can be
// shared layout-wise; macOS apps don't draw buttons today and pass the
// default empty vector.
struct HudButtonMacOS {
    std::wstring label;
    float x = 0, y = 0;
    float width = 0, height = 0;
    bool hovered = false;
};

struct HudRendererMacOS {
    uint32_t width = 0;
    uint32_t height = 0;
    float normalFontSize = 0;
    float smallFontSize = 0;
    // Owned CPU-side RGBA8 bitmap (row pitch = width * 4).
    void* bitmapData = nullptr;
    // Backing CGContextRef as void* to keep the header free of ObjC / CoreGraphics.
    void* cgContext = nullptr;
    // Cached CTFontRef instances so we don't recreate per frame.
    void* normalFont = nullptr;
    void* smallFont = nullptr;
};

bool InitializeHudRenderer(HudRendererMacOS& hud, uint32_t w, uint32_t h, uint32_t fontBaseHeight = 0);

const void* RenderHudAndMap(HudRendererMacOS& hud, uint32_t* rowPitch,
    const std::wstring& sessionText, const std::wstring& modeText,
    const std::wstring& perfText, const std::wstring& displayInfoText,
    const std::wstring& eyeText,
    const std::wstring& cameraText = L"",
    const std::wstring& stereoText = L"",
    const std::wstring& helpText = L"",
    const std::vector<HudButtonMacOS>& buttons = {},
    bool drawBody = true,
    bool bodyAtBottom = false);

// No-op on macOS (the bitmap is plain CPU memory) — kept for parity with
// the Windows API so call sites can stay symmetric.
void UnmapHud(HudRendererMacOS& hud);

void CleanupHudRenderer(HudRendererMacOS& hud);
