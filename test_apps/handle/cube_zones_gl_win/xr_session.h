// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the XR_DXR_display_zones exerciser (GL)
 *
 * Cloned from cube_handle_gl_win (GL session/binding) + cube_zones_d3d11_win
 * (zones detection harness) for #613. The zones extension composes
 * XR_DXR_local_3d_zone (the wish-mask tiers + the Local2D layer) and
 * XR_DXR_view_rig (per-zone framing), so it is enabled only when both
 * prerequisites are also available.
 */

#pragma once

#define XR_USE_GRAPHICS_API_OPENGL
#include "xr_session_common.h"
#include <GL/gl.h>
#include <openxr/XR_DXR_local_3d_zone.h>
#include <openxr/XR_DXR_display_zones.h>
#include <openxr/XR_DXR_view_rig.h>

// INV-1.3 (#715): 3D panel top-left in virtual-desktop pixels (top-down,
// origin = primary top-left); (0,0) = primary/unknown. Filled by
// InitializeOpenXR from the XrDisplayDesktopPositionDXR chain (spec v16).
extern int32_t g_displayScreenLeft;
extern int32_t g_displayScreenTop;

// XR_DXR_view_rig available + enabled on the instance.
extern bool g_hasViewRigExt;

// XR_DXR_local_3d_zone harness (mask handle + entry points). The zones app uses
// the mask as the per-frame wish referenced from the xrEndFrame chain
// (XrDisplayZonesFrameEndInfoDXR) — NOT via the sticky xrSubmitLocal3DZoneDXR
// channel.
//
// GL Tier-3 NOTE: there is NO OpenGL render-target binding struct in
// XR_DXR_local_3d_zone (only D3D11/D3D12/Vulkan). So pfnAcquire is intentionally
// left NULL on GL — wish mode 2 (Tier-3 freeform render target) is impractical
// to wire and is stubbed: the M-key cycle skips mode 2 and falls back to AUTO,
// exactly like the Vulkan zones app.
struct ZoneMaskHarness {
    bool available = false;
    PFN_xrCreateLocal3DZoneMaskDXR pfnCreate = nullptr;
    PFN_xrSetLocal3DZoneFromRectsDXR pfnSetRects = nullptr;
    PFN_xrAcquireLocal3DZoneRenderTargetDXR pfnAcquire = nullptr; // unused on GL (no GL binding)
    PFN_xrSubmitLocal3DZoneDXR pfnSubmit = nullptr;
    PFN_xrDestroyLocal3DZoneMaskDXR pfnDestroy = nullptr;
    XrLocal3DZoneMaskDXR mask = XR_NULL_HANDLE;
};
extern ZoneMaskHarness g_zone;

// XR_DXR_display_zones (ADR-027) available + enabled on the instance. Only true
// when local_3d_zone + view_rig were also enabled (the extension requires
// both). The runtime advertises it under the DISPLAYXR_ZONES=1 dev gate — when
// absent the app logs an error once and runs the plain single-projection
// fallback.
extern bool g_hasDisplayZonesExt;

struct DisplayZonesHarness {
    PFN_xrGetDisplayZoneCapabilitiesDXR pfnGetCaps = nullptr;
    PFN_xrGetDisplayZoneRecommendedViewSizeDXR pfnGetViewSize = nullptr;
};
extern DisplayZonesHarness g_zones;

// Initialize OpenXR instance and detect/enable extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get OpenGL graphics requirements (min/max GL version)
bool GetOpenGLGraphicsRequirements(XrSessionManager& xr);

// Create session with OpenGL context and window handle (XR_DXR_win32_window_binding)
bool CreateSession(XrSessionManager& xr, HDC hDC, HGLRC hGLRC, HWND hwnd);
