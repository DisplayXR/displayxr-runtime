// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the XR_EXT_display_zones exerciser —
 *         native D3D12 HANDLE leg (array / single-pass-instanced stereo).
 *
 * Cloned from cube_handle_d3d12_win (a working D3D12 HANDLE app: the runtime
 * owns presentation via XR_EXT_win32_window_binding) and extended with
 * XR_EXT_display_zones (ADR-027) detection + entry points, mirroring the D3D11
 * handle-class zones reference cube_zones_d3d11_win. The zones extension
 * composes XR_EXT_local_3d_zone (the wish-mask tiers) and XR_EXT_view_rig
 * (per-zone framing), so it is enabled only when both prerequisites are also
 * available.
 */

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#define XR_USE_GRAPHICS_API_D3D12
#include "xr_session_common.h"
#include <openxr/XR_EXT_view_rig.h>
#include <openxr/XR_EXT_local_3d_zone.h>
#include <openxr/XR_EXT_display_zones.h>

// XR_EXT_view_rig (#396 W7) available + enabled on the instance. App-local
// (not on the shared XrSessionManager); promote to xr_session_common when
// more consumers adopt it.
extern bool g_hasViewRigExt;

// XR_EXT_local_3d_zone harness (mask handle + entry points). Prerequisite for
// XR_EXT_display_zones; the zones path here uses AUTO wish (wishMask = NULL,
// runtime auto-derives), so the mask entry points are resolved but the mask is
// left unused — enabling the extension is what the runtime gates on.
struct ZoneMaskHarness {
    bool available = false;
    PFN_xrCreateLocal3DZoneMaskEXT pfnCreate = nullptr;
    PFN_xrSetLocal3DZoneFromRectsEXT pfnSetRects = nullptr;
    PFN_xrSubmitLocal3DZoneEXT pfnSubmit = nullptr;
    PFN_xrDestroyLocal3DZoneMaskEXT pfnDestroy = nullptr;
    XrLocal3DZoneMaskEXT mask = XR_NULL_HANDLE;
};
extern ZoneMaskHarness g_zone;

// XR_EXT_display_zones (ADR-027) available + enabled on the instance. Only true
// when local_3d_zone + view_rig were also enabled (the extension requires
// both). The runtime advertises it under the DISPLAYXR_ZONES=1 dev gate (P2) —
// when absent the app logs an error once and submits empty frames (graceful
// degrade).
extern bool g_hasDisplayZonesExt;

struct DisplayZonesHarness {
    PFN_xrGetDisplayZoneCapabilitiesEXT pfnGetCaps = nullptr;
    PFN_xrGetDisplayZoneRecommendedViewSizeEXT pfnGetViewSize = nullptr;
};
extern DisplayZonesHarness g_zones;

// Initialize OpenXR instance with D3D12 + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get D3D12 graphics requirements (adapter LUID + min feature level)
bool GetD3D12GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D12 device and window handle
bool CreateSession(XrSessionManager& xr, ID3D12Device* device, ID3D12CommandQueue* queue, HWND hwnd);
