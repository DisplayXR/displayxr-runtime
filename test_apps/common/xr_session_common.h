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
#include <openxr/XR_EXT_win32_window_binding.h>
#include <openxr/XR_EXT_display_info.h>
#include <DirectXMath.h>
#include <vector>
#include <string>

// [Commented out — will be reused for 3D-positioned HUD later]
// struct ConvergencePlane {
//     XrPosef pose;    // center position + orientation in LOCAL space
//     float width;     // meters
//     float height;    // meters
//     bool valid;
// };

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
    XrSpace localSpace = XR_NULL_HANDLE;   //!< LOCAL space: logically anchored, may be recentered
    XrSpace viewSpace = XR_NULL_HANDLE;    //!< VIEW space: head-relative, moves with the viewer
    XrSpace displaySpace = XR_NULL_HANDLE; //!< DISPLAY space (XR_EXT_display_info v2): physically
                                           //!< anchored to display center, unaffected by recentering.
                                           //!< XR_NULL_HANDLE if extension not enabled.

    // Single swapchain at native display resolution.
    // Stereo: both eyes render SBS with viewport offsets.
    // Mono: full window resolution.
    SwapchainInfo swapchain;

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
    bool hasWin32WindowBindingExt = false;
    bool hasDisplayInfoExt = false;

    // Display info from XR_EXT_display_info (static display properties)
    float recommendedViewScaleX = 1.0f;
    float recommendedViewScaleY = 1.0f;
    float displayWidthM = 0.0f;
    float displayHeightM = 0.0f;
    float nominalViewerX = 0.0f;
    float nominalViewerY = 0.0f;
    float nominalViewerZ = 0.5f;

    // Native display pixel dimensions (XR_EXT_display_info v5)
    uint32_t displayPixelWidth = 0;
    uint32_t displayPixelHeight = 0;

    // Display mode switching (XR_EXT_display_info v4)
    bool supportsDisplayModeSwitch = false;
    PFN_xrRequestDisplayModeEXT pfnRequestDisplayModeEXT = nullptr;

    // Window handle for session target (used by ext app, ignored by non-ext app)
    HWND windowHandle = nullptr;

    // HUD window-space layer swapchain
    SwapchainInfo hudSwapchain;
    bool hasHudSwapchain = false;

    // Eye tracking data — raw per-eye positions in display space
    float leftEyeX = 0.0f, leftEyeY = 0.0f, leftEyeZ = 0.0f;
    float rightEyeX = 0.0f, rightEyeY = 0.0f, rightEyeZ = 0.0f;
    bool eyeTrackingActive = false;

    // Frame timing
    XrTime predictedDisplayTime = 0;
    XrDuration predictedDisplayPeriod = 0;
};

// Create reference spaces
bool CreateSpaces(XrSessionManager& xr);

// Create a single SBS swapchain.
// Ext apps (win32_window_binding): native display resolution from XR_EXT_display_info.
// Non-ext / unmodified apps: recommendedImageRectWidth*2 x recommendedImageRectHeight.
bool CreateSwapchain(XrSessionManager& xr);

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
    float playerPitch = 0.0f,
    float zoomScale = 1.0f
);

// Acquire swapchain image for rendering
bool AcquireSwapchainImage(XrSessionManager& xr, uint32_t& imageIndex);

// Release swapchain image after rendering
bool ReleaseSwapchainImage(XrSessionManager& xr);

// App-side Kooima asymmetric frustum projection (RAW mode, app-owned camera model).
// Builds a projection matrix directly from eye position and physical screen extents,
// bypassing the atan/tan roundtrip through XrFovf. The screen dimensions should be
// the *effective* physical extent of the rendered area (meters), computed from the
// display's physical size scaled by renderSize / swapchainSize.
// Reference: Robert Kooima, "Generalized Perspective Projection" (2009).
DirectX::XMMATRIX ComputeKooimaProjection(
    const XrVector3f& eyePos,
    float screenWidthM, float screenHeightM,
    float nearZ, float farZ);

// Compute FOV angles (XrFovf) from eye position and physical screen extents.
// Same geometry as ComputeKooimaProjection but returns atan-based angles for
// layer submission via projectionViews[].fov.
XrFovf ComputeKooimaFov(
    const XrVector3f& eyePos,
    float screenWidthM, float screenHeightM);

// End frame and submit layers (projection layer only)
// viewCount defaults to 2 (stereo); pass 1 for mono submission in 2D mode
bool EndFrame(XrSessionManager& xr, XrTime displayTime, const XrCompositionLayerProjectionView* views, uint32_t viewCount = 2);

// Create a HUD swapchain for window-space layer submission
bool CreateHudSwapchain(XrSessionManager& xr, uint32_t width, uint32_t height);

// Acquire/release HUD swapchain image
bool AcquireHudSwapchainImage(XrSessionManager& xr, uint32_t& imageIndex);
bool ReleaseHudSwapchainImage(XrSessionManager& xr);

// End frame with both projection layer and window-space HUD layer
// viewCount defaults to 2 (stereo); pass 1 for mono submission in 2D mode
bool EndFrameWithWindowSpaceHud(
    XrSessionManager& xr,
    XrTime displayTime,
    const XrCompositionLayerProjectionView* projViews,
    float hudX, float hudY, float hudWidth, float hudHeight,
    float hudDisparity,
    uint32_t viewCount = 2
);

// [Commented out — will be reused for 3D-positioned HUD later]
// ConvergencePlane LocateConvergencePlane(const XrView views[2]);
// XrPosef ComputeHUDPose(
//     const ConvergencePlane& plane,
//     float coverageFraction,
//     const XrView views[2],
//     float& outWidth, float& outHeight);
// bool EndFrameWithQuadLayer(
//     XrSessionManager& xr,
//     XrTime displayTime,
//     const XrCompositionLayerProjectionView* projViews,
//     const XrPosef& quadPose,
//     float quadWidth, float quadHeight
// );

// Clean up OpenXR resources
void CleanupOpenXR(XrSessionManager& xr);

// Get session state as string for display
const char* GetSessionStateString(XrSessionState state);

// Request session exit
void RequestExit(XrSessionManager& xr);
