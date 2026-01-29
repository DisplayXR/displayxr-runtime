// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common OpenXR session management - shared struct and functions
 *
 * This header defines the XrSessionManager struct (superset of all fields)
 * and declares all shared OpenXR session functions used by all test apps.
 * Each app provides its own InitializeOpenXR() and CreateSession().
 *
 * This header is graphics-API-agnostic: it does NOT include any D3D11/D3D12/GL/VK
 * headers. Each app enumerates API-specific swapchain images in its own code
 * after calling CreateSwapchains().
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <unknwn.h>  // IUnknown - needed by openxr_platform.h MSFT extensions

// Only define platform — no graphics API here (apps define their own)
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_session_target.h>
#include <DirectXMath.h>
#include <vector>
#include <string>

struct ConvergencePlane {
    XrPosef pose;    // center position + orientation in LOCAL space
    float width;     // meters
    float height;    // meters
    bool valid;
};

struct SwapchainInfo {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t imageCount = 0;
};

struct XrSessionManager {
    // OpenXR handles
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;

    // Swapchains (one per eye for stereo)
    SwapchainInfo swapchains[2];

    // Quad layer swapchain for UI overlay
    SwapchainInfo quadSwapchain;
    bool hasQuadLayer = false;

    // View configuration
    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews;

    // Session state
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;

    // Extension support (used by ext app, ignored by non-ext app)
    bool hasSessionTargetExt = false;

    // Window handle for session target (used by ext app, ignored by non-ext app)
    HWND windowHandle = nullptr;

    // Eye tracking data (from views)
    float eyePosX = 0.0f;
    float eyePosY = 0.0f;
    float eyePosZ = 0.0f;
    bool eyeTrackingActive = false;

    // Frame timing
    XrTime predictedDisplayTime = 0;
    XrDuration predictedDisplayPeriod = 0;
};

// Create reference spaces
bool CreateSpaces(XrSessionManager& xr);

// Create swapchains for rendering
bool CreateSwapchains(XrSessionManager& xr);

// Create quad layer swapchain for UI overlay
bool CreateQuadLayerSwapchain(XrSessionManager& xr, uint32_t width, uint32_t height);

// Acquire/release quad layer swapchain image
bool AcquireQuadSwapchainImage(XrSessionManager& xr, uint32_t& imageIndex);
bool ReleaseQuadSwapchainImage(XrSessionManager& xr);

// Poll for OpenXR events and update session state
bool PollEvents(XrSessionManager& xr);

// Begin a frame - returns true if should render
bool BeginFrame(XrSessionManager& xr, XrFrameState& frameState);

// Get view poses for rendering.
// The player transform (position + yaw/pitch) is applied to all XR poses before building
// view matrices. This is the production-engine pattern for first-person locomotion:
// the reference space stays fixed at the tracking origin, and a player transform is
// applied to every pose from OpenXR (views, eye tracking, and future controller/hand poses).
// This matches how Unity/Unreal handle XR locomotion internally.
bool LocateViews(
    XrSessionManager& xr,
    XrTime displayTime,
    DirectX::XMMATRIX& leftViewMatrix,
    DirectX::XMMATRIX& leftProjMatrix,
    DirectX::XMMATRIX& rightViewMatrix,
    DirectX::XMMATRIX& rightProjMatrix,
    float playerPosX = 0.0f,
    float playerPosY = 0.0f,
    float playerPosZ = 0.0f,
    float playerYaw = 0.0f,
    float playerPitch = 0.0f
);

// Acquire swapchain image for rendering
bool AcquireSwapchainImage(XrSessionManager& xr, int eye, uint32_t& imageIndex);

// Release swapchain image after rendering
bool ReleaseSwapchainImage(XrSessionManager& xr, int eye);

// End frame and submit layers (projection layer only)
bool EndFrame(XrSessionManager& xr, XrTime displayTime, const XrCompositionLayerProjectionView* views);

// Compute the convergence plane (virtual display surface) from two stereo XrViews.
// The convergence plane is derived from the intersection of the asymmetric frustums
// of the two eyes, representing the physical display surface in LOCAL space.
ConvergencePlane LocateConvergencePlane(const XrView views[2]);

// Compute the HUD quad pose anchored to the top-left region of the convergence plane.
// coverageFraction controls how much of the display the HUD covers (0.2 = 20%).
// viewMidpoint is the eye midpoint pose used to transform the result into VIEW space.
// Returns the pose in VIEW space and writes the HUD dimensions to outWidth/outHeight.
XrPosef ComputeHUDPose(
    const ConvergencePlane& plane,
    float coverageFraction,
    const XrView views[2],
    float& outWidth, float& outHeight);

// End frame with projection layer and quad layer for UI
bool EndFrameWithQuadLayer(
    XrSessionManager& xr,
    XrTime displayTime,
    const XrCompositionLayerProjectionView* projViews,
    const XrPosef& quadPose,
    float quadWidth, float quadHeight
);

// Clean up OpenXR resources
void CleanupOpenXR(XrSessionManager& xr);

// Get session state as string for display
const char* GetSessionStateString(XrSessionState state);

// Request session exit
void RequestExit(XrSessionManager& xr);
