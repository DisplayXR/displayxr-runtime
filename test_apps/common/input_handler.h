// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keyboard and mouse input state tracking
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>

#include "stereo_params.h"

struct InputState {
    // Mouse state
    int mouseX = 0;
    int mouseY = 0;
    bool leftButton = false;
    bool rightButton = false;
    bool middleButton = false;

    // Mouse drag for camera look
    bool dragging = false;
    int dragStartX = 0;
    int dragStartY = 0;
    float yaw = 0.0f;    // Camera yaw (radians)
    float pitch = 0.0f;  // Camera pitch (radians)

    // Keyboard state for WASDQE movement
    bool keyW = false;
    bool keyA = false;
    bool keyS = false;
    bool keyD = false;
    bool keyE = false;      // Up
    bool keyQ = false;      // Down
    bool keyP = false;      // Parallax toggle (press)
    bool keyF11 = false;    // Fullscreen toggle (press)

    // Last key pressed for display
    std::string lastKey;

    // Camera position (updated by movement)
    float cameraPosX = 0.0f;
    float cameraPosY = 0.0f;
    float cameraPosZ = 0.0f;  // Start at origin (OpenXR view matrix provides eye offset)

    // View reset (spacebar or double-click)
    bool resetViewRequested = false;

    // Parallax toggle state
    bool parallaxEnabled = true;

    // Fullscreen state
    bool fullscreen = false;
    bool fullscreenToggleRequested = false;

    // Stereo camera parameters (IPD, parallax, perspective, scale/zoom)
    StereoParams stereo;

    // HUD visibility toggle (TAB key)
    bool hudVisible = true;

    // Display mode toggle (V key)
    bool displayMode3D = true;
    bool displayModeToggleRequested = false;

    // Output mode for sim_display (1/2/3 keys: 0=SBS, 1=Anaglyph, 2=Blend)
    int outputMode = 0;
    bool outputModeChangeRequested = false;
};

// Process a Win32 message and update input state
// Returns true if the message was handled
bool UpdateInputState(InputState& state, UINT msg, WPARAM wParam, LPARAM lParam);

// Update camera position based on current key states
// deltaTime is in seconds
void UpdateCameraMovement(InputState& state, float deltaTime);

// Get a string describing current mouse button state
std::string GetMouseButtonString(const InputState& state);
