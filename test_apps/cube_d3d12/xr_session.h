// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for D3D12 (standard mode, no win32_window_binding)
 *
 * This version does NOT use the XR_EXT_win32_window_binding extension.
 * OpenXR/DisplayXR will create its own window for rendering.
 */

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#define XR_USE_GRAPHICS_API_D3D12
#include "xr_session_common.h"

// Initialize OpenXR instance (D3D12 only, no win32_window_binding extension)
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D12 graphics requirements (adapter LUID)
bool GetD3D12GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D12 device only (no window handle - DisplayXR creates window)
bool CreateSession(XrSessionManager& xr, ID3D12Device* device, ID3D12CommandQueue* queue);
