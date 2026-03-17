// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management (standard mode, no win32_window_binding extension)
 *
 * This version does NOT use the XR_EXT_win32_window_binding extension.
 * OpenXR/DisplayXR will create its own window for rendering.
 */

#pragma once

#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"

// Initialize OpenXR instance (D3D11 only, no win32_window_binding extension)
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D11 graphics requirements (adapter LUID)
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D11 device only (no window handle - DisplayXR creates window)
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device);
