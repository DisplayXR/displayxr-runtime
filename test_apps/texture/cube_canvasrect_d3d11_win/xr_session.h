// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the XR_DXR_canvas_rect (#697) exerciser
 *
 * Cloned from cube_zones_texture_d3d11_win. This is the WINDOWLESS-producer
 * demo: like the zones-texture app it hands the runtime a shared D3D11 texture
 * HANDLE (the runtime composites/weaves into it and the app presents it), but
 * it passes NO HWND. Instead it declares its on-panel canvas rect explicitly —
 * display-relative device px — via XrCanvasRectBindingDXR at create and
 * xrSetCanvasRectDXR on window move. So canvas-scoped Kooima + weave phase come
 * from the declared rect, not a bound window (issue #697). The shared texture is
 * still carried on XrWin32WindowBindingCreateInfoDXR with windowHandle = NULL
 * (that struct is the texture-handoff channel; canvas_rect is the position
 * channel — the two roles the HWND used to fuse). Zones remain available under
 * DISPLAYXR_ZONES=1 but are off by default here.
 */

#pragma once

#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"
#include <openxr/XR_DXR_local_3d_zone.h>
#include <openxr/XR_DXR_display_zones.h>
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_canvas_rect.h>

// XR_DXR_canvas_rect (#697) available + enabled on the instance, and the
// resolved live setter. The app declares its on-panel rect through these
// instead of binding an HWND.
extern bool g_hasCanvasRectExt;
extern PFN_xrSetCanvasRectDXR g_pfnSetCanvasRect;

// Compute the window's client area as a DISPLAY-RELATIVE device-px rect
// (origin = panel top-left = g_displayScreenLeft/Top). Returns false if the
// HWND has no client area yet. This is exactly what a windowless producer must
// hand the runtime in place of a bound window.
bool ComputeCanvasRectPx(HWND hwnd, XrRect2Di* outRect);

// Push the current canvas rect to the runtime (call on window move/resize).
// No-op if the extension/setter is unavailable.
void PushCanvasRect(XrSessionManager& xr, HWND hwnd);

// INV-1.3 (#715): 3D panel top-left in virtual-desktop pixels (top-down,
// origin = primary top-left); (0,0) = primary/unknown. Filled by
// InitializeOpenXR from the XrDisplayDesktopPositionDXR chain (spec v16).
extern int32_t g_displayScreenLeft;
extern int32_t g_displayScreenTop;

// XR_DXR_view_rig available + enabled on the instance.
extern bool g_hasViewRigExt;

// XR_DXR_local_3d_zone harness (mask handle + entry points). The zones app
// uses the mask as the per-frame wish referenced from the xrEndFrame chain
// (XrDisplayZonesFrameEndInfoDXR) — NOT via the sticky xrSubmitLocal3DZoneDXR
// channel, which is inert in zones frames. pfnAcquire is the Tier-3 freeform
// render-target entry (optional; wish mode 2 is skipped when unresolved).
struct ZoneMaskHarness {
    bool available = false;
    PFN_xrCreateLocal3DZoneMaskDXR pfnCreate = nullptr;
    PFN_xrSetLocal3DZoneFromRectsDXR pfnSetRects = nullptr;
    PFN_xrAcquireLocal3DZoneRenderTargetDXR pfnAcquire = nullptr;
    PFN_xrSubmitLocal3DZoneDXR pfnSubmit = nullptr;
    PFN_xrDestroyLocal3DZoneMaskDXR pfnDestroy = nullptr;
    XrLocal3DZoneMaskDXR mask = XR_NULL_HANDLE;
};
extern ZoneMaskHarness g_zone;

// XR_DXR_display_zones (ADR-027) available + enabled on the instance. Only
// true when local_3d_zone + view_rig were also enabled (the extension
// requires both). The runtime advertises it under the DISPLAYXR_ZONES=1 dev
// gate (P2) — when absent the app logs an error once and runs the plain
// single-projection fallback.
extern bool g_hasDisplayZonesExt;

struct DisplayZonesHarness {
    PFN_xrGetDisplayZoneCapabilitiesDXR pfnGetCaps = nullptr;
    PFN_xrGetDisplayZoneRecommendedViewSizeDXR pfnGetViewSize = nullptr;
};
extern DisplayZonesHarness g_zones;

// Initialize OpenXR instance and detect/enable extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D11 graphics requirements (adapter LUID)
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create session with D3D11 device + the app's shared texture HANDLE (the
// runtime's composite target — the texture-mode marker, carried on
// XrWin32WindowBindingCreateInfoDXR with windowHandle = NULL). @p appHwnd is
// NOT passed to the runtime; it is used only to compute the initial
// display-relative canvas rect declared via XrCanvasRectBindingDXR (#697).
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device,
                   HANDLE sharedTextureHandle, HWND appHwnd);
