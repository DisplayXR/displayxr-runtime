// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the XR_EXT_display_zones exerciser
 *
 * Cloned from cube_handle_d3d11_win and extended with XR_EXT_display_zones
 * (ADR-027) detection + entry points. The zones extension composes
 * XR_EXT_local_3d_zone (the wish-mask tiers + the Local2D layer) and
 * XR_EXT_view_rig (per-zone framing), so it is enabled only when both
 * prerequisites are also available.
 */

#pragma once

#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"
#include <openxr/XR_EXT_local_3d_zone.h>
#include <openxr/XR_EXT_display_zones.h>
#include <openxr/XR_EXT_view_rig.h>

// INV-1.3 (#715): 3D panel top-left in virtual-desktop pixels (top-down,
// origin = primary top-left); (0,0) = primary/unknown. Filled by
// InitializeOpenXR from the XrDisplayDesktopPositionEXT chain (spec v16).
extern int32_t g_displayScreenLeft;
extern int32_t g_displayScreenTop;

// XR_EXT_view_rig available + enabled on the instance.
extern bool g_hasViewRigExt;

// XR_EXT_local_3d_zone harness (mask handle + entry points). The zones app
// uses the mask as the per-frame wish referenced from the xrEndFrame chain
// (XrDisplayZonesFrameEndInfoEXT) — NOT via the sticky xrSubmitLocal3DZoneEXT
// channel, which is inert in zones frames. pfnAcquire is the Tier-3 freeform
// render-target entry (optional; wish mode 2 is skipped when unresolved).
struct ZoneMaskHarness {
    bool available = false;
    PFN_xrCreateLocal3DZoneMaskEXT pfnCreate = nullptr;
    PFN_xrSetLocal3DZoneFromRectsEXT pfnSetRects = nullptr;
    PFN_xrAcquireLocal3DZoneRenderTargetEXT pfnAcquire = nullptr;
    PFN_xrSubmitLocal3DZoneEXT pfnSubmit = nullptr;
    PFN_xrDestroyLocal3DZoneMaskEXT pfnDestroy = nullptr;
    XrLocal3DZoneMaskEXT mask = XR_NULL_HANDLE;
};
extern ZoneMaskHarness g_zone;

// XR_EXT_display_zones (ADR-027) available + enabled on the instance. Only
// true when local_3d_zone + view_rig were also enabled (the extension
// requires both). The runtime advertises it under the DISPLAYXR_ZONES=1 dev
// gate (P2) — when absent the app logs an error once and runs the plain
// single-projection fallback.
extern bool g_hasDisplayZonesExt;

struct DisplayZonesHarness {
    PFN_xrGetDisplayZoneCapabilitiesEXT pfnGetCaps = nullptr;
    PFN_xrGetDisplayZoneRecommendedViewSizeEXT pfnGetViewSize = nullptr;
};
extern DisplayZonesHarness g_zones;

// Initialize OpenXR instance and detect/enable extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D11 graphics requirements (adapter LUID)
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D11 device and window handle (using XR_EXT_win32_window_binding)
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HWND hwnd);
