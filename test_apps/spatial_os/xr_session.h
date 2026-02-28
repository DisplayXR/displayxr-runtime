// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for Spatial OS demo
 *
 * Extends xr_session_common with multi-swapchain panel support.
 */

#pragma once

#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"
#include "panel.h"

// Initialize OpenXR instance and check for extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D11 graphics requirements (adapter LUID)
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D11 device and window handle
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HWND hwnd);

// Create a panel swapchain (window-space layer) at given pixel dimensions
bool CreatePanelSwapchain(XrSessionManager& xr, Panel& panel, uint32_t pixelW, uint32_t pixelH);

// Acquire/release a panel's swapchain image
bool AcquirePanelSwapchainImage(Panel& panel, uint32_t& imageIndex);
bool ReleasePanelSwapchainImage(Panel& panel);

// End frame with projection layer + multiple window-space panel layers
bool EndFrameMultiLayer(
    XrSessionManager& xr,
    XrTime displayTime,
    const XrCompositionLayerProjectionView* projViews,
    uint32_t viewCount,
    Panel* panels,
    int panelCount
);
