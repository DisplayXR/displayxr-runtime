// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management (standard mode, no win32_window_binding extension)
 *
 * This version does NOT use the XR_DXR_win32_window_binding extension.
 * OpenXR/DisplayXR will create its own window for rendering.
 */

#pragma once

#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"
#include "dxr_stylus.h"

// XR_LOGITECH_mx_ink_stylus_interaction (ADR-034 Amendment 8): set
// g_stylusRequested before InitializeOpenXR (`--stylus`); the extension is
// enabled only then, and g_hasMxInkExt says whether it was. The OpenXR half
// of the mode lives in test_apps/common/dxr_stylus.h.
extern bool g_stylusRequested;
extern bool g_hasMxInkExt;

// Initialize OpenXR instance (D3D11 only, no win32_window_binding extension)
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D11 graphics requirements (adapter LUID)
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D11 device only (no window handle - DisplayXR creates window)
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device);
