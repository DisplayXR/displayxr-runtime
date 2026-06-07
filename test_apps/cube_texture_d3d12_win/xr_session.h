// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for D3D12 with shared texture
 *
 * This version uses XR_EXT_win32_window_binding with sharedTextureHandle
 * for offscreen shared texture compositing. The app's HWND is passed
 * for weaver position tracking (interlacing alignment).
 */

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#define XR_USE_GRAPHICS_API_D3D12
#include "xr_session_common.h"
#include <openxr/XR_EXT_local_3d_zone.h>

// #439 — XR_EXT_local_3d_zone test harness state (D3D12 port of the
// cube_texture_d3d11_win Phase-1 harness). App-local: the shared
// XrSessionManager lives in displayxr-common, which doesn't carry this
// extension yet. Populated by InitializeOpenXR, driven by the 'Z' key cycle
// in main.cpp.
struct ZoneMaskHarness {
    bool available = false;
    PFN_xrGetLocal3DZoneCapabilitiesEXT pfnGetCaps = nullptr;
    PFN_xrCreateLocal3DZoneMaskEXT pfnCreate = nullptr;
    PFN_xrSetLocal3DZoneWholeWindowEXT pfnSetWhole = nullptr;
    PFN_xrSetLocal3DZoneFromRectsEXT pfnSetRects = nullptr;
    PFN_xrAcquireLocal3DZoneRenderTargetEXT pfnAcquireRT = nullptr;
    PFN_xrSubmitLocal3DZoneEXT pfnSubmit = nullptr;
    PFN_xrDestroyLocal3DZoneMaskEXT pfnDestroy = nullptr;
    XrLocal3DZoneMaskEXT mask = XR_NULL_HANDLE;
    // 0 = no mask (rect-surround behavior), 1 = Tier-1 whole-window 3D,
    // 2 = Tier-2 single rect == canvas, 3 = Tier-2 multi-rect islands,
    // 4 = Tier-3 freeform radial gradient.
    int state = 0;
};
extern ZoneMaskHarness g_zone;

// Initialize OpenXR instance with D3D12 + win32_window_binding (mandatory) extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get D3D12 graphics requirements (adapter LUID + min feature level)
bool GetD3D12GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D12 device/queue, shared texture handle, and app window (for position tracking)
bool CreateSession(XrSessionManager& xr, ID3D12Device* device, ID3D12CommandQueue* queue,
                   HANDLE sharedTextureHandle, HWND appHwnd);
