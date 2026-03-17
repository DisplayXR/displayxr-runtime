// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for D3D12 with XR_EXT_win32_window_binding
 */

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#define XR_USE_GRAPHICS_API_D3D12
#include "xr_session_common.h"

// Initialize OpenXR instance with D3D12 + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get D3D12 graphics requirements (adapter LUID + min feature level)
bool GetD3D12GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D12 device and window handle
bool CreateSession(XrSessionManager& xr, ID3D12Device* device, ID3D12CommandQueue* queue, HWND hwnd);
