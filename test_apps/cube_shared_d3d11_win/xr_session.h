// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management with shared D3D11 texture
 *
 * This version uses XR_EXT_win32_window_binding with sharedTextureHandle
 * for offscreen shared texture compositing. The app's HWND is passed
 * for weaver position tracking (interlacing alignment).
 */

#pragma once

#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"

// Initialize OpenXR instance and check for XR_EXT_win32_window_binding support
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D11 graphics requirements (adapter LUID)
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D11 device, shared texture handle, and app window (for position tracking)
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HANDLE sharedTextureHandle, HWND appHwnd);
