// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Spatial OS app state and panel management
 */

#pragma once

#include "panel.h"

static const int NUM_PANELS = 5;  // participants, chat, agenda, action items, scene3D
static const float LERP_SPEED = 8.0f;  // animation speed (units/sec)

struct SpatialAppState {
    Panel panels[NUM_PANELS];
    int selectedIndex = -1;
    bool focusMode = false;
    int focusedPanel = -1;
};

// Initialize all panels with default layout and content
void InitializePanels(SpatialAppState& app);

// Keyboard input processing
void HandleTabKey(SpatialAppState& app);
void HandleSpaceKey(SpatialAppState& app);
void HandleEscapeReset(SpatialAppState& app);
void HandleDisparityToggle(SpatialAppState& app);

// Per-frame layout animation
void UpdatePanelAnimations(SpatialAppState& app, float deltaTime);
