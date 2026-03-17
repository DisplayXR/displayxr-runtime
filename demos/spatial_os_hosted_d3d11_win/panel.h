// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Panel data structures for the Spatial OS demo
 *
 * Each panel represents a floating UI element in the war room layout.
 * Panels can be 2D (disparity=0) or "promoted to 3D" (negative disparity).
 */

#pragma once

#include <d3d11.h>
#include <string>
#include <vector>

#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"

enum class PanelType {
    Participants,
    Chat,
    Agenda,
    ActionItems,
    Scene3D  // center 3D projection panel (not a window-space layer)
};

struct PanelLayout {
    float x, y, width, height;  // fractional window coords [0..1]
};

struct Panel {
    PanelType type;
    std::wstring title;

    // Layout — current (animated) and target positions
    PanelLayout current;
    PanelLayout target;
    PanelLayout defaultLayout;  // original resting position

    // Disparity for 3D effect (0 = flat, negative = pop toward viewer, positive = push behind)
    float disparity = 0.0f;
    float targetDisparity = 0.0f;
    int disparityLevel = 0;  // 0..3 — cycles: flat, slightly out, slightly in, flat

    // Swapchain for window-space panels
    SwapchainInfo swapchainInfo;
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages;

    // Appearance
    float bgColor[4] = {0.1f, 0.1f, 0.15f, 0.92f};   // dark theme background
    float accentColor[3] = {0.3f, 0.6f, 1.0f};         // blue accent
    float alpha = 1.0f;       // current alpha (dimmed in focus mode)
    float targetAlpha = 1.0f;

    bool selected = false;
};

// Default layout constants (fractional window coords)
static const PanelLayout LAYOUT_PARTICIPANTS = {0.0f,  0.0f,  0.25f, 0.27f};
static const PanelLayout LAYOUT_CHAT         = {0.0f,  0.27f, 0.25f, 0.27f};
static const PanelLayout LAYOUT_AGENDA       = {0.0f,  0.54f, 0.25f, 0.26f};
static const PanelLayout LAYOUT_ACTION_ITEMS = {0.0f,  0.80f, 1.0f,  0.20f};
static const PanelLayout LAYOUT_SCENE_3D     = {0.25f, 0.0f,  0.75f, 0.80f};

// Disparity levels: flat → slightly out → slightly in (3-cycle, wraps back to flat)
static const float DISPARITY_LEVELS[] = {0.0f, -0.005f, 0.005f};
static const int NUM_DISPARITY_LEVELS = 3;

// Inset margin applied to panels with non-zero disparity to avoid edge clipping
static const float DISPARITY_INSET = 0.015f;
