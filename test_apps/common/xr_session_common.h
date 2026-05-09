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

#include "view_params.h"

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
    XrSpace localSpace = XR_NULL_HANDLE;   //!< LOCAL space
    XrSpace viewSpace = XR_NULL_HANDLE;    //!< VIEW space: head-relative, moves with the viewer

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

    // System name from xrGetSystemProperties (e.g. "Leia SR", "Sim 3D Display (SBS)")
    char systemName[XR_MAX_SYSTEM_NAME_SIZE] = {};

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
    PFN_xrRequestDisplayModeEXT pfnRequestDisplayModeEXT = nullptr;

    // Eye tracking mode control (XR_EXT_display_info v6)
    uint32_t supportedEyeTrackingModes = 0;   // bitmask: MANAGED_BIT=1, MANUAL_BIT=2
    uint32_t defaultEyeTrackingMode = 0;      // 0=MANAGED, 1=MANUAL
    bool isEyeTracking = false;               // per-frame: true if eyes actively tracked
    uint32_t activeEyeTrackingMode = 0;       // per-frame: currently active mode
    PFN_xrRequestEyeTrackingModeEXT pfnRequestEyeTrackingModeEXT = nullptr;

    // Display rendering mode control (XR_EXT_display_info v7)
    PFN_xrRequestDisplayRenderingModeEXT pfnRequestDisplayRenderingModeEXT = nullptr;
    PFN_xrEnumerateDisplayRenderingModesEXT pfnEnumerateDisplayRenderingModesEXT = nullptr;

    // Canvas sub-rect (XR_EXT_win32_window_binding / XR_EXT_cocoa_window_binding)
    PFN_xrSetSharedTextureOutputRectEXT pfnSetSharedTextureOutputRectEXT = nullptr;

    // Enumerated rendering mode info
    uint32_t currentModeIndex = 1;  // Default: mode 1 (first 3D mode, matches runtime default)
    uint32_t renderingModeCount = 0;
    char renderingModeNames[8][XR_MAX_SYSTEM_NAME_SIZE] = {};
    uint32_t renderingModeViewCounts[8] = {};
    uint32_t renderingModeTileColumns[8] = {};
    uint32_t renderingModeTileRows[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    bool renderingModeDisplay3D[8] = {};

    // Window handle for session target (used by ext app, ignored by non-ext app)
    HWND windowHandle = nullptr;

    // HUD window-space layer swapchain
    SwapchainInfo hudSwapchain;
    bool hasHudSwapchain = false;

    // Per-view data (N-view: up to 8 views, matching XRT_MAX_VIEWS)
    uint32_t viewCount = 2;
    float eyePositions[8][3] = {};       // [view][x,y,z] — raw per-eye positions in display space
    DirectX::XMMATRIX viewMatrices[8];   // per-view view matrices from LocateViews
    DirectX::XMMATRIX projMatrices[8];   // per-view projection matrices from LocateViews
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
    float playerPosX = 0.0f,
    float playerPosY = 0.0f,
    float playerPosZ = 0.0f,
    float playerYaw = 0.0f,
    float playerPitch = 0.0f,
    const ViewParams& viewParams = ViewParams{}
);

// Acquire swapchain image for rendering
bool AcquireSwapchainImage(XrSessionManager& xr, uint32_t& imageIndex);

// Release swapchain image after rendering
bool ReleaseSwapchainImage(XrSessionManager& xr);

// Load column-major float[16] (OpenGL/Vulkan convention) into XMMATRIX.
// XMMATRIX stores rows; column-major m[0..3] = column 0 → becomes row 0.
static inline DirectX::XMMATRIX ColumnMajorToXMMatrix(const float m[16]) {
    DirectX::XMMATRIX out;
    out.r[0] = DirectX::XMVectorSet(m[0],  m[1],  m[2],  m[3]);
    out.r[1] = DirectX::XMVectorSet(m[4],  m[5],  m[6],  m[7]);
    out.r[2] = DirectX::XMVectorSet(m[8],  m[9],  m[10], m[11]);
    out.r[3] = DirectX::XMVectorSet(m[12], m[13], m[14], m[15]);
    return out;
}

// App-side Kooima asymmetric frustum projection (delegates to display3d_view library).
// Wraps display3d_compute_projection() into an XMMATRIX for DirectX apps.
DirectX::XMMATRIX ComputeKooimaProjection(
    const XrVector3f& eyePos,
    float screenWidthM, float screenHeightM,
    float nearZ, float farZ);

// Compute FOV angles (delegates to display3d_compute_fov()).
XrFovf ComputeKooimaFov(
    const XrVector3f& eyePos,
    float screenWidthM, float screenHeightM);

// End frame and submit layers (projection layer only)
// viewCount defaults to 2 (stereo); pass 1 for mono submission in 2D mode.
// projectionLayerFlags is OR-ed onto XrCompositionLayerProjection::layerFlags
// (e.g. XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT for transparent
// backgrounds).
bool EndFrame(XrSessionManager& xr, XrTime displayTime, const XrCompositionLayerProjectionView* views,
              uint32_t viewCount = 2, XrCompositionLayerFlags projectionLayerFlags = 0);

// Create a HUD swapchain for window-space layer submission
bool CreateHudSwapchain(XrSessionManager& xr, uint32_t width, uint32_t height);

// Acquire/release HUD swapchain image
bool AcquireHudSwapchainImage(XrSessionManager& xr, uint32_t& imageIndex);
bool ReleaseHudSwapchainImage(XrSessionManager& xr);

// End frame with both projection layer and window-space HUD layer.
// viewCount defaults to 2 (stereo); pass 1 for mono submission in 2D mode.
// `srcW`/`srcH` < 0 means "use the full HUD swapchain image"; pass a smaller
// rect to display only a sub-region of the texture (e.g. just a top button
// bar) — useful for shrinking the opaque footprint when the body text is
// hidden, since the vk_native compositor blits this layer without alpha
// blending.
bool EndFrameWithWindowSpaceHud(
    XrSessionManager& xr,
    XrTime displayTime,
    const XrCompositionLayerProjectionView* projViews,
    float hudX, float hudY, float hudWidth, float hudHeight,
    float hudDisparity,
    uint32_t viewCount = 2,
    int32_t srcX = 0, int32_t srcY = 0,
    int32_t srcW = -1, int32_t srcH = -1,
    XrCompositionLayerFlags projectionLayerFlags = 0
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
