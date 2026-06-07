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
#include <openxr/XR_EXT_local_3d_zone.h>
#include <openxr/XR_EXT_view_rig.h>

// XR_EXT_view_rig (#396 W7) available + enabled on the instance. App-local
// for the same reason as the zone harness. The texture app chains only the
// RAW result struct — the one-shot log proves canvasRectPx reports the
// canvas SUB-RECT (xrSetSharedTextureOutputRectEXT), not the window client
// area.
extern bool g_hasViewRigExt;

// #439 Phase 1 — XR_EXT_local_3d_zone test harness state. App-local: the
// shared XrSessionManager lives in displayxr-common, which doesn't carry this
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

// Initialize OpenXR instance and check for XR_EXT_win32_window_binding support
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D11 graphics requirements (adapter LUID)
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D11 device, shared texture handle, and app window (for position tracking)
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HANDLE sharedTextureHandle, HWND appHwnd);
