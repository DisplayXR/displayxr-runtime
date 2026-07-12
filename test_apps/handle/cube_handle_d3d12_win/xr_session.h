// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for D3D12 with XR_DXR_win32_window_binding
 */

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#define XR_USE_GRAPHICS_API_D3D12
#include "xr_session_common.h"
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_local_3d_zone.h>

// INV-1.3 (#715): 3D panel top-left in virtual-desktop pixels (top-down,
// origin = primary top-left); (0,0) = primary/unknown. Filled by
// InitializeOpenXR from the XrDisplayDesktopPositionDXR chain (spec v16).
extern int32_t g_displayScreenLeft;
extern int32_t g_displayScreenTop;

// XR_DXR_view_rig (#396 W7) available + enabled on the instance. App-local
// (not on the shared XrSessionManager); promote to xr_session_common when
// more consumers adopt it.
extern bool g_hasViewRigExt;

// #439 Phase 3 — XR_DXR_local_3d_zone harness (header v3 carries the Local2D
// composition-layer + view-size-changed types). App-local for the same reason
// as the view-rig flag: the shared XrSessionManager doesn't carry this
// extension yet. Populated by InitializeOpenXR; drives the handle-app panel
// modes (DXR_LOCAL2D_PANEL / +DXR_LOCAL2D_MASK / +DXR_LOCAL2D_PANEL2).
struct ZoneMaskHarness {
    bool available = false;
    PFN_xrCreateLocal3DZoneMaskDXR pfnCreate = nullptr;
    PFN_xrSetLocal3DZoneFromRectsDXR pfnSetRects = nullptr;
    PFN_xrSubmitLocal3DZoneDXR pfnSubmit = nullptr;
    PFN_xrDestroyLocal3DZoneMaskDXR pfnDestroy = nullptr;
    XrLocal3DZoneMaskDXR mask = XR_NULL_HANDLE;
};
extern ZoneMaskHarness g_zone;

// Initialize OpenXR instance with D3D12 + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get D3D12 graphics requirements (adapter LUID + min feature level)
bool GetD3D12GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D12 device and window handle
bool CreateSession(XrSessionManager& xr, ID3D12Device* device, ID3D12CommandQueue* queue, HWND hwnd);
