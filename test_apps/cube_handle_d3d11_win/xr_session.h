// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management with XR_EXT_win32_window_binding extension
 *
 * This version uses the XR_EXT_win32_window_binding extension to render into
 * an application-controlled window instead of a runtime-created one.
 */

#pragma once

#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"
#include <openxr/XR_EXT_local_3d_zone.h>
#include <openxr/XR_EXT_view_rig.h>

// XR_EXT_view_rig (#396 W7) available + enabled on the instance. App-local
// (not on the shared XrSessionManager) — this app is the extension's verify
// vehicle; promote to xr_session_common when more consumers adopt it.
extern bool g_hasViewRigExt;

// #439 Phase 3 — XR_EXT_local_3d_zone harness (header v3 carries the Local2D
// composition-layer + view-size-changed types). App-local for the same reason
// as the view-rig flag: the shared XrSessionManager doesn't carry this
// extension yet. Populated by InitializeOpenXR; drives the handle-app panel
// modes (DXR_LOCAL2D_PANEL / +DXR_LOCAL2D_MASK / +DXR_LOCAL2D_PANEL2).
struct ZoneMaskHarness {
    bool available = false;
    PFN_xrCreateLocal3DZoneMaskEXT pfnCreate = nullptr;
    PFN_xrSetLocal3DZoneFromRectsEXT pfnSetRects = nullptr;
    PFN_xrSubmitLocal3DZoneEXT pfnSubmit = nullptr;
    PFN_xrDestroyLocal3DZoneMaskEXT pfnDestroy = nullptr;
    XrLocal3DZoneMaskEXT mask = XR_NULL_HANDLE;
};
extern ZoneMaskHarness g_zone;

// Initialize OpenXR instance and check for XR_EXT_win32_window_binding support
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D11 graphics requirements (adapter LUID)
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D11 device and window handle (using XR_EXT_win32_window_binding)
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HWND hwnd);
