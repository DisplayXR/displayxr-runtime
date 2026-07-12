// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the XR_DXR_display_zones TEXTURE
 *         exerciser — OpenGL leg.
 *
 * Cloned from cube_zones_texture_vk_win (texture-class D3D11/DComp machinery) +
 * cube_zones_gl_win (GL render + zones logic) for #613.
 *
 *  - The OpenXR graphics binding is OPENGL (XrGraphicsBindingOpenGLWin32KHR) —
 *    the zone cubes render into OpenGL OpenXR swapchains, exactly like the GL
 *    handle/zones app.
 *  - The shared composite target the runtime writes back into is a D3D11 KMT
 *    texture (BGRA). TEXTURE MODE: the app passes the D3D11 texture HANDLE
 *    (the texture-mode marker) + the app HWND via XR_DXR_win32_window_binding,
 *    chained on the GL graphics binding. The runtime's GL native compositor
 *    bridges that D3D11 KMT handle into a GL texture via WGL_NV_DX_interop2
 *    (wglDXOpenDeviceNV / wglDXRegisterObjectNV + per-frame
 *    wglDXLockObjectsNV/UnlockObjectsNV) and composites the app's GL zone
 *    swapchains INTO it — see comp_gl_compositor.cpp (has_shared_texture
 *    branch). The APP does NOT touch WGL interop: the runtime owns it.
 *
 * Unlike the VK leg there is NO adapter-LUID match step: GL has no
 * runtime-dictated physical device, so the app simply creates a D3D11 device
 * (the runtime opens the shared KMT handle on the GL driver's device). The
 * shared texture is created on a plain hardware D3D11 device (main.cpp).
 *
 * The display-zones detection + entry points are unchanged from the GL zones
 * leg (XR_DXR_display_zones composes XR_DXR_local_3d_zone + XR_DXR_view_rig).
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
// exactly like the Vulkan zones apps.
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

// Create the OpenXR session with XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR
// chained to XrWin32WindowBindingCreateInfoDXR carrying the app's D3D11
// shared-texture HANDLE (the runtime's composite target — the texture-mode
// marker) + the app HWND (weaver position tracking) + transparentBackgroundEnabled.
bool CreateSession(XrSessionManager& xr, HDC hDC, HGLRC hGLRC,
    HANDLE sharedTextureHandle, HWND hwnd);
