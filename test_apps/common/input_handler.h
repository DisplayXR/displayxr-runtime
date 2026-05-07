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

#include <openxr/openxr.h>
#include "view_params.h"

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

    // View reset (spacebar)
    bool resetViewRequested = false;

    // Teleport to clicked point (double-click)
    bool teleportRequested = false;
    float teleportMouseX = 0.0f;
    float teleportMouseY = 0.0f;

    // Teleport animation
    bool teleportAnimating = false;
    float teleportTargetX = 0, teleportTargetY = 0, teleportTargetZ = 0;

    // Parallax toggle state
    bool parallaxEnabled = true;

    // Fullscreen state
    bool fullscreen = false;
    bool fullscreenToggleRequested = false;

    // View parameters (IPD, parallax, perspective, scale/zoom)
    ViewParams viewParams;

    // HUD visibility toggle (TAB key)
    bool hudVisible = true;

    // Unified rendering mode (V key cycles, 0-8 keys select directly)
    uint32_t currentRenderingMode = 1;   // Default: mode 1 (first 3D mode)
    uint32_t renderingModeCount = 0;     // Set from xrEnumerateDisplayRenderingModesEXT
    bool renderingModeChangeRequested = false;

    // Camera vs display mode toggle (C key)
    bool cameraMode = false;
    float nominalViewerZ = 0.5f;  // Cached from runtime for camera-mode init

    // Eye tracking mode toggle (T key)
    bool eyeTrackingModeToggleRequested = false;

    // 'I' key: snapshot the **app's projection-layer atlas only** (cols ×
    // rows × renderW × renderH) to %USERPROFILE%\Pictures\DisplayXR\
    // <app>-<N>_<cols>x<rows>.png. One projection layer; HUD / window-space
    // / quad layers and per-eye disparity are NOT included. Skipped for 1×1
    // (mono) layouts. Filename auto-increments per (cols×rows).
    //
    // For runtime-side captures, use the trigger files (or MCP capture_frame):
    //   `displayxr_atlas_trigger` → post-compose (all layers, what DP weaves)
    //   `displayxr_atlas_trigger.projection` → projection-only (per-tile,
    //                                          no window-space layers)
    // See u_capture_intent.h and issue #210.
    bool captureAtlasRequested = false;

    // --- Gaussian-splat demo extensions (additive; unused by cube_* apps) ---

    // Smooth display-pose transition (double-click focus). When active,
    // UpdateCameraMovement interpolates pose and rewrites cameraPos + yaw/pitch.
    bool transitioning = false;
    XrPosef transitionFrom = {{0,0,0,1}, {0,0,0}};
    XrPosef transitionTo   = {{0,0,0,1}, {0,0,0}};
    float transitionT = 0.0f;
    float transitionDuration = 0.45f;

    // Auto-orbit (turntable) — idle-timer gated yaw advance.
    bool animateEnabled = false;
    double lastInputTimeSec = 0.0;
    bool animationActive = false;          // derived: (animateEnabled && idle>10s)
    bool animateToggleRequested = false;   // set by 'M' key or UI button

    // File-open / drag-drop queue (app-populated, app-consumed).
    std::string pendingLoadPath;
    bool loadRequested = false;            // app queues; caller shows dialog
};

// Process a Win32 message and update input state
// Returns true if the message was handled
bool UpdateInputState(InputState& state, UINT msg, WPARAM wParam, LPARAM lParam);

// Update camera position based on current key states
// deltaTime is in seconds, displayHeightM is physical display height for m2v scaling
void UpdateCameraMovement(InputState& state, float deltaTime, float displayHeightM = 0.0f);

// Get a string describing current mouse button state
std::string GetMouseButtonString(const InputState& state);
